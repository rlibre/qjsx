/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The USB serial port of the i.MX RT1062 (see usb_serial_imxrt1062.h), over
// NXP's USB device stack (middleware/usb of the MCUXpresso SDK) in bare-metal
// mode: every callback below runs in the USB interrupt.
//
// Descriptors: a CDC ACM device (communication interface 0 with its
// notification endpoint 0x81, data interface 1 with bulk endpoints 0x02 /
// 0x82), PJRC's VID / PID of the Teensy serial port (16C0:0483, agreed
// 2026-11-16), the chip's unique id as serial number. High speed (512-byte
// bulk packets) or full speed (64), patched at each bus reset.
//
// Rings (single producer, single consumer; the indexes are only moved by
// their owner, read with acquire / release order):
//   send     written by write() (under the write lock), emptied by the
//            interrupt: one transfer at a time from a DMA buffer
//   receive  filled by the interrupt, emptied by read(); a packet is armed
//            again only when the ring has room for it (the host waits)
// The buffers the USB DMA reads or writes are in the DTCM (.bss), which has
// no cache.
#include "usb_serial_imxrt1062.h"
#include "loader_imxrt1062.h"
#include "clock_imxrt1062.h"
#include "board.h"
#include "platform.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_cdc_acm.h"
#include "usb_phy.h"
#include "fsl_clock.h"

namespace {

// ### Constants ###############################################################

const uint16_t VENDOR_ID = 0x16C0;                // PJRC (Van Ooijen Technische Informatica)
const uint16_t PRODUCT_ID = 0x0483;               // the Teensy USB serial port
const uint16_t DEVICE_RELEASE = 0x0100;           // bcdDevice
const uint8_t  COMM_INTERFACE = 0;                // CDC communication interface
const uint8_t  DATA_INTERFACE = 1;                // CDC data interface
const uint8_t  NOTIFY_ENDPOINT = 0x81;            // interrupt IN of the communication interface
const uint8_t  DATA_OUT_ENDPOINT = 0x02;          // bulk OUT: host -> board
const uint8_t  DATA_IN_ENDPOINT = 0x82;           // bulk IN: board -> host
const uint8_t  DATA_ENDPOINT_NUMBER = 2;          // both bulk endpoints: what the CDC class's Send / Recv take (not the address)
const uint16_t NOTIFY_PACKET = 16;                // bytes of the notification endpoint
const uint16_t HIGH_SPEED_PACKET = 512;           // bulk packet at high speed
const uint16_t FULL_SPEED_PACKET = 64;            // bulk packet at full speed
const uint8_t  HIGH_SPEED_NOTIFY_INTERVAL = 8;    // 2^(8-1) microframes = 16 ms
const uint8_t  FULL_SPEED_NOTIFY_INTERVAL = 16;   // ms
const uint32_t LOADER_BAUD_RATE = 134;            // asks for the Teensy loader (PJRC)
const uint32_t USB_INTERRUPT_PRIORITY = 6;        // below configMAX_SYSCALL_INTERRUPT_PRIORITY (FreeRTOSConfig.h)

const size_t SEND_RING_SIZE = 8192;               // bytes waiting for the host (a power of two)
const size_t RECEIVE_RING_SIZE = 4096;            // bytes waiting for the reader (a power of two)
const size_t SEND_CHUNK = 2048;                   // bytes of one IN transfer

// USB PHY trimming values of NXP's RT1060 boards
const uint8_t PHY_D_CAL = 0x0C;
const uint8_t PHY_TXCAL45DP = 0x06;
const uint8_t PHY_TXCAL45DM = 0x06;

// ### Descriptors #############################################################

// Device descriptor (USB 2.0, class CDC at device level).
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t deviceDescriptor[] = {
    18, USB_DESCRIPTOR_TYPE_DEVICE,
    0x00, 0x02,                                           // bcdUSB 2.0
    0x02, 0x00, 0x00,                                     // class CDC, no subclass, no protocol
    64,                                                   // control packet
    USB_SHORT_GET_LOW(VENDOR_ID), USB_SHORT_GET_HIGH(VENDOR_ID),
    USB_SHORT_GET_LOW(PRODUCT_ID), USB_SHORT_GET_HIGH(PRODUCT_ID),
    USB_SHORT_GET_LOW(DEVICE_RELEASE), USB_SHORT_GET_HIGH(DEVICE_RELEASE),
    1, 2, 3,                                              // strings: manufacturer, product, serial
    1,                                                    // one configuration
};

// Device qualifier (what the device would be at the other speed).
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t qualifierDescriptor[] = {
    10, USB_DESCRIPTOR_TYPE_DEVICE_QUALITIER,
    0x00, 0x02,
    0x02, 0x00, 0x00,
    64,
    1,
    0,
};

// Offsets in configurationDescriptor of the values that depend on the speed.
const size_t NOTIFY_INTERVAL_OFFSET = 9 + 9 + 5 + 5 + 4 + 5 + 6;
const size_t OUT_PACKET_OFFSET = 9 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 4;
const size_t IN_PACKET_OFFSET = OUT_PACKET_OFFSET + 7;

// Configuration: CDC ACM, two interfaces (67 bytes).
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t configurationDescriptor[] = {
    // configuration: 67 bytes, 2 interfaces, bus powered, 100 mA
    9, USB_DESCRIPTOR_TYPE_CONFIGURE, 67, 0, 2, 1, 0, 0x80, 50,

    // communication interface: CDC, ACM, AT commands, one endpoint
    9, USB_DESCRIPTOR_TYPE_INTERFACE, COMM_INTERFACE, 0, 1, 0x02, 0x02, 0x01, 0,
    // header functional descriptor, CDC 1.10
    5, 0x24, 0x00, 0x10, 0x01,
    // call management: none, data interface 1
    5, 0x24, 0x01, 0x00, DATA_INTERFACE,
    // ACM: line coding + control line state, send break
    4, 0x24, 0x02, 0x06,
    // union: communication 0, data 1
    5, 0x24, 0x06, COMM_INTERFACE, DATA_INTERFACE,
    // notification endpoint (interval patched)
    7, USB_DESCRIPTOR_TYPE_ENDPOINT, NOTIFY_ENDPOINT, USB_ENDPOINT_INTERRUPT,
    USB_SHORT_GET_LOW(NOTIFY_PACKET), USB_SHORT_GET_HIGH(NOTIFY_PACKET), HIGH_SPEED_NOTIFY_INTERVAL,

    // data interface: two bulk endpoints
    9, USB_DESCRIPTOR_TYPE_INTERFACE, DATA_INTERFACE, 0, 2, 0x0A, 0x00, 0x00, 0,
    // bulk OUT, bulk IN (packet sizes patched)
    7, USB_DESCRIPTOR_TYPE_ENDPOINT, DATA_OUT_ENDPOINT, USB_ENDPOINT_BULK,
    USB_SHORT_GET_LOW(HIGH_SPEED_PACKET), USB_SHORT_GET_HIGH(HIGH_SPEED_PACKET), 0,
    7, USB_DESCRIPTOR_TYPE_ENDPOINT, DATA_IN_ENDPOINT, USB_ENDPOINT_BULK,
    USB_SHORT_GET_LOW(HIGH_SPEED_PACKET), USB_SHORT_GET_HIGH(HIGH_SPEED_PACKET), 0,
};

static_assert(sizeof(configurationDescriptor) == 67, "the configuration descriptor announces 67 bytes");

// String 0: the languages (US English).
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
uint8_t languageString[] = { 4, USB_DESCRIPTOR_TYPE_STRING, 0x09, 0x04 };

// Strings 1 and 2 (UTF-16LE, built by start()), 3: the serial number.
const char *const MANUFACTURER = "qjsx";
const char *const PRODUCT = "qjsx";
const size_t MAX_STRING_CHARS = 32;
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) uint8_t manufacturerString[2 + 2 * MAX_STRING_CHARS];
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) uint8_t productString[2 + 2 * MAX_STRING_CHARS];
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) uint8_t serialString[2 + 2 * MAX_STRING_CHARS];

// ### What the class driver needs #############################################

// The endpoints of each interface (sizes and interval patched by applySpeed).
usb_device_endpoint_struct_t notifyEndpoints[] = {
    { NOTIFY_ENDPOINT, USB_ENDPOINT_INTERRUPT, NOTIFY_PACKET, HIGH_SPEED_NOTIFY_INTERVAL },
};
usb_device_endpoint_struct_t dataEndpoints[] = {
    { DATA_IN_ENDPOINT, USB_ENDPOINT_BULK, HIGH_SPEED_PACKET, 0 },
    { DATA_OUT_ENDPOINT, USB_ENDPOINT_BULK, HIGH_SPEED_PACKET, 0 },
};

// The two interfaces of the CDC function, then the function itself.
usb_device_interface_struct_t commInterface[] = {
    { 0, { 1, notifyEndpoints }, nullptr },
};
usb_device_interface_struct_t dataInterface[] = {
    { 0, { 2, dataEndpoints }, nullptr },
};
usb_device_interfaces_struct_t interfaces[] = {
    { 0x02, 0x02, 0x01, COMM_INTERFACE, commInterface, 1 },
    { 0x0A, 0x00, 0x00, DATA_INTERFACE, dataInterface, 1 },
};
usb_device_interface_list_t interfaceLists[] = {
    { 2, interfaces },
};
usb_device_class_struct_t cdcClass = { interfaceLists, kUSB_DeviceClassTypeCdc, 1 };

// The callbacks of the stack (defined below), then the configuration given to
// USB_DeviceClassInit.
usb_status_t onClassEvent(class_handle_t handle, uint32_t event, void *param);
usb_status_t onDeviceEvent(usb_device_handle handle, uint32_t event, void *param);

usb_device_class_config_struct_t classConfigs[] = {
    { onClassEvent, 0, &cdcClass },
};
usb_device_class_config_list_struct_t classConfigList = { classConfigs, onDeviceEvent, 1 };

// ### State ###################################################################

// The line coding the host sets and reads (7 bytes: rate, stop bits,
// parity, data bits).
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) uint8_t lineCoding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };

// DMA buffers of the bulk transfers.
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) uint8_t sendPacket[SEND_CHUNK];
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) uint8_t receivePacket[HIGH_SPEED_PACKET];

uint8_t  sendRing[SEND_RING_SIZE];        // bytes for the host
size_t   sendHead = 0;                    // next byte written (write(), under writeLock)
size_t   sendTail = 0;                    // next byte sent (USB interrupt)
uint8_t  receiveRing[RECEIVE_RING_SIZE];  // bytes from the host
size_t   receiveHead = 0;                 // next byte stored (USB interrupt)
size_t   receiveTail = 0;                 // next byte read (reader task)

usb_device_handle  device = nullptr;      // the stack's device
bool               configured = false;    // the host chose the configuration: the port is up
bool               sending = false;       // an IN transfer is in progress (interrupt and tasks, critical sections)
bool               receiving = false;     // an OUT transfer is armed (interrupt and reader, critical sections)
uint16_t           bulkPacket = HIGH_SPEED_PACKET;   // bulk packet size of the current speed
uint8_t            currentConfiguration = 0;         // what SET_CONFIGURATION chose
SemaphoreHandle_t  receivedSignal = nullptr;         // given by the interrupt when bytes arrive
platform::Mutex    writeLock;                        // held by write() for a whole call

// ### Rings ###################################################################

/**
 * Bytes in a ring of 'size' (a power of two) between tail and head.
 */
size_t ringUsed(size_t head, size_t tail, size_t size) {
    return (head - tail) & (size - 1);
}

/**
 * Room left in such a ring (one byte stays empty: full and empty differ).
 */
size_t ringFree(size_t head, size_t tail, size_t size) {
    return size - 1 - ringUsed(head, tail, size);
}

/**
 * Reads an index moved by the other side.
 */
size_t loadIndex(const size_t *index) {
    return __atomic_load_n(index, __ATOMIC_ACQUIRE);
}

/**
 * Publishes an index for the other side.
 */
void storeIndex(size_t *index, size_t value) {
    __atomic_store_n(index, value, __ATOMIC_RELEASE);
}

/**
 * The class handle of the CDC function (filled by USB_DeviceClassInit).
 */
class_handle_t cdcHandle() {
    return classConfigs[0].classHandle;
}

// ### Sending (interrupt and tasks) ###########################################

/**
 * Starts the next IN transfer if none runs and bytes wait: copies them from
 * the send ring into the DMA buffer. Called with the interrupts masked (a
 * task) or from the USB interrupt.
 */
void startSending() {
    if (!configured || sending) {
        return;
    }

    // what waits, in one piece (up to the end of the ring or SEND_CHUNK)
    size_t head = loadIndex(&sendHead);
    size_t used = ringUsed(head, sendTail, SEND_RING_SIZE);
    if (used == 0) {
        return;
    }

    size_t untilEnd = SEND_RING_SIZE - sendTail;
    size_t chunk = used < untilEnd ? used : untilEnd;
    chunk = chunk < SEND_CHUNK ? chunk : SEND_CHUNK;
    memcpy(sendPacket, sendRing + sendTail, chunk);

    // the transfer; the bytes leave the ring now
    if (USB_DeviceCdcAcmSend(cdcHandle(), DATA_ENDPOINT_NUMBER, sendPacket, (uint32_t)chunk) == kStatus_USB_Success) {
        sending = true;
        storeIndex(&sendTail, (sendTail + chunk) & (SEND_RING_SIZE - 1));
    }
}

/**
 * An IN transfer ended: a zero-length packet closes a transfer of whole
 * packets (else the host waits for more), then the next bytes.
 */
void onSent(const usb_device_endpoint_callback_message_struct_t *message) {
    sending = false;

    // cancelled (bus reset, deconfiguration): nothing more
    bool cancelled = message->length == USB_CANCELLED_TRANSFER_LENGTH;
    if (cancelled || !configured) {
        return;
    }

    // whole packets: the zero-length one
    bool wholePackets = message->length != 0 && (message->length % bulkPacket) == 0;
    if (wholePackets) {
        if (USB_DeviceCdcAcmSend(cdcHandle(), DATA_ENDPOINT_NUMBER, nullptr, 0) == kStatus_USB_Success) {
            sending = true;
        }

        return;
    }

    startSending();
}

// ### Receiving (interrupt and reader) ########################################

/**
 * Arms the next OUT transfer if the receive ring has room for a whole packet
 * (else the reader arms it once it made room). Interrupt, or interrupts
 * masked.
 */
void armReceiving() {
    if (!configured || receiving) {
        return;
    }

    size_t room = ringFree(receiveHead, loadIndex(&receiveTail), RECEIVE_RING_SIZE);
    if (room < bulkPacket) {
        return;
    }

    if (USB_DeviceCdcAcmRecv(cdcHandle(), DATA_ENDPOINT_NUMBER, receivePacket, bulkPacket) == kStatus_USB_Success) {
        receiving = true;
    }
}

/**
 * An OUT transfer ended: its bytes into the receive ring, the reader woken,
 * the next transfer armed.
 */
void onReceived(const usb_device_endpoint_callback_message_struct_t *message) {
    receiving = false;

    // cancelled (bus reset, deconfiguration): nothing more
    bool cancelled = message->length == USB_CANCELLED_TRANSFER_LENGTH;
    if (cancelled || !configured) {
        return;
    }

    // the bytes (armReceiving left room for a whole packet)
    size_t head = receiveHead;

    for (uint32_t i = 0; i < message->length; i++) {
        receiveRing[head] = message->buffer[i];
        head = (head + 1) & (RECEIVE_RING_SIZE - 1);
    }

    storeIndex(&receiveHead, head);

    // wake the reader
    if (message->length > 0) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(receivedSignal, &woken);
        portYIELD_FROM_ISR(woken);
    }

    armReceiving();
}

// ### Speed ###################################################################

/**
 * Sets the packet sizes and the notification interval of the speed the
 * host chose, in the descriptors and in what the class driver opens.
 */
void applySpeed(uint8_t speed) {
    // the packet size and the notification interval of that speed
    bool high = speed == USB_SPEED_HIGH;
    bulkPacket = high ? HIGH_SPEED_PACKET : FULL_SPEED_PACKET;
    uint8_t interval = high ? HIGH_SPEED_NOTIFY_INTERVAL : FULL_SPEED_NOTIFY_INTERVAL;

    // the configuration descriptor
    configurationDescriptor[NOTIFY_INTERVAL_OFFSET] = interval;
    configurationDescriptor[OUT_PACKET_OFFSET] = USB_SHORT_GET_LOW(bulkPacket);
    configurationDescriptor[OUT_PACKET_OFFSET + 1] = USB_SHORT_GET_HIGH(bulkPacket);
    configurationDescriptor[IN_PACKET_OFFSET] = USB_SHORT_GET_LOW(bulkPacket);
    configurationDescriptor[IN_PACKET_OFFSET + 1] = USB_SHORT_GET_HIGH(bulkPacket);

    // the endpoints the class driver opens
    notifyEndpoints[0].interval = interval;
    dataEndpoints[0].maxPacketSize = bulkPacket;
    dataEndpoints[1].maxPacketSize = bulkPacket;
}

// ### Callbacks of the stack (USB interrupt) ##################################

/**
 * Events of the CDC class: transfers done, line coding (134 bauds: the
 * loader), control line state.
 */
usb_status_t onClassEvent(class_handle_t, uint32_t event, void *param) {
    switch (event) {
        case kUSB_DeviceCdcEventSendResponse: {
            onSent((usb_device_endpoint_callback_message_struct_t *)param);
            return kStatus_USB_Success;
        }

        case kUSB_DeviceCdcEventRecvResponse: {
            onReceived((usb_device_endpoint_callback_message_struct_t *)param);
            return kStatus_USB_Success;
        }

        case kUSB_DeviceCdcEventGetLineCoding: {
            usb_device_cdc_acm_request_param_struct_t *request = (usb_device_cdc_acm_request_param_struct_t *)param;
            *request->buffer = lineCoding;
            *request->length = sizeof(lineCoding);
            return kStatus_USB_Success;
        }

        case kUSB_DeviceCdcEventSetLineCoding: {
            // the setup stage gives the buffer, the data stage fills it
            usb_device_cdc_acm_request_param_struct_t *request = (usb_device_cdc_acm_request_param_struct_t *)param;
            if (request->isSetup) {
                *request->buffer = lineCoding;
                *request->length = sizeof(lineCoding);
                return kStatus_USB_Success;
            }

            // the rate: 134 bauds asks for the loader
            uint32_t rate = USB_LONG_FROM_LITTLE_ENDIAN_ADDRESS(lineCoding);
            if (rate == LOADER_BAUD_RATE) {
                loader::enterSoon();
            }

            return kStatus_USB_Success;
        }

        case kUSB_DeviceCdcEventSetControlLineState:
        case kUSB_DeviceCdcEventSendBreak:
        case kUSB_DeviceCdcEventSerialStateNotif: {
            return kStatus_USB_Success;
        }

        default: {
            return kStatus_USB_InvalidRequest;
        }
    }
}

/**
 * Gives a descriptor: 'descriptor' and its length into the request.
 */
usb_status_t giveDescriptor(void *param, uint8_t *descriptor, uint32_t length) {
    usb_device_get_descriptor_common_struct_t *request = (usb_device_get_descriptor_common_struct_t *)param;
    request->buffer = descriptor;
    request->length = length;
    return kStatus_USB_Success;
}

/**
 * The string descriptor 'index', or nullptr.
 */
uint8_t *stringDescriptor(uint8_t index) {
    switch (index) {
        case 0: {
            return languageString;
        }

        case 1: {
            return manufacturerString;
        }

        case 2: {
            return productString;
        }

        case 3: {
            return serialString;
        }

        default: {
            return nullptr;
        }
    }
}

/**
 * Events of the device: bus reset (speed), configuration, descriptors.
 */
usb_status_t onDeviceEvent(usb_device_handle, uint32_t event, void *param) {
    switch (event) {
        case kUSB_DeviceEventBusReset: {
            // no configuration any more; the speed the host chose
            configured = false;
            sending = false;
            receiving = false;
            currentConfiguration = 0;
            uint8_t speed = USB_SPEED_FULL;
            if (USB_DeviceClassGetSpeed(kUSB_ControllerEhci0, &speed) == kStatus_USB_Success) {
                applySpeed(speed);
            }

            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventSetConfiguration: {
            // 1: the port is up, receive and send; 0: down
            currentConfiguration = *(uint8_t *)param;
            configured = currentConfiguration == 1;
            sending = false;
            receiving = false;
            armReceiving();
            startSending();
            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventGetConfiguration: {
            *(uint8_t *)param = currentConfiguration;
            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventSetInterface: {
            // one alternate setting per interface
            uint8_t alternate = (uint8_t)(*(uint16_t *)param & 0xFF);
            return alternate == 0 ? kStatus_USB_Success : kStatus_USB_InvalidRequest;
        }

        case kUSB_DeviceEventGetInterface: {
            // alternate setting 0, the interface kept in the high byte
            uint16_t *value = (uint16_t *)param;
            *value = (uint16_t)(*value & 0xFF00);
            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventGetDeviceDescriptor: {
            return giveDescriptor(param, deviceDescriptor, sizeof(deviceDescriptor));
        }

        case kUSB_DeviceEventGetConfigurationDescriptor: {
            return giveDescriptor(param, configurationDescriptor, sizeof(configurationDescriptor));
        }

        case kUSB_DeviceEventGetDeviceQualifierDescriptor: {
            return giveDescriptor(param, qualifierDescriptor, sizeof(qualifierDescriptor));
        }

        case kUSB_DeviceEventGetStringDescriptor: {
            usb_device_get_string_descriptor_struct_t *request = (usb_device_get_string_descriptor_struct_t *)param;
            uint8_t *string = stringDescriptor(request->stringIndex);
            if (string == nullptr) {
                return kStatus_USB_InvalidRequest;
            }

            return giveDescriptor(param, string, string[0]);
        }

        default: {
            return kStatus_USB_InvalidRequest;
        }
    }
}

// ### Start ###################################################################

/**
 * A string descriptor of ASCII 'text' (UTF-16LE) into 'descriptor', which
 * holds MAX_STRING_CHARS characters.
 */
void makeString(uint8_t *descriptor, const char *text) {
    size_t length = strlen(text);
    length = length < MAX_STRING_CHARS ? length : MAX_STRING_CHARS;

    descriptor[0] = (uint8_t)(2 + 2 * length);
    descriptor[1] = USB_DESCRIPTOR_TYPE_STRING;

    for (size_t i = 0; i < length; i++) {
        descriptor[2 + 2 * i] = (uint8_t)text[i];
        descriptor[3 + 2 * i] = 0;
    }
}

/**
 * The serial number: the chip's unique id (board::chipId) in hexadecimal.
 */
void makeSerialString() {
    // the id
    uint8_t id[board::MAX_CHIP_ID];
    size_t length = board::chipId(id);

    // as hexadecimal text
    const char *digits = "0123456789ABCDEF";
    char text[2 * board::MAX_CHIP_ID + 1];

    for (size_t i = 0; i < length; i++) {
        text[2 * i] = digits[id[i] >> 4];
        text[2 * i + 1] = digits[id[i] & 0xF];
    }

    text[2 * length] = 0;

    makeString(serialString, text);
}

} // namespace

/**
 * The interrupt of USB1: the whole stack runs here.
 */
extern "C" void USB_OTG1_IRQHandler(void) {
    USB_DeviceEhciIsrFunction(device);
    __DSB();
}

namespace usbserial {

/**
 * 1. the strings (the serial number from the fuses), the receive signal;
 * 2. the USB clocks and the PHY;
 * 3. the stack with the CDC class;
 * 4. its interrupt, then the device on the bus.
 */
void start() {
    // strings, signal
    makeString(manufacturerString, MANUFACTURER);
    makeString(productString, PRODUCT);
    makeSerialString();
    receivedSignal = xSemaphoreCreateBinary();

    // clocks and PHY
    usb_phy_config_struct_t phyConfig = { PHY_D_CAL, PHY_TXCAL45DP, PHY_TXCAL45DM };
    CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, 480000000U);
    CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, 480000000U);
    USB_EhciPhyInit(kUSB_ControllerEhci0, qjsxCrystalHz(), &phyConfig);

    // the stack
    if (USB_DeviceClassInit(kUSB_ControllerEhci0, &classConfigList, &device) != kStatus_USB_Success) {
        platform::fatal("cannot start the USB device");
    }

    // the interrupt, then the device on the bus
    NVIC_SetPriority(USB_OTG1_IRQn, USB_INTERRUPT_PRIORITY);
    EnableIRQ(USB_OTG1_IRQn);
    USB_DeviceRun(device);
}

/**
 * 1. copy what the ring holds, in up to two pieces (it may wrap);
 * 2. room was made: arm the next OUT transfer if it waited for room.
 */
size_t read(uint8_t *buffer, size_t size) {
    // the bytes
    size_t head = loadIndex(&receiveHead);
    size_t tail = receiveTail;
    size_t used = ringUsed(head, tail, RECEIVE_RING_SIZE);
    size_t taken = used < size ? used : size;

    for (size_t i = 0; i < taken; i++) {
        buffer[i] = receiveRing[tail];
        tail = (tail + 1) & (RECEIVE_RING_SIZE - 1);
    }

    storeIndex(&receiveTail, tail);

    // the next transfer, if it waited
    if (taken > 0) {
        uint32_t mask = DisableGlobalIRQ();
        armReceiving();
        EnableGlobalIRQ(mask);
    }

    return taken;
}

/**
 * Takes the signal given by the interrupt.
 */
bool waitReceived(uint32_t timeoutMs) {
    return xSemaphoreTake(receivedSignal, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

/**
 * 1. the write lock for the whole call;
 * 2. copy into the send ring what fits, start sending, wait for room while
 *    the host takes the bytes (WRITE_TIMEOUT_MS without progress: drop);
 * 3. without a host: drop at once.
 */
bool write(const uint8_t *data, size_t size) {
    // one writer at a time, for the whole call
    writeLock.lock();

    // before the scheduler runs, no waiting for the host
    int64_t lastProgress = platform::monotonicMs();
    bool schedulerRunning = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    bool complete = true;

    while (size > 0) {
        // no host: nothing to send to
        if (!configured) {
            complete = false;
            break;
        }

        // what fits in the ring now
        size_t head = sendHead;
        size_t room = ringFree(head, loadIndex(&sendTail), SEND_RING_SIZE);
        size_t taken = size < room ? size : room;

        for (size_t i = 0; i < taken; i++) {
            sendRing[head] = data[i];
            head = (head + 1) & (SEND_RING_SIZE - 1);
        }

        storeIndex(&sendHead, head);
        data += taken;
        size -= taken;

        // start the transfer if none runs
        uint32_t mask = DisableGlobalIRQ();
        startSending();
        EnableGlobalIRQ(mask);

        // progress, or wait for the host (give up after WRITE_TIMEOUT_MS)
        int64_t now = platform::monotonicMs();
        bool givingUp = now - lastProgress > WRITE_TIMEOUT_MS || !schedulerRunning;
        if (taken > 0) {
            lastProgress = now;
        } else if (givingUp) {
            complete = false;
            break;
        } else {
            vTaskDelay(1);
        }
    }

    writeLock.unlock();
    return complete;
}

/**
 * Waits until the send ring is empty and no transfer runs.
 */
void flush(uint32_t timeoutMs) {
    int64_t deadline = platform::monotonicMs() + timeoutMs;
    bool schedulerRunning = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;

    for (;;) {
        // everything sent, no host any more, or too late: done
        bool empty = ringUsed(loadIndex(&sendHead), loadIndex(&sendTail), SEND_RING_SIZE) == 0;
        bool drained = empty && !sending;
        bool late = platform::monotonicMs() > deadline;
        if (drained || !configured || late) {
            return;
        }

        // let the interrupt send (a busy wait before the scheduler runs)
        if (schedulerRunning) {
            vTaskDelay(1);
        }
    }
}

} // namespace usbserial
