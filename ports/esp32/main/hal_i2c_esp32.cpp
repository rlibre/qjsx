/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// I2C master on the ESP32 (hal_i2c.h), with the i2c_master driver of
// ESP-IDF in its blocking mode: a bus handle per controller, a device handle
// per address and clock.
//
// How the driver's errors map (read in esp_driver_i2c/i2c_master.c of
// ESP-IDF 6.1): a transfer that does not end well (NACK, or the clock held
// low too long by a device) gives ESP_ERR_INVALID_RESPONSE -> noAck; the time
// limit of the call gives ESP_ERR_TIMEOUT -> timeout; a probe without answer
// gives ESP_ERR_NOT_FOUND -> noAck. A probe always runs at 100 kHz.
#include "hal_i2c.h"
#include "platform.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "soc/soc_caps.h"

namespace hal {
namespace i2c {

namespace {

const int BUSES = SOC_HP_I2C_NUM;      // controllers of the chip (not the low-power one)
const int DEVICES = 32;                // devices of every bus together
const int GLITCH_FILTER = 7;           // clock cycles of glitches ignored (the driver's usual value)

i2c_master_bus_handle_t buses[BUSES];          // nullptr: closed (tableLock)

// A device added on a bus (tableLock).
struct Device {
    bool                    taken;     // reserved by addDevice(); false: free
    i2c_master_dev_handle_t handle;    // the driver's device; nullptr while being added
    int                     bus;       // its bus
};

Device devices[DEVICES];               // tableLock
platform::Mutex tableLock;             // buses, devices (not held during a transfer)

/**
 * The handle of device 'device'; nullptr if it is not one.
 */
i2c_master_dev_handle_t deviceHandle(int device) {
    if (device < 0 || device >= DEVICES) {
        return nullptr;
    }

    tableLock.lock();
    i2c_master_dev_handle_t handle = devices[device].handle;
    tableLock.unlock();
    return handle;
}

/**
 * The handle of bus 'bus'; nullptr if it is closed or not one.
 */
i2c_master_bus_handle_t busHandle(int bus) {
    if (bus < 0 || bus >= BUSES) {
        return nullptr;
    }

    tableLock.lock();
    i2c_master_bus_handle_t handle = buses[bus];
    tableLock.unlock();
    return handle;
}

} // namespace

/**
 * Controllers of the chip.
 */
int busCount() {
    return BUSES;
}

/**
 * A master bus on the controller 'bus'.
 */
bool openBus(int bus, int sda, int scl, bool pullUp) {
    if (bus < 0 || bus >= BUSES || busHandle(bus) != nullptr) {
        return false;
    }

    // the bus, blocking mode (no transfer queue)
    i2c_master_bus_config_t config;
    memset(&config, 0, sizeof(config));
    config.i2c_port = bus;
    config.sda_io_num = (gpio_num_t)sda;
    config.scl_io_num = (gpio_num_t)scl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = GLITCH_FILTER;
    config.flags.enable_internal_pullup = pullUp ? 1 : 0;

    i2c_master_bus_handle_t handle = nullptr;
    if (i2c_new_master_bus(&config, &handle) != ESP_OK) {
        return false;
    }

    tableLock.lock();
    buses[bus] = handle;
    tableLock.unlock();
    return true;
}

/**
 * Frees the bus (its devices were removed).
 */
void closeBus(int bus) {
    i2c_master_bus_handle_t handle = busHandle(bus);
    if (handle == nullptr) {
        return;
    }

    i2c_del_master_bus(handle);
    tableLock.lock();
    buses[bus] = nullptr;
    tableLock.unlock();
}

/**
 * A device handle for 'address' at 'frequencyHz' on the bus, in a free
 * entry of the table.
 */
int addDevice(int bus, int address, uint32_t frequencyHz) {
    i2c_master_bus_handle_t handle = busHandle(bus);
    if (handle == nullptr) {
        return -1;
    }

    // a free entry, reserved while the driver adds the device
    tableLock.lock();
    int found = -1;
    for (int i = 0; i < DEVICES && found < 0; i++) {
        if (!devices[i].taken) {
            found = i;
        }
    }
    if (found >= 0) {
        devices[found] = Device{ true, nullptr, bus };
    }
    tableLock.unlock();
    if (found < 0) {
        return -1;
    }

    // the device in the driver
    i2c_device_config_t config;
    memset(&config, 0, sizeof(config));
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = (uint16_t)address;
    config.scl_speed_hz = frequencyHz;

    i2c_master_dev_handle_t device = nullptr;
    bool added = i2c_master_bus_add_device(handle, &config, &device) == ESP_OK;

    tableLock.lock();
    devices[found] = added ? Device{ true, device, bus } : Device{ false, nullptr, 0 };
    tableLock.unlock();
    return added ? found : -1;
}

/**
 * Frees a device handle.
 */
void removeDevice(int device) {
    i2c_master_dev_handle_t handle = deviceHandle(device);
    if (handle == nullptr) {
        return;
    }

    i2c_master_bus_rm_device(handle);
    tableLock.lock();
    devices[device] = Device{ false, nullptr, 0 };
    tableLock.unlock();
}

/**
 * A write, a read, or both with a repeated start.
 */
Status transfer(int device, const uint8_t *writeData, size_t writeSize, uint8_t *readData, size_t readSize,
                int timeoutMs) {
    i2c_master_dev_handle_t handle = deviceHandle(device);
    if (handle == nullptr || (writeSize == 0 && readSize == 0)) {
        return Status::failed;
    }

    // the call of the driver for this kind of transfer
    esp_err_t result = ESP_FAIL;
    if (readSize == 0) {
        result = i2c_master_transmit(handle, writeData, writeSize, timeoutMs);
    } else if (writeSize == 0) {
        result = i2c_master_receive(handle, readData, readSize, timeoutMs);
    } else {
        result = i2c_master_transmit_receive(handle, writeData, writeSize, readData, readSize, timeoutMs);
    }

    // its result
    switch (result) {
        case ESP_OK: {
            return Status::ok;
        }
        case ESP_ERR_INVALID_RESPONSE: {
            return Status::noAck;
        }
        case ESP_ERR_TIMEOUT: {
            return Status::timeout;
        }
        default: {
            return Status::failed;
        }
    }
}

/**
 * Whether a device answers at 'address'.
 */
Status probe(int bus, int address, int timeoutMs) {
    i2c_master_bus_handle_t handle = busHandle(bus);
    if (handle == nullptr) {
        return Status::failed;
    }

    switch (i2c_master_probe(handle, (uint16_t)address, timeoutMs)) {
        case ESP_OK: {
            return Status::ok;
        }
        case ESP_ERR_NOT_FOUND: {
            return Status::noAck;
        }
        case ESP_ERR_TIMEOUT: {
            return Status::timeout;
        }
        default: {
            return Status::failed;
        }
    }
}

/**
 * The driver's bus recovery: clock pulses, then a stop.
 */
void resetBus(int bus) {
    i2c_master_bus_handle_t handle = busHandle(bus);
    if (handle != nullptr) {
        i2c_master_bus_reset(handle);
    }
}

} // namespace i2c
} // namespace hal
