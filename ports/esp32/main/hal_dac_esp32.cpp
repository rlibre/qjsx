/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Analog outputs on the ESP32 (hal_dac.h), with the oneshot driver of
// ESP-IDF (esp_driver_dac, driver/dac_oneshot.h), on the chips that have a
// DAC only (ports/esp32/main/CMakeLists.txt: QJSX_DAC_TARGETS):
//   - two 8-bit channels on fixed pins: GPIO25 / GPIO26 on the original
//     ESP32, GPIO17 / GPIO18 on the S2;
//   - a code c gives about c / 255 of VDD3P3_RTC: the full scale is the
//     board's (DAC_FULL_SCALE_MV of board_config.h, 3.3 V on our boards);
//   - closed: the channel deleted (its output disabled), then the pin reset
//     (gpio_reset_pin), as the other hals give their pins back.
// One channel handle per open output.
#include "hal_dac.h"

#include <errno.h>

#include "board_config.h"
#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

namespace {

const int CHANNELS = 2;                  // DAC channels of the chip
const int BITS = 8;                      // resolution of each

// A channel of the chip and its pin.
struct Channel {
    dac_channel_t id;                    // of ESP-IDF
    int           pin;                   // its GPIO (fixed by the chip)
};

#if CONFIG_IDF_TARGET_ESP32
const Channel CHANNEL_PINS[CHANNELS] = {
    { DAC_CHAN_0, 25 },
    { DAC_CHAN_1, 26 },
};
#elif CONFIG_IDF_TARGET_ESP32S2
const Channel CHANNEL_PINS[CHANNELS] = {
    { DAC_CHAN_0, 17 },
    { DAC_CHAN_1, 18 },
};
#else
#error "hal_dac_esp32.cpp is for the chips with a DAC only (QJSX_DAC_TARGETS of main/CMakeLists.txt)"
#endif

// An open output, by its channel (the handle is its index).
struct Output {
    bool                 used;           // open() gave it
    dac_oneshot_handle_t channel;        // of the driver
};

Output outputs[CHANNELS];                // JS thread only

/**
 * The index of the channel on 'pin'; -1 if the pin has no DAC.
 */
int channelOf(int pin) {
    for (int i = 0; i < CHANNELS; i++) {
        if (CHANNEL_PINS[i].pin == pin) {
            return i;
        }
    }

    return -1;
}

/**
 * An open output by its handle; nullptr if there is none.
 */
Output *outputOf(int handle) {
    bool inTable = handle >= 0 && handle < CHANNELS;
    if (!inTable || !outputs[handle].used) {
        return nullptr;
    }

    return &outputs[handle];
}

} // namespace

namespace hal {
namespace dac {

/**
 * Opens the DAC of a pin:
 * 1. the pin must be the pin of a channel, not open yet;
 * 2. the channel made by the driver (it takes the pin), at code 0.
 */
Opened open(int pin) {
    // a pin with a DAC, free
    int index = channelOf(pin);
    if (index < 0) {
        return Opened{ -1, EINVAL, 0, 0 };
    }

    if (outputs[index].used) {
        return Opened{ -1, EBUSY, 0, 0 };
    }

    // the channel; another driver holding it (the continuous or cosine
    // mode, unused by qjsx) would be an invalid state: taken
    dac_oneshot_config_t config = {};
    config.chan_id = CHANNEL_PINS[index].id;
    dac_oneshot_handle_t channel = nullptr;
    esp_err_t made = dac_oneshot_new_channel(&config, &channel);
    if (made != ESP_OK) {
        int error = made == ESP_ERR_INVALID_STATE ? EBUSY : EIO;
        return Opened{ -1, error, 0, 0 };
    }

    // at code 0 until the first write
    dac_oneshot_output_voltage(channel, 0);
    outputs[index] = Output{ true, channel };
    return Opened{ index, 0, BITS, board::DAC_FULL_SCALE_MV };
}

/**
 * Drives a code.
 */
int write(int handle, int raw) {
    // an open output
    Output *output = outputOf(handle);
    if (output == nullptr) {
        return EINVAL;
    }

    // the code (checked by the module: 0 to 255)
    bool written = dac_oneshot_output_voltage(output->channel, (uint8_t)raw) == ESP_OK;
    return written ? 0 : EIO;
}

/**
 * Stops an output: the channel deleted, the pin reset.
 */
void close(int handle) {
    // an open output
    Output *output = outputOf(handle);
    if (output == nullptr) {
        return;
    }

    // the channel goes, the pin is given back
    dac_oneshot_del_channel(output->channel);
    gpio_reset_pin((gpio_num_t)CHANNEL_PINS[handle].pin);
    *output = Output{};
}

} // namespace dac
} // namespace hal
