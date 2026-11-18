/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display of the M5Stack Tab5 (board_screen.h), its first version: a 5"
// panel of 720 x 1280 with an ILI9881C on MIPI-DSI, 2 lanes. Its reset pin
// is pin 4 of the expander PI4IOE5V6408 at 0x43 on the system I2C bus (SDA
// G31, SCL G32); its backlight is G22, driven by the program (stdlib/display,
// a PWM). The link and video settings are those of M5Stack's code for the
// Tab5 (M5Tab5-UserDemo, Apache-2.0: 2 lanes at 730 Mb/s, pixel clock
// 60 MHz, porches H 140 / 40 / 40, V 20 / 4 / 20; about 48 images a second),
// the regulator of the PHY its LDO 3 at 2.5 V.
//
// Its touch controller, a GT911 at 0x14 on the same bus (interrupt G23, reset
// pin 5 of the same expander), is started here too: without its start
// sequence it answers on I2C but has no configuration and reports nothing.
// The program then reads it with stdlib/touch.
#include "board_screen.h"

#include <stdio.h>

#include "hal_dsi.h"
#include "hal_gpio.h"
#include "hal_i2c.h"
#include "ili9881c.h"
#include "pi4ioe5v6408.h"
#include "platform.h"

namespace board {

namespace {

// The system I2C bus and the expander that holds the reset of the panel.
const int      I2C_BUS = 0;
const int      I2C_SDA = 31;
const int      I2C_SCL = 32;
const int      EXPANDER_ADDRESS = 0x43;
const uint32_t EXPANDER_HZ = 400000;
const int      PANEL_RESET_PIN = 4;
const int      TOUCH_RESET_PIN = 5;

// The interrupt pin of the GT911: during its reset, its level chooses the
// address (high: 0x14), then held low a moment (the GT911's start sequence).
const int TOUCH_INTERRUPT = 23;
const int TOUCH_SETTLE_MS = 10;    // reset released, before INT goes low
const int TOUCH_SYNC_MS = 50;      // INT held low

// How long the reset pulse lasts, and the wait after it (the controller's).
const int RESET_PULSE_MS = 10;
const int AFTER_RESET_MS = 120;

// The link and the video of the panel.
const hal::dsi::Settings PANEL = {
    2,                             // lanes
    730,                           // Mb/s a lane
    60,                            // pixel clock, MHz
    720,                           // width
    1280,                          // height
    40, 140, 40,                   // horizontal sync, back porch, front porch
    4, 20, 20,                     // vertical sync, back porch, front porch
    3, 2500,                       // the LDO of the PHY, mV
};

Screen started = {};               // the display, once started
bool   isStarted = false;

/**
 * Pulses the reset pins of the panel and of the touch controller (through
 * the expander), the touch's interrupt pin high meanwhile (its address
 * 0x14), then gives the GT911 its start signal (INT low a moment) and waits
 * for the panel's controller. The bus and the interrupt pin are given back
 * at the end, for the programs. False after printing why.
 */
bool resetPanel() {
    // the bus and the expander
    if (!hal::i2c::openBus(I2C_BUS, I2C_SDA, I2C_SCL, true)) {
        printf("qjsx: screen: the system I2C bus is refused\n");
        return false;
    }

    int expander = hal::i2c::addDevice(I2C_BUS, EXPANDER_ADDRESS, EXPANDER_HZ);
    bool pulsed = false;
    if (expander >= 0) {
        // both reset pins low a moment, INT high (the GT911 at 0x14)
        hal::gpio::configure(TOUCH_INTERRUPT, hal::gpio::Mode::output, hal::gpio::Pull::none);
        hal::gpio::write(TOUCH_INTERRUPT, 1);
        pulsed = drivers::pi4ioe5v6408::drive(expander, PANEL_RESET_PIN, false);
        pulsed = drivers::pi4ioe5v6408::drive(expander, TOUCH_RESET_PIN, false) && pulsed;
        platform::sleepMs(RESET_PULSE_MS);

        // released to their pull-ups
        pulsed = drivers::pi4ioe5v6408::release(expander, PANEL_RESET_PIN) && pulsed;
        pulsed = drivers::pi4ioe5v6408::release(expander, TOUCH_RESET_PIN) && pulsed;
        hal::i2c::removeDevice(expander);

        // the GT911's start signal: INT low a moment, then an input again
        platform::sleepMs(TOUCH_SETTLE_MS);
        hal::gpio::write(TOUCH_INTERRUPT, 0);
        platform::sleepMs(TOUCH_SYNC_MS);
        hal::gpio::reset(TOUCH_INTERRUPT);
    }

    hal::i2c::closeBus(I2C_BUS);
    if (!pulsed) {
        printf("qjsx: screen: the expander at 0x%02x does not answer\n", EXPANDER_ADDRESS);
        return false;
    }

    platform::sleepMs(AFTER_RESET_MS);
    return true;
}

} // namespace

bool startScreen() {
    // the controller reset, the link open, the controller set up
    bool ready = resetPanel()
                 && hal::dsi::open(&PANEL)
                 && drivers::ili9881c::start(&drivers::ili9881c::M5STACK_TAB5, &hal::dsi::command);
    if (!ready) {
        printf("qjsx: screen: the display of the Tab5 could not be set up\n");
        return false;
    }

    // the video, from its image
    uint16_t *pixels = hal::dsi::startVideo();
    if (pixels == nullptr) {
        return false;
    }

    started = Screen{ pixels, PANEL.width, PANEL.height };
    isStarted = true;
    return true;
}

const Screen *screen() {
    return isStarted ? &started : nullptr;
}

void flushScreen(ScreenArea area) {
    // the whole rows of the area: the cache is written back by lines, a
    // row is contiguous
    uint16_t *first = started.pixels + (size_t)area.y * (size_t)started.width;
    hal::dsi::flush(first, (size_t)area.height * (size_t)started.width * sizeof(uint16_t));
}

} // namespace board
