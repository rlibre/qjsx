/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Digital pins on the i.MX RT1062 (hal_gpio.h). The pins are the board's
// numbers (boards/<board>/board_pins.h: pad and GPIO bit of each).
//
// Each pin goes through the fast GPIO ports (GPIO6 to GPIO9, on the core's
// own bus, mirrors of GPIO1 to GPIO4 chosen per bit in IOMUXC_GPR26..29):
// set, clear and toggle registers act on the bits written as 1 (no
// read-modify-write), and toggle is the chip's own. The level read is the
// pad's (PSR), outputs included. Open drain is the pad's own (ODE): an
// output whose 1 lets the line go.
//
// Changes of watched pins: both edges (EDGE_SEL) raise the one interrupt of
// the fast ports, which hands the pin to the watch shared by the FreeRTOS
// ports (ports/freertos/gpio_watch.h), which debounces and reports the
// changes from its event task.
#include "hal_gpio.h"
#include "gpio_watch.h"
#include "board_pins.h"

#include <stdint.h>

#include "fsl_common.h"

namespace hal {
namespace gpio {

namespace {

const int      FIRST_FAST_PORT = 6;               // GPIO6
const int      FAST_PORTS = 4;                    // GPIO6 to GPIO9
const int      PORT_BITS = 32;                    // bits of a port
// ALT5: the pad is a GPIO; SION: its input path stays on, so that PSR reads
// the pad's level for an output too (as PJRC's core does)
const uint32_t MUX_GPIO = 5 | IOMUXC_SW_MUX_CTL_PAD_SION_MASK;
const uint32_t GPIO_INTERRUPT_PRIORITY = 6;       // below configMAX_SYSCALL_INTERRUPT_PRIORITY: may wake tasks

// pad control (reference manual, SW_PAD_CTL_PAD): drive strength, speed,
// hysteresis on the input; pulls of 100 kOhm down or 22 kOhm up
const uint32_t PAD_BASE = IOMUXC_SW_PAD_CTL_PAD_DSE(6) | IOMUXC_SW_PAD_CTL_PAD_SPEED(2) | IOMUXC_SW_PAD_CTL_PAD_HYS_MASK;
const uint32_t PAD_PULL = IOMUXC_SW_PAD_CTL_PAD_PKE_MASK | IOMUXC_SW_PAD_CTL_PAD_PUE_MASK;
const uint32_t PAD_PULL_DOWN_100K = 0;
const uint32_t PAD_PULL_UP_22K = 3;
const uint32_t PAD_AT_RESET = 0x10B0;             // the pads' reset value: keeper, DSE 6, speed 2

// The registers of each fast port (index: port number - FIRST_FAST_PORT).
GPIO_Type *const FAST_PORT_BASES[FAST_PORTS] = { GPIO6, GPIO7, GPIO8, GPIO9 };

// The board's pin of each fast port bit (-1: none), for the interrupt.
int8_t pinOfBit[FAST_PORTS][PORT_BITS];   // filled once by mapBits()
bool   pinOfBitReady = false;             // pinOfBit is filled

/**
 * The fast GPIO port of a pin.
 */
GPIO_Type *portOf(int pin) {
    return FAST_PORT_BASES[board::PINS[pin].port - FIRST_FAST_PORT];
}

/**
 * The bit of a pin in its port.
 */
uint32_t bitOf(int pin) {
    return 1u << board::PINS[pin].bit;
}

/**
 * The register of IOMUXC_GPR that chooses, per bit, between GPIOn and its
 * fast mirror GPIOn+5 (GPR26 for GPIO1 / GPIO6...).
 */
volatile uint32_t *fastSelectOf(int pin) {
    volatile uint32_t *first = &IOMUXC_GPR->GPR26;
    return first + (board::PINS[pin].port - FIRST_FAST_PORT);
}

/**
 * Fills pinOfBit once.
 */
void mapBits() {
    if (pinOfBitReady) {
        return;
    }

    // no pin on any bit
    for (int port = 0; port < FAST_PORTS; port++) {
        for (int bit = 0; bit < PORT_BITS; bit++) {
            pinOfBit[port][bit] = -1;
        }
    }

    // then the bit of each pin
    for (int pin = 0; pin < board::PIN_COUNT; pin++) {
        pinOfBit[board::PINS[pin].port - FIRST_FAST_PORT][board::PINS[pin].bit] = (int8_t)pin;
    }

    pinOfBitReady = true;
}

/**
 * The pad control value of a mode and a pull.
 */
uint32_t padControl(Mode mode, Pull pull) {
    // drive, speed, hysteresis; open drain
    uint32_t value = PAD_BASE;
    if (mode == Mode::openDrain) {
        value |= IOMUXC_SW_PAD_CTL_PAD_ODE_MASK;
    }

    // the pull, if any
    if (pull == Pull::up) {
        value |= PAD_PULL | IOMUXC_SW_PAD_CTL_PAD_PUS(PAD_PULL_UP_22K);
    } else if (pull == Pull::down) {
        value |= PAD_PULL | IOMUXC_SW_PAD_CTL_PAD_PUS(PAD_PULL_DOWN_100K);
    }

    return value;
}

} // namespace

/**
 * The board's pins.
 */
int pinCount() {
    return board::PIN_COUNT;
}

/**
 * 1. the pin on its fast port, its latch at 0;
 * 2. its pad: GPIO function, drive, pulls, open drain;
 * 3. an output drives 0 from now on, an input does not drive.
 */
bool configure(int pin, Mode mode, Pull pull) {
    bool isPin = pin >= 0 && pin < board::PIN_COUNT;
    if (!isPin) {
        return false;
    }

    // the fast port, latch 0
    GPIO_Type *port = portOf(pin);
    uint32_t bit = bitOf(pin);
    *fastSelectOf(pin) |= bit;
    port->DR_CLEAR = bit;

    // the pad
    IOMUXC->SW_PAD_CTL_PAD[board::PINS[pin].control] = padControl(mode, pull);
    IOMUXC->SW_MUX_CTL_PAD[board::PINS[pin].mux] = MUX_GPIO;

    // the direction
    if (mode == Mode::input) {
        port->GDIR &= ~bit;
    } else {
        port->GDIR |= bit;
    }

    return true;
}

/**
 * The level on the pad (PSR).
 */
int read(int pin) {
    return (portOf(pin)->PSR & bitOf(pin)) != 0 ? 1 : 0;
}

/**
 * Sets or clears the output bit (open drain: 1 lets the line go).
 */
void write(int pin, int level) {
    if (level != 0) {
        portOf(pin)->DR_SET = bitOf(pin);
    } else {
        portOf(pin)->DR_CLEAR = bitOf(pin);
    }
}

/**
 * Inverts the output bit: one write to the toggle register.
 */
void toggle(int pin) {
    portOf(pin)->DR_TOGGLE = bitOf(pin);
}

/**
 * Interrupt on both edges, changes reported by the shared watch.
 */
bool watch(int pin, int debounceMs, ChangeHandler *handler) {
    // the shared watch (its event task), once
    if (!gpiowatch::start(board::PIN_COUNT, &read)) {
        return false;
    }

    // record the watch (or change its debounce)
    bool added = gpiowatch::add(pin, debounceMs, handler);
    if (!added) {
        return true;
    }

    // both edges of the pin, the shared interrupt of the fast ports
    mapBits();
    GPIO_Type *port = portOf(pin);
    uint32_t bit = bitOf(pin);
    port->EDGE_SEL |= bit;
    port->ISR = bit;
    port->IMR |= bit;
    NVIC_SetPriority(GPIO6_7_8_9_IRQn, GPIO_INTERRUPT_PRIORITY);
    EnableIRQ(GPIO6_7_8_9_IRQn);
    return true;
}

/**
 * Masks the interrupt of the pin, then forgets the watch.
 */
void unwatch(int pin) {
    GPIO_Type *port = portOf(pin);
    uint32_t bit = bitOf(pin);
    port->IMR &= ~bit;
    port->EDGE_SEL &= ~bit;
    port->ISR = bit;
    gpiowatch::remove(pin);
}

/**
 * Not watched any more, then as at boot: input, the pad's reset value.
 */
void reset(int pin) {
    bool isPin = pin >= 0 && pin < board::PIN_COUNT;
    if (!isPin) {
        return;
    }

    // not watched, then an input with the pad as at reset
    unwatch(pin);
    portOf(pin)->GDIR &= ~bitOf(pin);
    IOMUXC->SW_PAD_CTL_PAD[board::PINS[pin].control] = PAD_AT_RESET;
}

} // namespace gpio
} // namespace hal

/**
 * The interrupt of the fast GPIO ports: each pending and enabled bit is
 * acknowledged and its pin handed to the shared watch.
 */
extern "C" void GPIO6_7_8_9_IRQHandler(void) {
    for (int index = 0; index < hal::gpio::FAST_PORTS; index++) {
        // the bits pending and enabled, acknowledged
        GPIO_Type *port = hal::gpio::FAST_PORT_BASES[index];
        uint32_t pending = port->ISR & port->IMR;
        port->ISR = pending;

        while (pending != 0) {
            // the lowest pending bit, then cleared from 'pending'
            int bit = __builtin_ctz(pending);
            pending &= pending - 1;

            // its pin to the watch (a bit without pin: ignored)
            int pin = hal::gpio::pinOfBit[index][bit];
            if (pin >= 0) {
                gpiowatch::postFromInterrupt(pin);
            }
        }
    }

    // the acknowledgements done before the return (no spurious interrupt again)
    __DSB();
}
