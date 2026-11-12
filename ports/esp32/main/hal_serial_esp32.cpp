/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The driver of device/serial on the ESP32 (hal_serial.h): the UARTs of the
// chip, with the uart driver of ESP-IDF.
//
// - A UART whose driver is installed already is busy: the link of the agent
//   (UART0, board_esp32.cpp) is refused that way.
// - Reception: the driver signals bytes through its event queue when the
//   line has been quiet for 10 characters (0.87 ms at 115200) or when 120
//   bytes are in its FIFO; a read waits for such an event (100 ms at most),
//   then takes what the driver buffered. A burst thus arrives in one block.
// - A closed UART gives its pins back as plain GPIO inputs (gpio_reset_pin):
//   unlike a pin of device/gpio, a pin of a UART is the UART's while it is
//   open only.
// - A program that ends closes its ports from its finalizers, through the
//   reader threads (within 100 ms): open() waits that long for a UART being
//   closed, so that the next program can take it again at once.
#include "hal_serial.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"

#ifdef SOC_UART_HP_NUM
const int UARTS = SOC_UART_HP_NUM;     // UARTs of the chip (not the low-power one)
#else
const int UARTS = SOC_UART_NUM;
#endif

// An open UART (hal_serial.h).
struct hal::serial::Port {
    uart_port_t   number;              // the UART
    QueueHandle_t events;              // the driver's event queue
    int           pins[4];             // tx, rx, rts, cts; hal::serial::NO_PIN: not wired
};

namespace {

using hal::serial::Parity;
using hal::serial::Port;
using hal::serial::Settings;

const int RX_BUFFER = 2048;            // receive buffer of the driver, bytes
const int TX_BUFFER = 1024;            // send buffer of the driver, bytes
const int EVENT_QUEUE = 16;            // events the driver can queue
const int FLOW_THRESHOLD = 100;        // bytes in the receive FIFO (128) that raise RTS
const int READ_WAIT_MS = 100;          // a read waits that long for bytes at most
const int CLOSING_WAIT_MS = 300;       // open() waits that long for a UART being closed
const int CLOSING_POLL_MS = 10;        // ...looking that often

Port ports[UARTS];                     // the open UARTs (tableLock for 'taken')
bool taken[UARTS];                     // the UART is open by device/serial (tableLock)
platform::Mutex tableLock;             // 'taken'

/**
 * True while UART 'number' is open by this driver.
 */
bool isTaken(int number) {
    tableLock.lock();
    bool busy = taken[number];
    tableLock.unlock();
    return busy;
}

/**
 * Waits until UART 'number' is no longer open by this driver (a port being
 * closed by its reader thread), CLOSING_WAIT_MS at most. True if it is free.
 */
bool waitUntilFree(int number) {
    for (int waited = 0; waited < CLOSING_WAIT_MS; waited += CLOSING_POLL_MS) {
        if (!isTaken(number)) {
            return true;
        }

        platform::sleepMs(CLOSING_POLL_MS);
    }

    return !isTaken(number);
}

/**
 * The configuration of the driver for 'settings'.
 */
uart_config_t configOf(const Settings *settings) {
    uart_config_t config = {};
    config.baud_rate = settings->baudRate;
    config.data_bits = (uart_word_length_t)(UART_DATA_5_BITS + (settings->dataBits - 5));
    config.parity = settings->parity == Parity::even  ? UART_PARITY_EVEN
                  : settings->parity == Parity::odd   ? UART_PARITY_ODD
                                                      : UART_PARITY_DISABLE;
    config.stop_bits = settings->stopBits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    config.flow_ctrl = settings->flowControl ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE;
    config.rx_flow_ctrl_thresh = FLOW_THRESHOLD;
    config.source_clk = UART_SCLK_DEFAULT;
    return config;
}

/**
 * Gives the wired pins of 'port' back as plain GPIO.
 */
void releasePins(const Port *port) {
    for (int i = 0; i < 4; i++) {
        if (port->pins[i] != hal::serial::NO_PIN) {
            gpio_reset_pin((gpio_num_t)port->pins[i]);
        }
    }
}

/**
 * Opens UART 'number' (see hal_serial.h for the errors):
 * 1. checks the number, waits for a UART being closed, refuses one in use
 *    (open already, or the link of the agent);
 * 2. installs the driver with its event queue;
 * 3. sets the format and the pins (a pin not given is not wired).
 */
Port *openPort(const char *, int number, const Settings *settings, int *error) {
    // 1. a UART of the chip, free
    if (number < 0 || number >= UARTS) {
        *error = ENOENT;
        return nullptr;
    }

    if (!waitUntilFree(number) || uart_is_driver_installed((uart_port_t)number)) {
        *error = EBUSY;
        return nullptr;
    }

    // 2. the driver
    Port *port = &ports[number];
    port->number = (uart_port_t)number;
    port->events = nullptr;
    if (uart_driver_install(port->number, RX_BUFFER, TX_BUFFER, EVENT_QUEUE, &port->events, 0) != ESP_OK) {
        *error = EIO;
        return nullptr;
    }

    // 3. format and pins; refused: undo
    uart_config_t config = configOf(settings);
    bool configured = uart_param_config(port->number, &config) == ESP_OK
                      && uart_set_pin(port->number, settings->tx, settings->rx, settings->rts, settings->cts) == ESP_OK;
    if (!configured) {
        uart_driver_delete(port->number);
        *error = EINVAL;
        return nullptr;
    }

    // open
    port->pins[0] = settings->tx;
    port->pins[1] = settings->rx;
    port->pins[2] = settings->rts;
    port->pins[3] = settings->cts;
    tableLock.lock();
    taken[number] = true;
    tableLock.unlock();
    return port;
}

/**
 * Reads what the driver received (see the top of the file):
 * 1. bytes left by a previous read are taken at once;
 * 2. else waits for an event of the driver; an overflow empties its buffer
 *    (the bytes are lost: said on the console);
 * 3. takes what is buffered, as much as fits.
 */
int readPort(Port *port, uint8_t *buffer, size_t size) {
    // 1. bytes there already
    size_t buffered = 0;
    uart_get_buffered_data_len(port->number, &buffered);

    // 2. wait for the driver
    if (buffered == 0) {
        uart_event_t event;
        if (xQueueReceive(port->events, &event, pdMS_TO_TICKS(READ_WAIT_MS)) != pdTRUE) {
            return 0;
        }

        bool overflow = event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL;
        if (overflow) {
            uart_flush_input(port->number);
            xQueueReset(port->events);
            printf("qjsx: UART %d: receive buffer full, bytes lost\n", (int)port->number);
            return 0;
        }

        uart_get_buffered_data_len(port->number, &buffered);
    }

    // 3. what is there
    size_t wanted = buffered < size ? buffered : size;
    if (wanted == 0) {
        return 0;
    }

    int received = uart_read_bytes(port->number, buffer, wanted, 0);
    return received < 0 ? -1 : received;
}

/**
 * Queues the bytes in the driver's send buffer (blocks while it is full).
 */
bool writePort(Port *port, const uint8_t *data, size_t size) {
    int written = uart_write_bytes(port->number, data, size);
    return written == (int)size;
}

/**
 * Removes the driver, gives the pins back, frees the UART.
 */
void closePort(Port *port) {
    uart_driver_delete(port->number);
    releasePins(port);

    tableLock.lock();
    taken[port->number] = false;
    tableLock.unlock();
}

/**
 * The UARTs neither open nor used by the system (the link of the agent).
 */
void listPorts(hal::serial::ListHandler *add, void *context) {
    for (int number = 0; number < UARTS; number++) {
        bool free = !isTaken(number) && !uart_is_driver_installed((uart_port_t)number);
        if (free) {
            add(context, nullptr, number);
        }
    }
}

const hal::serial::Driver BOARD_DRIVER = {
    false,                             // UARTs by number, with pins
    &openPort,
    &readPort,
    &writePort,
    &closePort,
    &listPorts,
};

} // namespace

namespace hal {
namespace serial {

/**
 * The driver of the chip's UARTs.
 */
const Driver *boardDriver() {
    return &BOARD_DRIVER;
}

} // namespace serial
} // namespace hal
