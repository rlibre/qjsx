/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Analog inputs on the ESP32 (hal_adc.h), with the oneshot driver of
// ESP-IDF (esp_adc):
//   - ADC1 only (user, 2026-11-12): ADC2 does not work while the WiFi is on;
//     a pin of ADC2, or without analog input, is refused (EINVAL);
//   - the range asked chooses the attenuation: the smallest whose readable
//     span covers it (0 dB ~0.95 V, 2.5 dB ~1.25 V, 6 dB ~1.75 V, 12 dB
//     ~3.1 V, read up to 3.3 V: above 3.1 V the reads saturate);
//   - millivolts through the calibration of the chip (curve fitting where
//     the chip has it, line fitting else); without calibration, a straight
//     line over the span.
// One unit handle for ADC1, shared by the open inputs.
#include "hal_adc.h"

#include <errno.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

namespace {

const int MAX_INPUTS = 10;               // ADC1 has 10 channels on the ESP32-S3
const int HIGHEST_MV = 3300;             // the highest range accepted

// An attenuation and the span it reads.
struct Attenuation {
    adc_atten_t atten;                   // the setting of the channel
    int spanMv;                          // what it reads, about
};

// From the smallest span to the largest (ESP-IDF, "ADC attenuation").
const Attenuation ATTENUATIONS[] = {
    { ADC_ATTEN_DB_0, 950 },
    { ADC_ATTEN_DB_2_5, 1250 },
    { ADC_ATTEN_DB_6, 1750 },
    { ADC_ATTEN_DB_12, 3100 },
};

// An open input.
struct Input {
    bool used;                           // open() gave it
    int pin;                             // its GPIO
    adc_channel_t channel;               // its channel of ADC1
    int spanMv;                          // the span of its attenuation
    adc_cali_handle_t calibration;       // nullptr: none (a straight line)
};

adc_oneshot_unit_handle_t unit = nullptr;   // ADC1, made at the first open
int unitUsers = 0;                          // inputs open on it
Input inputs[MAX_INPUTS];

/**
 * The attenuation for a range: the first whose span covers it, the largest
 * up to HIGHEST_MV.
 */
Attenuation attenuationFor(int rangeMv) {
    for (const Attenuation &attenuation : ATTENUATIONS) {
        if (rangeMv <= attenuation.spanMv) {
            return attenuation;
        }
    }
    return ATTENUATIONS[sizeof(ATTENUATIONS) / sizeof(ATTENUATIONS[0]) - 1];
}

/**
 * The calibration of a channel at an attenuation, by the scheme the chip
 * has; nullptr when none.
 */
adc_cali_handle_t makeCalibration(adc_channel_t channel, adc_atten_t atten) {
    adc_cali_handle_t handle = nullptr;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve = {};
    curve.unit_id = ADC_UNIT_1;
    curve.chan = channel;
    curve.atten = atten;
    curve.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&curve, &handle) == ESP_OK) {
        return handle;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line = {};
    line.unit_id = ADC_UNIT_1;
    line.atten = atten;
    line.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&line, &handle) == ESP_OK) {
        return handle;
    }
#endif
    (void)channel;
    (void)atten;
    return nullptr;
}

/**
 * Lets the calibration of an input go.
 */
void deleteCalibration(adc_cali_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(handle);
#endif
}

/**
 * An open input by its handle; nullptr if there is none.
 */
Input *inputOf(int handle) {
    if (handle < 0 || handle >= MAX_INPUTS || !inputs[handle].used) {
        return nullptr;
    }
    return &inputs[handle];
}

} // namespace

namespace hal {
namespace adc {

/**
 * Opens the analog input of a pin of ADC1:
 * 1. the pin must be a channel of ADC1, not open yet, the range readable;
 * 2. the unit made at the first input;
 * 3. the channel set for the attenuation of the range, its calibration.
 */
Opened open(int pin, int rangeMv) {
    Opened refused = { -1, EINVAL, 0, 0 };

    // a channel of ADC1, free, a range it can read
    adc_unit_t unitId = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    bool isAdc1 = adc_oneshot_io_to_channel(pin, &unitId, &channel) == ESP_OK && unitId == ADC_UNIT_1;
    if (!isAdc1 || rangeMv <= 0 || rangeMv > HIGHEST_MV) {
        return refused;
    }

    int free = -1;
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (inputs[i].used && inputs[i].pin == pin) {
            return Opened{ -1, EBUSY, 0, 0 };
        }
        if (!inputs[i].used && free < 0) {
            free = i;
        }
    }
    if (free < 0) {
        return Opened{ -1, EBUSY, 0, 0 };
    }

    // the unit, shared
    if (unit == nullptr) {
        adc_oneshot_unit_init_cfg_t unitConfig = {};
        unitConfig.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&unitConfig, &unit) != ESP_OK) {
            unit = nullptr;
            return Opened{ -1, EIO, 0, 0 };
        }
    }

    // the channel, at the attenuation of the range
    Attenuation attenuation = attenuationFor(rangeMv);
    adc_oneshot_chan_cfg_t channelConfig = {};
    channelConfig.atten = attenuation.atten;
    channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit, channel, &channelConfig) != ESP_OK) {
        return Opened{ -1, EIO, 0, 0 };
    }

    inputs[free] = Input{ true, pin, channel, attenuation.spanMv, makeCalibration(channel, attenuation.atten) };
    unitUsers++;
    return Opened{ free, 0, SOC_ADC_RTC_MAX_BITWIDTH, attenuation.spanMv };
}

/**
 * Closes an input; the unit goes with the last one.
 */
void close(int handle) {
    Input *input = inputOf(handle);
    if (input == nullptr) {
        return;
    }

    deleteCalibration(input->calibration);
    *input = Input{};
    unitUsers--;
    if (unitUsers == 0 && unit != nullptr) {
        adc_oneshot_del_unit(unit);
        unit = nullptr;
    }
}

/**
 * A raw conversion.
 */
int readRaw(int handle) {
    Input *input = inputOf(handle);
    int raw = 0;
    if (input == nullptr || adc_oneshot_read(unit, input->channel, &raw) != ESP_OK) {
        return -1;
    }
    return raw;
}

/**
 * A conversion in millivolts: the calibration, else a straight line over
 * the span.
 */
int readMillivolts(int handle) {
    // the raw value
    int raw = readRaw(handle);
    if (raw < 0) {
        return -1;
    }

    // calibrated
    Input *input = inputOf(handle);
    int millivolts = 0;
    if (input->calibration != nullptr && adc_cali_raw_to_voltage(input->calibration, raw, &millivolts) == ESP_OK) {
        return millivolts;
    }

    // else a straight line
    int maxRaw = (1 << SOC_ADC_RTC_MAX_BITWIDTH) - 1;
    return (int)((int64_t)raw * input->spanMv / maxRaw);
}

} // namespace adc
} // namespace hal
