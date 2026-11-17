/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// SPI master on the ESP32 (hal_spi.h), with the spi_master driver of ESP-IDF:
// ports 2 and 3 are SPI2_HOST and SPI3_HOST (0 and 1 hold the flash: busy),
// buses with DMA (SPI_DMA_CH_AUTO), one driver device per device.
//
// A transaction (run(), on the calling thread):
//   1. a buffer of the device's fill byte when it reads (sent while reading);
//   2. the bus acquired for the device (spi_device_acquire_bus: it also sets
//      the clock's idle level of the device's mode), then its chip select
//      driven active;
//   3. each segment: a delay is a busy wait (esp_rom_delay_us, at most 1 ms
//      by the module); bytes go in pieces of at most the bus's maxTransfer,
//      each one driver transaction, polled when small, else through the
//      driver's queue (DMA, the CPU free); the DC pin is set by the driver's
//      pre_cb from the transaction's 'user' field, just before its bytes;
//   4. chip select inactive, the bus released.
//
// Choices:
// - The chip select is driven as a GPIO by this file (the driver's device has
//   spics_io_num -1): it must stay active across pieces and delays, also a
//   delay that starts or ends the transaction, which the driver's own chip
//   select (SPI_TRANS_CS_KEEP_ACTIVE) cannot do without a data phase.
// - start() hands the transaction to the worker task of the port, which runs
//   it as run() does and calls 'done' from the task: 'done' posts to the
//   loop's mailbox, which takes a lock and allocates, so it is never called
//   from the driver's interrupt (post_cb). One worker per port, started at
//   its first openBus(), kept for good.
// - Buffers that are not DMA-capable (PSRAM, unaligned) are copied by the
//   driver into DMA-capable ones, piece by piece.
// - A full-duplex device above about 26 MHz on pins routed through the GPIO
//   matrix is refused by the driver (reads would be wrong); it is then added
//   with SPI_DEVICE_NO_DUMMY: writes are right, reads at the program's risk.
#include "hal_spi.h"
#include "memory.h"
#include "platform.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

namespace hal {
namespace spi {

namespace {

const int    FIRST_PORT = 2;                    // SPI2: the first controller a program may take
const int    PORTS = 4;                         // port numbers 0..3 (0 and 1: the flash's)
const int    WORKERS = PORTS - FIRST_PORT;      // one worker task per port a program may take
const int    DEVICES = 16;                      // devices of every bus together
const size_t POLLING_MAX_BYTES = 32;            // pieces up to this size are polled (no interrupt)
const int    DEVICE_QUEUE_SIZE = 2;             // transactions of a device in the driver's queue (2: a made segment
                                                // makes a piece while the one before goes out)

// An open bus (tableLock).
struct Bus {
    bool              open;                     // spi_bus_initialize done
    spi_host_device_t host;                     // the controller
    size_t            maxTransfer;              // bytes of one driver transaction at most
};

// A device added on a bus (tableLock).
struct Device {
    bool                taken;                  // reserved by addDevice(); false: free
    spi_device_handle_t handle;                 // the driver's device; nullptr while being added
    int                 port;                   // its bus
    int                 cs;                     // chip select pin, driven here; -1: none
    int                 dc;                     // data / command pin; -1: none
    bool                csActiveHigh;           // chip select active at level 1
    uint8_t             fill;                   // byte sent while reading
};

// The worker task of a port: runs the transactions given by start().
struct Worker {
    platform::Signal signal;                    // guards the fields below; notified when work comes
    bool             started = false;           // its task runs (JS thread only)
    bool             pending = false;           // a transaction waits for the task
    int              device = -1;               // the transaction: its device,
    const Segment   *segments = nullptr;        // its segments (the caller's, valid until finish())
    size_t           count = 0;                 // and their number
    DoneCallback     done = nullptr;            // called by the task at its end
    void            *opaque = nullptr;          // given to 'done'
};

Bus buses[PORTS];                               // by port; tableLock
Device devices[DEVICES];                        // tableLock
platform::Mutex tableLock;                      // buses, devices (not held during a transfer)
Worker workers[WORKERS];                        // by port - FIRST_PORT
const char *const WORKER_NAMES[WORKERS] = { "spi2", "spi3" };   // names of their tasks

/**
 * The controller of 'port' in *host; false if the chip has none a program
 * may take.
 */
bool hostOf(int port, spi_host_device_t *host) {
    // SPI2, on every chip
    if (port == 2) {
        *host = SPI2_HOST;
        return true;
    }

    // SPI3, on the chips that have it (ESP32, ESP32-S2, ESP32-S3); an
    // ESP-IDF without SOC_SPI_PERIPH_NUM is taken for one that has it
#if !defined(SOC_SPI_PERIPH_NUM) || SOC_SPI_PERIPH_NUM > 2
    if (port == 3) {
        *host = SPI3_HOST;
        return true;
    }

#endif
    return false;
}

/**
 * Makes 'pin' an output at 'level' (the level written before and after the
 * configuration, so that it does not glitch).
 */
void claimOutput(int pin, int level) {
    // the level first, then the pin as an output
    gpio_set_level((gpio_num_t)pin, (uint32_t)level);
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&config);
    gpio_set_level((gpio_num_t)pin, (uint32_t)level);
}

/**
 * Gives 'pin' back as at boot (-1: nothing).
 */
void releasePin(int pin) {
    if (pin >= 0) {
        gpio_reset_pin((gpio_num_t)pin);
    }
}

/**
 * The level of the chip select of 'device' when 'active' or not.
 */
int chipSelectLevel(const Device *device, bool active) {
    bool high = active == device->csActiveHigh;
    return high ? 1 : 0;
}

/**
 * Drives the chip select of 'device' active or inactive (none: nothing).
 */
void setChipSelect(const Device *device, bool active) {
    if (device->cs >= 0) {
        gpio_set_level((gpio_num_t)device->cs, (uint32_t)chipSelectLevel(device, active));
    }
}

/**
 * The 'user' field of a driver transaction for setDataCommand(): the DC pin
 * of 'device' and the level 'dc' packed as (pin + 1) << 1 | level, so that
 * 0 means "no DC pin".
 */
void *dataCommandCode(const Device *device, int dc) {
    // no DC pin: nothing to set
    if (device->dc < 0) {
        return nullptr;
    }

    intptr_t code = ((intptr_t)(device->dc + 1) << 1) | (intptr_t)(dc & 1);
    return (void *)code;
}

/**
 * pre_cb of the devices that have a DC pin: sets it from the transaction's
 * 'user' field (dataCommandCode()) just before its bytes go out. Called by
 * the driver, from its interrupt for a queued transaction (in IRAM; the bus's
 * interrupt is not registered as IRAM-safe, so it never runs while the flash
 * cache is off).
 */
void IRAM_ATTR setDataCommand(spi_transaction_t *transaction) {
    // 0: no DC pin
    intptr_t code = (intptr_t)transaction->user;
    if (code == 0) {
        return;
    }

    // the pin and the level, unpacked
    int pin = (int)(code >> 1) - 1;
    uint32_t level = (uint32_t)(code & 1);
    gpio_set_level((gpio_num_t)pin, level);
}

/**
 * A copy of device 'number' and the maxTransfer of its bus; false if it is
 * not a device added.
 */
bool deviceOf(int number, Device *out, size_t *maxTransfer) {
    // an entry of the table
    if (number < 0 || number >= DEVICES) {
        return false;
    }

    // read under the lock
    tableLock.lock();
    *out = devices[number];
    bool added = out->taken && out->handle != nullptr;
    if (added) {
        *maxTransfer = buses[out->port].maxTransfer;
    }

    tableLock.unlock();
    return added;
}

/**
 * The bytes of fill a transaction needs: its largest read, at most one
 * piece (0 when it reads nothing).
 */
size_t fillSizeOf(const Segment *segments, size_t count, size_t maxTransfer) {
    size_t largest = 0;

    for (size_t i = 0; i < count; i++) {
        bool isRead = segments[i].write == nullptr && segments[i].read != nullptr;
        if (isRead && segments[i].size > largest) {
            largest = segments[i].size;
        }
    }

    return largest < maxTransfer ? largest : maxTransfer;
}

/**
 * Runs a made segment on 'device' (bus acquired, chip select active): each
 * piece is made while the one before goes out, in the two halves of the
 * segment's 'piece' in turn (hal_spi.h), so making the pixels of a display
 * and sending them overlap. A half is made again only once its transfer is
 * over (at most two transfers in the driver's queue: DEVICE_QUEUE_SIZE).
 */
Status runMadeSegment(const Device *device, const Segment *segment, size_t maxTransfer) {
    // pieces of its own size, within one transfer of the bus
    bool fits = segment->pieceSize > 0 && segment->pieceSize <= maxTransfer;
    if (!fits) {
        return Status::failed;
    }

    // the two halves, and the transfer of each
    uint8_t *halves[2] = { segment->piece, segment->piece + segment->pieceSize };
    spi_transaction_t transfers[2];
    size_t offset = 0;
    int next = 0;
    int inFlight = 0;
    Status status = Status::ok;

    while (offset < segment->size || inFlight > 0) {
        // a half free and bytes left: the next piece made there, queued
        bool canQueue = offset < segment->size && inFlight < 2 && status == Status::ok;
        if (canQueue) {
            size_t left = segment->size - offset;
            size_t piece = left < segment->pieceSize ? left : segment->pieceSize;
            segment->make(segment->makeOpaque, halves[next], offset, piece);

            spi_transaction_t *transfer = &transfers[next];
            memset(transfer, 0, sizeof(*transfer));
            transfer->length = piece * 8;
            transfer->tx_buffer = halves[next];
            transfer->user = dataCommandCode(device, segment->dc);
            if (spi_device_queue_trans(device->handle, transfer, portMAX_DELAY) != ESP_OK) {
                status = Status::failed;
                continue;
            }

            offset += piece;
            next = 1 - next;
            inFlight++;
            continue;
        }

        // nothing more to queue now: the oldest transfer waited for, its
        // half free again
        if (inFlight == 0) {
            break;
        }

        spi_transaction_t *done = nullptr;
        if (spi_device_get_trans_result(device->handle, &done, portMAX_DELAY) != ESP_OK) {
            status = Status::failed;
        }

        inFlight--;
    }

    return status;
}

/**
 * Runs one segment on 'device' (bus acquired, chip select active): a delay,
 * a made segment (runMadeSegment), or its bytes in pieces of at most
 * 'maxTransfer'. A read sends 'fill'.
 */
Status runSegment(const Device *device, const Segment *segment, const uint8_t *fill, size_t maxTransfer) {
    // a made segment: made and sent at the same time
    if (segment->make != nullptr) {
        return runMadeSegment(device, segment, maxTransfer);
    }

    // a delay: a busy wait (at most 1 ms), the chip select kept
    bool isDelay = segment->write == nullptr && segment->read == nullptr;
    if (isDelay) {
        esp_rom_delay_us(segment->delayUs);
        return Status::ok;
    }

    // the bytes, piece by piece
    size_t offset = 0;

    while (offset < segment->size) {
        // the next piece: what it sends (its bytes or the fill), where it
        // receives, its DC level
        size_t left = segment->size - offset;
        size_t piece = left < maxTransfer ? left : maxTransfer;
        const uint8_t *sent = segment->write != nullptr ? segment->write + offset : fill;

        spi_transaction_t transaction;
        memset(&transaction, 0, sizeof(transaction));
        transaction.length = piece * 8;
        transaction.tx_buffer = sent;
        if (segment->read != nullptr) {
            transaction.rx_buffer = segment->read + offset;
            transaction.rxlength = piece * 8;
        }

        transaction.user = dataCommandCode(device, segment->dc);

        // polled when small (no interrupt), else queued and awaited (DMA)
        esp_err_t result = ESP_FAIL;
        if (piece <= POLLING_MAX_BYTES) {
            result = spi_device_polling_transmit(device->handle, &transaction);
        } else {
            result = spi_device_transmit(device->handle, &transaction);
        }

        if (result != ESP_OK) {
            return Status::failed;
        }

        offset += piece;
    }

    return Status::ok;
}

/**
 * The worker task of a port (argument: its Worker): runs each transaction
 * start() gives it, then calls its 'done', for good.
 */
void workerTask(void *argument) {
    Worker *worker = (Worker *)argument;
    worker->signal.lock();

    for (;;) {
        // the next transaction
        while (!worker->pending) {
            worker->signal.wait(-1);
        }

        int device = worker->device;
        const Segment *segments = worker->segments;
        size_t count = worker->count;
        DoneCallback done = worker->done;
        void *opaque = worker->opaque;
        worker->pending = false;
        worker->signal.unlock();

        // run, then tell the module
        Status status = run(device, segments, count);
        done(opaque, status);
        worker->signal.lock();
    }
}

/**
 * Starts, once, the worker task of 'port' (JS thread). False if it cannot
 * start.
 */
bool startWorker(int port) {
    // started already
    Worker *worker = &workers[port - FIRST_PORT];
    if (worker->started) {
        return true;
    }

    // its task
    platform::ThreadOptions options = platform::defaultThreadOptions(WORKER_NAMES[port - FIRST_PORT]);
    worker->started = platform::startThread(&workerTask, worker, options);
    return worker->started;
}

/**
 * The status of an error of spi_bus_initialize / spi_bus_add_device.
 */
Status statusOf(esp_err_t result) {
    switch (result) {
        case ESP_OK: {
            return Status::ok;
        }
        case ESP_ERR_INVALID_STATE:
        case ESP_ERR_NOT_FOUND: {
            // the controller in use, or no driver slot left for a device
            return Status::busy;
        }
        default: {
            return Status::failed;
        }
    }
}

} // namespace

/**
 * SPI2 and SPI3 (when the chip has it).
 */
bool portExists(int port) {
    spi_host_device_t host;
    return hostOf(port, &host);
}

/**
 * The controller with DMA on its pins, and the worker task of the port.
 */
Status openBus(int port, int sclk, int mosi, int miso, size_t maxTransfer) {
    // the flash's controllers, then a controller of the chip, not open yet
    if (port == 0 || port == 1) {
        return Status::busy;
    }

    spi_host_device_t host;
    if (!hostOf(port, &host)) {
        return Status::noSuchPort;
    }

    tableLock.lock();
    bool isOpen = buses[port].open;
    tableLock.unlock();
    if (isOpen) {
        return Status::busy;
    }

    // pins the chip can use: outputs for the clock and MOSI, an input for
    // MISO, at least one data pin
    bool sclkValid = GPIO_IS_VALID_OUTPUT_GPIO(sclk);
    bool mosiValid = mosi < 0 || GPIO_IS_VALID_OUTPUT_GPIO(mosi);
    bool misoValid = miso < 0 || GPIO_IS_VALID_GPIO(miso);
    bool hasData = mosi >= 0 || miso >= 0;
    bool pinsValid = sclkValid && mosiValid && misoValid && hasData;
    if (!pinsValid) {
        return Status::badPin;
    }

    // its worker, for start()
    if (!startWorker(port)) {
        return Status::failed;
    }

    // the bus: no quad / octal lines, DMA for the large pieces
    spi_bus_config_t config = {};
    config.mosi_io_num = mosi;
    config.miso_io_num = miso;
    config.sclk_io_num = sclk;
    config.quadwp_io_num = -1;
    config.quadhd_io_num = -1;
    config.data4_io_num = -1;
    config.data5_io_num = -1;
    config.data6_io_num = -1;
    config.data7_io_num = -1;
    config.max_transfer_sz = (int)maxTransfer;
    config.flags = SPICOMMON_BUSFLAG_MASTER;

    esp_err_t result = spi_bus_initialize(host, &config, SPI_DMA_CH_AUTO);
    if (result == ESP_ERR_INVALID_ARG) {
        // the pins were checked: a pin the controller cannot route
        return Status::badPin;
    }

    if (result != ESP_OK) {
        return statusOf(result);
    }

    // recorded
    tableLock.lock();
    buses[port] = Bus{ true, host, maxTransfer };
    tableLock.unlock();
    return Status::ok;
}

/**
 * Frees the controller (its devices were removed). The worker stays.
 */
void closeBus(int port) {
    // an open bus
    bool isPort = port >= 0 && port < PORTS;
    if (!isPort) {
        return;
    }

    tableLock.lock();
    Bus bus = buses[port];
    tableLock.unlock();
    if (!bus.open) {
        return;
    }

    // freed by the driver, then forgotten
    spi_bus_free(bus.host);
    tableLock.lock();
    buses[port] = Bus{ false, bus.host, 0 };
    tableLock.unlock();
}

/**
 * A driver device (no chip select of its own: this file drives it), its
 * chip select and DC pins as outputs, in a free entry of the table.
 */
Status addDevice(int port, const DeviceConfig *config, int *device, uint32_t *actualHz) {
    // an open bus, pins the chip can drive
    bool isPort = port >= 0 && port < PORTS;
    if (!isPort) {
        return Status::noSuchPort;
    }

    tableLock.lock();
    Bus bus = buses[port];
    tableLock.unlock();
    if (!bus.open) {
        return Status::failed;
    }

    bool csValid = config->cs < 0 || GPIO_IS_VALID_OUTPUT_GPIO(config->cs);
    bool dcValid = config->dc < 0 || GPIO_IS_VALID_OUTPUT_GPIO(config->dc);
    if (!csValid || !dcValid) {
        return Status::badPin;
    }

    // a free entry, reserved while the driver adds the device
    tableLock.lock();
    int found = -1;

    for (int i = 0; i < DEVICES && found < 0; i++) {
        if (!devices[i].taken) {
            found = i;
        }
    }

    Device entry = { true, nullptr, port, config->cs, config->dc, config->csActiveHigh, config->fill };
    if (found >= 0) {
        devices[found] = entry;
    }

    tableLock.unlock();
    if (found < 0) {
        return Status::busy;
    }

    // its pins: chip select inactive, DC high (data)
    if (config->cs >= 0) {
        claimOutput(config->cs, chipSelectLevel(&entry, false));
    }

    if (config->dc >= 0) {
        claimOutput(config->dc, 1);
    }

    // the device in the driver: mode, clock, bit order, DC through pre_cb
    spi_device_interface_config_t settings = {};
    settings.mode = (uint8_t)config->mode;
    settings.clock_source = SPI_CLK_SRC_DEFAULT;
    settings.clock_speed_hz = (int)config->frequencyHz;
    settings.spics_io_num = -1;
    settings.queue_size = DEVICE_QUEUE_SIZE;
    settings.flags = config->lsbFirst ? SPI_DEVICE_BIT_LSBFIRST : 0;
    settings.pre_cb = config->dc >= 0 ? &setDataCommand : nullptr;

    spi_device_handle_t handle = nullptr;
    esp_err_t result = spi_bus_add_device(bus.host, &settings, &handle);

    // refused (most often a full-duplex clock too high for the GPIO matrix):
    // once more without the read timing compensation
    if (result != ESP_OK) {
        settings.flags |= SPI_DEVICE_NO_DUMMY;
        result = spi_bus_add_device(bus.host, &settings, &handle);
    }

    // still refused: the pins back, the entry free again
    if (result != ESP_OK) {
        releasePin(config->cs);
        releasePin(config->dc);
        tableLock.lock();
        devices[found] = Device{ false, nullptr, 0, -1, -1, false, 0 };
        tableLock.unlock();
        return statusOf(result);
    }

    // the clock the hardware makes (kHz in the driver)
    int actualKhz = 0;
    bool knowsClock = spi_device_get_actual_freq(handle, &actualKhz) == ESP_OK;
    *actualHz = knowsClock ? (uint32_t)actualKhz * 1000 : config->frequencyHz;

    // recorded
    tableLock.lock();
    devices[found].handle = handle;
    tableLock.unlock();
    *device = found;
    return Status::ok;
}

/**
 * Removes the driver device and gives its pins back.
 */
void removeDevice(int number) {
    // a device added
    Device device;
    size_t maxTransfer = 0;
    if (!deviceOf(number, &device, &maxTransfer)) {
        return;
    }

    // removed from the driver, its pins back, then its entry freed
    spi_bus_remove_device(device.handle);
    releasePin(device.cs);
    releasePin(device.dc);
    tableLock.lock();
    devices[number] = Device{ false, nullptr, 0, -1, -1, false, 0 };
    tableLock.unlock();
}

/**
 * The transaction, on the calling thread (see the top of this file).
 */
Status run(int number, const Segment *segments, size_t count) {
    // a device added
    Device device;
    size_t maxTransfer = 0;
    if (!deviceOf(number, &device, &maxTransfer)) {
        return Status::failed;
    }

    // the fill bytes sent while reading
    size_t fillSize = fillSizeOf(segments, count, maxTransfer);
    uint8_t *fill = nullptr;
    if (fillSize > 0) {
        fill = (uint8_t *)memory::allocate(fillSize);
        memset(fill, device.fill, fillSize);
    }

    // the bus for this device alone, then its chip select
    if (spi_device_acquire_bus(device.handle, portMAX_DELAY) != ESP_OK) {
        memory::release(fill);
        return Status::failed;
    }

    setChipSelect(&device, true);

    // each segment, up to the first failure
    Status status = Status::ok;

    for (size_t i = 0; i < count && status == Status::ok; i++) {
        status = runSegment(&device, &segments[i], fill, maxTransfer);
    }

    // chip select inactive, the bus released
    setChipSelect(&device, false);
    spi_device_release_bus(device.handle);
    memory::release(fill);
    return status;
}

/**
 * Hands the transaction to the worker of the device's port; 'done' comes
 * from that task (at once if there is no such device).
 */
void start(int number, const Segment *segments, size_t count, DoneCallback done, void *opaque) {
    // the worker of the device's port
    Device device;
    size_t maxTransfer = 0;
    bool added = deviceOf(number, &device, &maxTransfer);
    bool hasWorker = added && device.port >= FIRST_PORT && workers[device.port - FIRST_PORT].started;
    if (!hasWorker) {
        done(opaque, Status::failed);
        return;
    }

    // the transaction, for it (the module never starts two on one bus)
    Worker *worker = &workers[device.port - FIRST_PORT];
    worker->signal.lock();
    worker->device = number;
    worker->segments = segments;
    worker->count = count;
    worker->done = done;
    worker->opaque = opaque;
    worker->pending = true;
    worker->signal.notify();
    worker->signal.unlock();
}

/**
 * Nothing to free: the worker acquired and released the bus itself.
 */
void finish(int) {
}

} // namespace spi
} // namespace hal
