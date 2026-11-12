/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The hardware modules of the boards (device/...), written once on top of the
// hal interfaces (common/hal/). A port compiles those whose hal it implements
// (ports/cmake/qjsx.cmake: QJSX_DEVICE_SOURCES) and registers them in its
// entry point, before agent::runDevice():
//
//   rt::registerNativeModule("device/gpio", &jsNewModule_gpio);
//
// The desktop compiles them for the digital twin of a board (host/twin/sim
// implements their hal). device/serial is apart: every executable has it,
// with the driver of its ports (hal_serial.h, setSerialDriver).
#pragma once

#include "quickjs.h"
#include "hal_serial.h"

/**
 * Creates the module "device/gpio" (gpio_module.cpp, JS_MODULE): the class
 * Gpio. Needs hal_gpio.h.
 */
JSModuleDef *jsNewModule_gpio(JSContext *ctx, const char *moduleName);

/**
 * Creates the module "device/rmt" (rmt_module.cpp, JS_MODULE): the class
 * Rmt. Needs hal_rmt.h.
 */
JSModuleDef *jsNewModule_rmt(JSContext *ctx, const char *moduleName);

/**
 * Creates the module "device/usb" (usb_module.cpp, JS_MODULE): the classes
 * Usb and UsbCdc. Needs hal_usb.h.
 */
JSModuleDef *jsNewModule_usb(JSContext *ctx, const char *moduleName);

/**
 * Creates the module "device/i2c" (i2c_module.cpp, JS_MODULE): the classes
 * I2c and I2cDevice. Needs hal_i2c.h.
 */
JSModuleDef *jsNewModule_i2c(JSContext *ctx, const char *moduleName);

/**
 * Creates the module "device/adc" (adc_module.cpp, JS_MODULE): the class
 * Adc. Needs hal_adc.h.
 */
JSModuleDef *jsNewModule_adc(JSContext *ctx, const char *moduleName);

/**
 * Creates the module "device/serial" (serial_module.cpp, JS_MODULE): list()
 * and the class Serial, through the driver given to setSerialDriver().
 */
JSModuleDef *jsNewModule_serial(JSContext *ctx, const char *moduleName);

/**
 * Chooses the driver of device/serial: the PC's ports, a board's UARTs, or
 * the twin's (hal_serial.h). Before any JS runtime exists; the driver is
 * static.
 */
void setSerialDriver(const hal::serial::Driver *driver);
