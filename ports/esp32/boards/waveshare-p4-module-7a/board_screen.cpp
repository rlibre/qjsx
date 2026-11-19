/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display of the Waveshare ESP32-P4-Module-DEV-KIT (board_screen.h): a
// 7-DSI-TOUCH-A on its ribbon cable, a 7" panel of 720 x 1280 with an
// ILI9881C on MIPI-DSI, 2 lanes. The cable carries the DSI lanes and the
// board's I2C bus (SDA G7, SCL G8, pulled up on the board), nothing else: no
// reset pin, no backlight pin. A circuit of the display at 0x45 on that bus
// powers the panel (register 0x95) and sets its backlight (register 0x96, 0
// to 255); the program sets the backlight afterwards by writing that
// register with device/i2c.
//
// The backlight stays OFF while the display starts (user, 2026-11-19): it is
// set to 0 with the power, and to full only as the last step, once the
// controller is set up and the video runs from its black image. Before, it
// was lit from the power on, so the panel glowed for the whole set-up (the
// link, the 197 commands of the controller), about a second and a half
// during which it shows nothing clean. This is the end of the board's own
// start, just before the agent and the program: a program finds the light
// on, as before. A display that fails to start stays dark.
//
// The power sequence, the link and the video settings are those of
// Waveshare's code for this display (Waveshare-ESP32-components,
// display/lcd/esp_lcd_ili9881c and bsp/esp32_p4_platform, Apache-2.0: 2 lanes
// at 1000 Mb/s, pixel clock 80 MHz, porches H 239 / 50 / 33, V 20 / 30 / 2;
// about 57 images a second), the regulator of the PHY its LDO 3 at 2.5 V.
//
// Its touch controller (a GT9271, read as a GT911) is on the same bus,
// without interrupt nor reset pin: the program reads it with stdlib/touch.
#include "board_screen.h"

#include <stdio.h>

#include "hal_dsi.h"
#include "hal_i2c.h"
#include "ili9881c.h"
#include "platform.h"

namespace board {

namespace {

// The board's I2C bus and the circuit of the display that powers the panel.
const int      I2C_BUS = 0;
const int      I2C_SDA = 7;
const int      I2C_SCL = 8;
const int      POWER_ADDRESS = 0x45;
const uint32_t POWER_HZ = 100000;
const int      POWER_TIMEOUT_MS = 50;

// Its registers (their bits are not documented: the values of Waveshare's
// code).
const uint8_t POWER_REGISTER = 0x95;
const uint8_t POWER_FIRST = 0x11;
const uint8_t POWER_ON = 0x17;
const uint8_t BACKLIGHT_REGISTER = 0x96;
const uint8_t BACKLIGHT_OFF = 0x00;
const uint8_t BACKLIGHT_FULL = 0xFF;

// The waits: the controller after its power on and after its software reset
// (the controller's).
const int AFTER_POWER_MS = 120;
const int AFTER_RESET_MS = 120;

// The software reset of the controller (MIPI DCS): it has no reset pin.
const uint8_t SOFTWARE_RESET = 0x01;

// The link and the video of the panel.
const hal::dsi::Settings PANEL = {
    2,                             // lanes
    1000,                          // Mb/s a lane
    80,                            // pixel clock, MHz
    720,                           // width
    1280,                          // height
    50, 239, 33,                   // horizontal sync, back porch, front porch
    30, 20, 2,                     // vertical sync, back porch, front porch
    3, 2500,                       // the LDO of the PHY, mV
};

Screen started = {};               // the display, once started
bool   isStarted = false;

/**
 * Writes 'value' into the register 'index' of the power circuit; false when
 * it does not answer.
 */
bool writePower(int device, uint8_t index, uint8_t value) {
    const uint8_t bytes[2] = { index, value };
    return hal::i2c::transfer(device, bytes, sizeof(bytes), nullptr, 0, POWER_TIMEOUT_MS) == hal::i2c::Status::ok;
}

/**
 * Opens the board's I2C bus and the power circuit of the display on it.
 * Returns the device of the circuit, or -1 after printing why (the bus is
 * then closed).
 */
int openPowerCircuit() {
    // the bus (pulled up on the board)
    if (!hal::i2c::openBus(I2C_BUS, I2C_SDA, I2C_SCL, false)) {
        printf("qjsx: screen: the I2C bus of the board is refused\n");
        return -1;
    }

    // the circuit on it
    int device = hal::i2c::addDevice(I2C_BUS, POWER_ADDRESS, POWER_HZ);
    if (device < 0) {
        hal::i2c::closeBus(I2C_BUS);
        printf("qjsx: screen: the display's circuit cannot be added to the I2C bus\n");
    }

    return device;
}

/**
 * Gives the bus back, for the programs.
 */
void closePowerCircuit(int device) {
    hal::i2c::removeDevice(device);
    hal::i2c::closeBus(I2C_BUS);
}

/**
 * Powers the panel through the circuit of the display, its backlight OFF,
 * and waits for the panel's controller. False after printing why.
 */
bool powerPanel(int device) {
    // the power, the backlight off
    bool powered = writePower(device, POWER_REGISTER, POWER_FIRST)
                   && writePower(device, POWER_REGISTER, POWER_ON)
                   && writePower(device, BACKLIGHT_REGISTER, BACKLIGHT_OFF);
    if (!powered) {
        printf("qjsx: screen: the display's circuit at 0x%02x does not answer (ribbon cable?)\n", POWER_ADDRESS);
        return false;
    }

    platform::sleepMs(AFTER_POWER_MS);
    return true;
}

/**
 * Resets the controller by its command (no reset pin) and waits for it.
 * False when the command could not be sent.
 */
bool resetController() {
    if (!hal::dsi::command(SOFTWARE_RESET, nullptr, 0)) {
        return false;
    }

    platform::sleepMs(AFTER_RESET_MS);
    return true;
}

} // namespace

bool startScreen() {
    // the circuit that powers the panel and sets its backlight, kept for the
    // whole start
    int circuit = openPowerCircuit();
    if (circuit < 0) {
        return false;
    }

    // in the dark: the panel powered, the link open, the controller reset
    // and set up
    bool ready = powerPanel(circuit)
                 && hal::dsi::open(&PANEL)
                 && resetController()
                 && drivers::ili9881c::start(&drivers::ili9881c::WAVESHARE_7_DSI_TOUCH_A, &hal::dsi::command);
    if (!ready) {
        printf("qjsx: screen: the 7-DSI-TOUCH-A display could not be set up\n");
    }

    // the video, from its black image
    uint16_t *pixels = ready ? hal::dsi::startVideo() : nullptr;

    // the last step: the light, on a display that shows its image (a display
    // that did not start stays dark); then the bus given back
    if (pixels != nullptr) {
        writePower(circuit, BACKLIGHT_REGISTER, BACKLIGHT_FULL);
    }

    closePowerCircuit(circuit);
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

// The touch panel: the program reads the GT9271 itself (stdlib/touch over
// device/i2c): none read by the board.
bool touchInfo(TouchInfo *) {
    return false;
}

void touchWatch(TouchChanged *, void *) {
}

int touchRead(TouchPoint *, int) {
    return -1;
}

void flushScreen(ScreenArea area) {
    // the whole rows of the area: the cache is written back by lines, a
    // row is contiguous
    uint16_t *first = started.pixels + (size_t)area.y * (size_t)started.width;
    hal::dsi::flush(first, (size_t)area.height * (size_t)started.width * sizeof(uint16_t));
}

} // namespace board
