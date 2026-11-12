/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// USB device on the ESP32 (hal_usb.h), with TinyUSB through Espressif's
// esp_tinyusb component, on the chip's USB OTG port (the native USB of the
// board, not the USB-serial bridge of the agent). Only built for the chips
// that have USB OTG (main/CMakeLists.txt).
//
// start():
//   1. fills the descriptors TinyUSB reads at each enumeration (static: they
//      must outlive the call), and picks the configuration of 1 or 2 serial
//      functions (CDC ACM, endpoints as esp_tinyusb's own defaults);
//   2. installs the driver: its task (core 0, away from the JS task) runs
//      TinyUSB;
//   3. creates the serial functions, whose callbacks run on that task.
// The received bytes are read straight into a block of memory::allocate()
// given to the data handler (no buffer on the stack of the task).
#include "hal_usb.h"
#include "memory.h"
#include "platform.h"

#include <string.h>

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"

namespace hal {
namespace usb {

namespace {

const uint16_t DEFAULT_PRODUCT_ID = 0x4001;      // what esp_tinyusb gives a device of CDC only (0x4000 | CDC)
const uint16_t DEVICE_RELEASE = 0x0100;          // bcdDevice: 1.00
const int WRITE_TIMEOUT_MS = 50;                 // longest wait for the host to take the bytes of a write
const int TASK_STACK = 4096;                     // stack of the TinyUSB task, bytes
const int TASK_PRIORITY = 5;                     // its priority
const int TASK_CORE = 0;                         // its core: the JS task runs on core 1

// Interfaces and endpoints of the serial functions, as esp_tinyusb numbers
// them in its default descriptors (two interfaces and three endpoints each).
const uint8_t ENDPOINT_IN = 0x80;                // direction bit of an IN endpoint
const int NOTIFY_PACKET = 8;                     // packet size of the notification endpoint
const int DATA_PACKET = 64;                      // packet size of the data endpoints (full speed)
const int POWER_MA = 100;                        // current asked from the host

// String descriptors, by index.
enum StringIndex {
    STRING_LANGUAGE = 0,                         // the language list: English (0x0409)
    STRING_MANUFACTURER,
    STRING_PRODUCT,
    STRING_SERIAL_NUMBER,
    STRING_COUNT
};

// Configurations of one and of two serial functions.
const uint16_t CONFIG_ONE_SIZE = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN;
const uint16_t CONFIG_TWO_SIZE = TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN;

const uint8_t configOne[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_ONE_SIZE, 0, POWER_MA),
    TUD_CDC_DESCRIPTOR(0, 0, ENDPOINT_IN | 1, NOTIFY_PACKET, 2, ENDPOINT_IN | 2, DATA_PACKET),
};

const uint8_t configTwo[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, CONFIG_TWO_SIZE, 0, POWER_MA),
    TUD_CDC_DESCRIPTOR(0, 0, ENDPOINT_IN | 1, NOTIFY_PACKET, 2, ENDPOINT_IN | 2, DATA_PACKET),
    TUD_CDC_DESCRIPTOR(2, 0, ENDPOINT_IN | 3, NOTIFY_PACKET, 4, ENDPOINT_IN | 4, DATA_PACKET),
};

// What TinyUSB reads while the device is started (JS thread writes them
// while it is stopped).
tusb_desc_device_t deviceDescriptor;             // the device
Descriptors texts;                               // copy of the texts given to start()
const char language[2] = { 0x09, 0x04 };         // English (0x0409), little-endian
const char *strings[STRING_COUNT];               // by StringIndex

bool started = false;                            // the driver is installed (JS thread)
int serialCount = 0;                             // serial functions of the started device

DataHandler *dataHandler = nullptr;              // handlerLock
LineStateHandler *lineStateHandler = nullptr;    // handlerLock
platform::Mutex handlerLock;                     // the handlers

/**
 * Bytes received on a serial function (TinyUSB task): read into a new block,
 * given to the data handler (dropped without one).
 */
void onReceive(int serial, cdcacm_event_t *) {
    // read what came, straight into the block
    uint8_t *block = (uint8_t *)memory::allocate(CONFIG_TINYUSB_CDC_RX_BUFSIZE);
    size_t size = 0;
    esp_err_t read = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)serial, block, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &size);
    if (read != ESP_OK || size == 0) {
        memory::release(block);
        return;
    }

    // to the handler, which owns the block
    handlerLock.lock();
    DataHandler *handler = dataHandler;
    handlerLock.unlock();
    if (handler == nullptr) {
        memory::release(block);
        return;
    }
    handler(serial, block, size);
}

/**
 * The host changed the line state of a serial function (TinyUSB task).
 */
void onLineState(int serial, cdcacm_event_t *event) {
    handlerLock.lock();
    LineStateHandler *handler = lineStateHandler;
    handlerLock.unlock();

    if (handler != nullptr) {
        handler(serial, event->line_state_changed_data.dtr, event->line_state_changed_data.rts);
    }
}

/**
 * Fills the device descriptor and the strings from 'descriptors'.
 */
void fillDescriptors(const Descriptors *descriptors) {
    texts = *descriptors;
    bool hasSerialNumber = texts.serialNumber[0] != '\0';

    // the device: a composite of CDC functions, which need the interface
    // association class
    memset(&deviceDescriptor, 0, sizeof(deviceDescriptor));
    deviceDescriptor.bLength = sizeof(deviceDescriptor);
    deviceDescriptor.bDescriptorType = TUSB_DESC_DEVICE;
    deviceDescriptor.bcdUSB = 0x0200;
    deviceDescriptor.bDeviceClass = TUSB_CLASS_MISC;
    deviceDescriptor.bDeviceSubClass = MISC_SUBCLASS_COMMON;
    deviceDescriptor.bDeviceProtocol = MISC_PROTOCOL_IAD;
    deviceDescriptor.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    deviceDescriptor.idVendor = texts.vendorId != 0 ? texts.vendorId : TINYUSB_ESPRESSIF_VID;
    deviceDescriptor.idProduct = texts.productId != 0 ? texts.productId : DEFAULT_PRODUCT_ID;
    deviceDescriptor.bcdDevice = DEVICE_RELEASE;
    deviceDescriptor.iManufacturer = STRING_MANUFACTURER;
    deviceDescriptor.iProduct = STRING_PRODUCT;
    deviceDescriptor.iSerialNumber = hasSerialNumber ? STRING_SERIAL_NUMBER : 0;
    deviceDescriptor.bNumConfigurations = 1;

    // the strings
    strings[STRING_LANGUAGE] = language;
    strings[STRING_MANUFACTURER] = texts.manufacturer;
    strings[STRING_PRODUCT] = texts.product;
    strings[STRING_SERIAL_NUMBER] = texts.serialNumber;
}

/**
 * Frees the first 'count' serial functions and the driver.
 */
void uninstall(int count) {
    for (int i = 0; i < count; i++) {
        tinyusb_cdcacm_deinit(i);
    }
    tinyusb_driver_uninstall();
}

} // namespace

/**
 * The handlers of the events.
 */
void setHandlers(DataHandler *data, LineStateHandler *lineState) {
    handlerLock.lock();
    dataHandler = data;
    lineStateHandler = lineState;
    handlerLock.unlock();
}

/**
 * Starts the device: descriptors, driver, then the serial functions.
 */
bool start(const Descriptors *descriptors, int serials) {
    if (started || serials < 1 || serials > MAX_SERIALS || serials > CONFIG_TINYUSB_CDC_COUNT) {
        return false;
    }

    // 1. the descriptors
    fillDescriptors(descriptors);

    // 2. the driver and its task
    tinyusb_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = TINYUSB_PORT_FULL_SPEED_0;
    config.task.size = TASK_STACK;
    config.task.priority = TASK_PRIORITY;
    config.task.xCoreID = TASK_CORE;
    config.descriptor.device = &deviceDescriptor;
    config.descriptor.string = strings;
    config.descriptor.string_count = STRING_COUNT;
    config.descriptor.full_speed_config = serials == 1 ? configOne : configTwo;
    if (tinyusb_driver_install(&config) != ESP_OK) {
        return false;
    }

    // 3. the serial functions
    for (int i = 0; i < serials; i++) {
        tinyusb_config_cdcacm_t function;
        memset(&function, 0, sizeof(function));
        function.cdc_port = (tinyusb_cdcacm_itf_t)i;
        function.callback_rx = &onReceive;
        function.callback_line_state_changed = &onLineState;
        if (tinyusb_cdcacm_init(&function) != ESP_OK) {
            uninstall(i);
            return false;
        }
    }

    started = true;
    serialCount = serials;
    return true;
}

/**
 * Stops the device: the serial functions, then the driver.
 */
void stop() {
    if (!started) {
        return;
    }

    uninstall(serialCount);
    started = false;
    serialCount = 0;
}

/**
 * Sends bytes: queued in the FIFO of the function as room comes, for at
 * most WRITE_TIMEOUT_MS; what the host did not take by then is dropped. DTR
 * is not looked at (a host may read without raising it). Dropped at once
 * when the host has not set the device up (cable unplugged).
 */
bool write(int serial, const uint8_t *data, size_t size) {
    if (!started || serial < 0 || serial >= serialCount) {
        return false;
    }

    // no host: dropped, as with a cable unplugged
    if (!tud_mounted()) {
        return true;
    }

    // queue them as room comes, pushing what is queued to the host; past
    // the deadline the rest is dropped
    tinyusb_cdcacm_itf_t function = (tinyusb_cdcacm_itf_t)serial;
    int64_t deadline = platform::monotonicMs() + WRITE_TIMEOUT_MS;
    size_t queued = 0;

    while (queued < size && platform::monotonicMs() < deadline) {
        queued += tinyusb_cdcacm_write_queue(function, data + queued, size - queued);
        if (queued < size) {
            tinyusb_cdcacm_write_flush(function, 0);
            platform::sleepMs(1);
        }
    }

    // send what is queued without waiting for it
    tinyusb_cdcacm_write_flush(function, 0);
    return true;
}

/**
 * The line state TinyUSB keeps: bit 0 DTR, bit 1 RTS.
 */
void lineState(int serial, bool *dtr, bool *rts) {
    *dtr = false;
    *rts = false;
    if (!started || serial < 0 || serial >= serialCount) {
        return;
    }

    uint8_t state = tud_cdc_n_get_line_state(serial);
    *dtr = (state & 0x01) != 0;
    *rts = (state & 0x02) != 0;
}

} // namespace usb
} // namespace hal
