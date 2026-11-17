/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// PWM outputs on the i.MX RT1062 (hal_pwm.h), design agreed 2026-11-16. A pin
// is driven by a FlexPWM submodule (output A, B or X) or by a QuadTimer
// channel (boards/<board>/board_pwm.h); the registers are written directly,
// as PJRC's core does (teensy4/pwm.c).
//
//   - a generator (submodule or channel) counts IPG clock ticks (150 MHz),
//     divided by 2^prescale (0 to 7), 'counts' ticks per period (16 bits):
//     the smallest prescale whose count fits; out of reach below 2 ticks or
//     beyond the largest prescale (about 18 Hz to 75 MHz);
//   - the duty cycle is a whole number of ticks; 'bits' is the number of
//     whole bits of 'counts' (floor of log2);
//   - a FlexPWM submodule has one counter for its outputs A, B and X: the
//     first output opened sets its frequency, another one at the same
//     frequency shares it, at another one it is refused (EBUSY);
//   - a new duty cycle takes effect at the next period (FlexPWM: buffered
//     values loaded at the end of the period, LDOK; QuadTimer: load
//     registers);
//   - invert: the hardware's output polarity;
//   - closed: the pin becomes a GPIO output at its idle level (0, or 1 when
//     inverted), the generator is freed when nobody uses it.
//
// The QuadTimer makes the high time and the low time as two loads of its
// counter (CMPLD1 and LOAD), neither may be 0: a duty cycle of exactly 0 or 1
// is made by driving the pin as a GPIO at that level, the timer left aside.
// A FlexPWM X output cannot be fully on: at most counts - 1 ticks.
#include "hal_pwm.h"
#include "hal_gpio.h"
#include "board_pwm.h"
#include "board_pins.h"

#include <errno.h>
#include <math.h>

#include "fsl_clock.h"
#include "fsl_common.h"

namespace hal {
namespace pwm {

namespace {

// ### Constants ###############################################################

const int      MODULES = 4;                       // FlexPWM1..4, TMR1..4
const int      UNITS = 4;                         // submodules or channels per module
const int      MAX_PRESCALE = 7;                  // the clock divided by 2^0..2^7
const uint32_t MIN_COUNTS = 2;                    // ticks of the shortest period (1 bit)
const uint32_t FLEXPWM_MAX_COUNTS = 65535;        // VAL1 = counts - 1, and a duty of counts fits 16 bits
const uint32_t QUADTIMER_MAX_COUNTS = 65534;      // as PJRC's core
const uint32_t QUADTIMER_LOAD_BASE = 65537;       // LOAD = 65537 - low ticks (PJRC's core)
const uint16_t QUADTIMER_PCS_IPG = 8;             // primary count source: the IPG clock, + prescale
const uint16_t QUADTIMER_SET_ON_COMPARE = 6;      // OUTMODE: set on compare, cleared on rollover
// pad of a PWM output: drive strength, speed
const uint32_t PAD_OUTPUT = IOMUXC_SW_PAD_CTL_PAD_DSE(6) | IOMUXC_SW_PAD_CTL_PAD_SPEED(2);

// The registers and the clock gates of each module (index: module number - 1).
PWM_Type *const FLEXPWM[MODULES] = { PWM1, PWM2, PWM3, PWM4 };
TMR_Type *const QUADTIMER[MODULES] = { TMR1, TMR2, TMR3, TMR4 };
const clock_ip_name_t FLEXPWM_CLOCKS[MODULES] = { kCLOCK_Pwm1, kCLOCK_Pwm2, kCLOCK_Pwm3, kCLOCK_Pwm4 };
const clock_ip_name_t QUADTIMER_CLOCKS[MODULES] = { kCLOCK_Timer1, kCLOCK_Timer2, kCLOCK_Timer3, kCLOCK_Timer4 };

// ### State (JS thread) #######################################################

// How a generator makes its frequency.
struct Setting {
    int      prescale;    // the clock divided by 2^prescale
    uint32_t counts;      // ticks per period
};

// A FlexPWM submodule or a QuadTimer channel.
struct Generator {
    int      users;       // outputs on it; 0: free
    uint32_t frequency;   // Hz, as asked by its first output
    Setting  setting;     // how it makes it
};

// An output; its handle is its pin.
struct Channel {
    const board::PwmPin *pwm;        // its line of the board's table; nullptr: not open
    bool                 invert;     // active low
    double               duty;       // 0 to 1, as asked last
    bool                 counting;   // QuadTimer: its counter runs and drives the pad (else the pin is a GPIO)
};

Generator flexGenerators[MODULES][UNITS];   // FlexPWM submodules, by module - 1 and submodule
Generator quadGenerators[MODULES][UNITS];   // QuadTimer channels, by module - 1 and channel
bool      flexModuleStarted[MODULES];       // its clock is on (startFlexModule)
bool      quadModuleStarted[MODULES];       // its clock is on (startQuadModule)
Channel   channels[board::PIN_COUNT];       // the outputs, by pin (= handle)

// ### Helpers #################################################################

/**
 * The PWM line of a pin, nullptr if it has no PWM output.
 */
const board::PwmPin *findPwm(int pin) {
    for (int i = 0; i < board::PWM_PIN_COUNT; i++) {
        if (board::PWM_PINS[i].pin == pin) {
            return &board::PWM_PINS[i];
        }
    }

    return nullptr;
}

/**
 * The generator of a PWM line.
 */
Generator *generatorOf(const board::PwmPin *pwm) {
    bool flex = pwm->kind == board::PwmKind::flexPwm;
    Generator (*table)[UNITS] = flex ? flexGenerators : quadGenerators;
    return &table[pwm->module - 1][pwm->unit];
}

/**
 * The IPG clock, which the generators count.
 */
uint32_t clockHz() {
    return CLOCK_GetFreq(kCLOCK_IpgClk);
}

/**
 * How a generator of 'kind' makes 'frequency': the smallest prescale whose
 * count fits. False when out of reach.
 */
bool settingFor(board::PwmKind kind, uint32_t frequency, Setting *out) {
    if (frequency == 0) {
        return false;
    }

    // ticks per period at the undivided clock, then halved until they fit
    uint32_t maxCounts = kind == board::PwmKind::flexPwm ? FLEXPWM_MAX_COUNTS : QUADTIMER_MAX_COUNTS;
    double ticks = (double)clockHz() / frequency;
    int prescale = 0;

    while (ticks > maxCounts + 0.5 && prescale < MAX_PRESCALE) {
        ticks /= 2;
        prescale++;
    }

    // too short a period, or too long even at the largest prescale
    uint32_t counts = (uint32_t)lround(ticks);
    bool outOfReach = counts < MIN_COUNTS || counts > maxCounts;
    if (outOfReach) {
        return false;
    }

    *out = Setting{ prescale, counts };
    return true;
}

/**
 * The duty cycle as ticks of the period, rounded.
 */
uint32_t dutyTicks(double duty, uint32_t counts) {
    // the duty cycle brought back into 0..1
    double clamped = duty;
    if (duty < 0) {
        clamped = 0;
    } else if (duty > 1) {
        clamped = 1;
    }

    return (uint32_t)lround(clamped * counts);
}

/**
 * The high ticks a QuadTimer makes for 'ticks' (0 and counts are made as a
 * GPIO level instead: see the top of the file).
 */
uint32_t quadHighTicks(uint32_t ticks, uint32_t counts) {
    // neither load may be 0: 1 to counts - 1 ticks
    uint32_t highest = counts - 1;
    if (ticks < 1) {
        return 1;
    }

    if (ticks > highest) {
        return highest;
    }

    return ticks;
}

/**
 * The duty cycle a channel really makes, in ticks.
 */
uint32_t madeTicks(const Channel *channel, const Setting *setting) {
    uint32_t ticks = dutyTicks(channel->duty, setting->counts);
    bool fullOrNone = ticks == 0 || ticks == setting->counts;

    // a QuadTimer between 0 and 1: its two loads, neither 0
    bool isQuad = channel->pwm->kind == board::PwmKind::quadTimer;
    if (isQuad && !fullOrNone) {
        return quadHighTicks(ticks, setting->counts);
    }

    // a FlexPWM X output cannot be fully on
    bool isX = channel->pwm->output == board::PwmOutput::x;
    if (isX && ticks == setting->counts) {
        return setting->counts - 1;
    }

    // else exactly the ticks asked (a QuadTimer at 0 or 1: a GPIO level)
    return ticks;
}

/**
 * What an output does: its generator's frequency, its duty cycle in ticks,
 * the whole bits of its period.
 */
Output outputOf(const Channel *channel) {
    const Setting *setting = &generatorOf(channel->pwm)->setting;
    double frequency = (double)clockHz() / (double)(1u << setting->prescale) / setting->counts;
    double duty = (double)madeTicks(channel, setting) / setting->counts;
    int bits = (int)floor(log2((double)setting->counts));
    return Output{ frequency, duty, bits };
}

/**
 * Drives the pin as a GPIO output at 'level' (idle level, or a duty cycle of
 * exactly 0 or 1 on a QuadTimer).
 */
void driveAsGpio(int pin, int level) {
    gpio::configure(pin, gpio::Mode::output, gpio::Pull::none);
    gpio::write(pin, level);
}

/**
 * Connects the pin's pad to its PWM output.
 */
void connectPad(const board::PwmPin *pwm) {
    const board::PinMap *map = &board::PINS[pwm->pin];
    IOMUXC->SW_PAD_CTL_PAD[map->control] = PAD_OUTPUT;
    IOMUXC->SW_MUX_CTL_PAD[map->mux] = pwm->mux;
}

// ### FlexPWM #################################################################

/**
 * Starts a FlexPWM module once: its clock, no fault (the fault inputs are
 * low, a high level would be one; the faults disable no output).
 */
void startFlexModule(int module) {
    if (flexModuleStarted[module - 1]) {
        return;
    }

    // the clock, then the fault inputs: active when high, flags cleared, no filter
    PWM_Type *flex = FLEXPWM[module - 1];
    CLOCK_EnableClock(FLEXPWM_CLOCKS[module - 1]);
    flex->FCTRL = PWM_FCTRL_FLVL(0xF);
    flex->FSTS = PWM_FSTS_FFLAG(0xF);
    flex->FFILT = 0;

    // no fault disables an output of any submodule
    for (int unit = 0; unit < UNITS; unit++) {
        flex->SM[unit].DISMAP[0] = 0;
    }

    flexModuleStarted[module - 1] = true;
}

/**
 * Sets a submodule up for its first output: independent outputs, counting
 * from 0, no dead time, every output off.
 */
void startFlexUnit(const board::PwmPin *pwm) {
    PWM_Type *flex = FLEXPWM[pwm->module - 1];
    int unit = pwm->unit;

    // the buffered values may be written, then every value at rest
    flex->MCTRL |= PWM_MCTRL_CLDOK(1u << unit);
    flex->SM[unit].CTRL2 = PWM_CTRL2_INDEP_MASK | PWM_CTRL2_WAITEN_MASK | PWM_CTRL2_DBGEN_MASK;
    flex->SM[unit].INIT = 0;
    flex->SM[unit].DTCNT0 = 0;
    flex->SM[unit].OCTRL = 0;
    flex->SM[unit].VAL0 = 0;
    flex->SM[unit].VAL2 = 0;
    flex->SM[unit].VAL3 = 0;
    flex->SM[unit].VAL4 = 0;
    flex->SM[unit].VAL5 = 0;
}

/**
 * Programs a FlexPWM output: its submodule's period and prescale, its duty
 * cycle, its polarity, all loaded together at the end of the period (LDOK),
 * then the output and the submodule enabled.
 */
void programFlex(const Channel *channel) {
    const board::PwmPin *pwm = channel->pwm;
    const Setting *setting = &generatorOf(pwm)->setting;
    PWM_Type *flex = FLEXPWM[pwm->module - 1];
    int unit = pwm->unit;
    uint16_t mask = (uint16_t)(1u << unit);
    uint32_t ticks = madeTicks(channel, setting);

    // the buffered values may be written
    flex->MCTRL |= PWM_MCTRL_CLDOK(mask);

    // the period
    flex->SM[unit].CTRL = PWM_CTRL_FULL_MASK | PWM_CTRL_PRSC(setting->prescale);
    flex->SM[unit].VAL1 = (uint16_t)(setting->counts - 1);

    // the duty cycle and the polarity of the output
    uint16_t polarity = 0;
    uint16_t enable = 0;
    if (pwm->output == board::PwmOutput::a) {
        flex->SM[unit].VAL2 = 0;
        flex->SM[unit].VAL3 = (uint16_t)ticks;
        polarity = PWM_OCTRL_POLA_MASK;
        enable = PWM_OUTEN_PWMA_EN(mask);
    } else if (pwm->output == board::PwmOutput::b) {
        flex->SM[unit].VAL4 = 0;
        flex->SM[unit].VAL5 = (uint16_t)ticks;
        polarity = PWM_OCTRL_POLB_MASK;
        enable = PWM_OUTEN_PWMB_EN(mask);
    } else {
        flex->SM[unit].VAL0 = (uint16_t)(setting->counts - 1 - ticks);
        polarity = PWM_OCTRL_POLX_MASK;
        enable = PWM_OUTEN_PWMX_EN(mask);
    }

    // the polarity: inverted, or not
    if (channel->invert) {
        flex->SM[unit].OCTRL |= polarity;
    } else {
        flex->SM[unit].OCTRL &= (uint16_t)~polarity;
    }

    // loaded at the end of the period; the output and the counter on
    flex->MCTRL |= PWM_MCTRL_LDOK(mask);
    flex->OUTEN |= enable;
    flex->MCTRL |= PWM_MCTRL_RUN(mask);
}

/**
 * Disables a FlexPWM output; the submodule stops when nobody uses it.
 */
void stopFlex(const board::PwmPin *pwm, bool last) {
    PWM_Type *flex = FLEXPWM[pwm->module - 1];
    uint16_t mask = (uint16_t)(1u << pwm->unit);

    // the output off
    if (pwm->output == board::PwmOutput::a) {
        flex->OUTEN &= (uint16_t)~PWM_OUTEN_PWMA_EN(mask);
    } else if (pwm->output == board::PwmOutput::b) {
        flex->OUTEN &= (uint16_t)~PWM_OUTEN_PWMB_EN(mask);
    } else {
        flex->OUTEN &= (uint16_t)~PWM_OUTEN_PWMX_EN(mask);
    }

    // its last output: the counter stopped
    if (last) {
        flex->MCTRL &= (uint16_t)~PWM_MCTRL_RUN(mask);
    }
}

// ### QuadTimer ###############################################################

/**
 * Starts a QuadTimer module once: its clock.
 */
void startQuadModule(int module) {
    if (quadModuleStarted[module - 1]) {
        return;
    }

    CLOCK_EnableClock(QUADTIMER_CLOCKS[module - 1]);
    quadModuleStarted[module - 1] = true;
}

/**
 * Programs a QuadTimer channel (as PJRC's core): its counter reloads the
 * high time (CMPLD1) and the low time (LOAD) in turn, its output set on
 * compare and cleared on rollover; OPS set: active high, clear: inverted.
 *   - a duty cycle of exactly 0 or 1: the pin as a GPIO at that level, the
 *     counter stopped;
 *   - the counter running: only the two loads change, used from the next
 *     period;
 *   - else (first time, after a GPIO level, a new frequency): the channel
 *     set up and started, the pad on its output.
 */
void programQuad(Channel *channel) {
    const board::PwmPin *pwm = channel->pwm;
    const Setting *setting = &generatorOf(pwm)->setting;
    TMR_Type *quad = QUADTIMER[pwm->module - 1];
    int unit = pwm->unit;

    // 0 or 1: a GPIO level, the counter stopped
    uint32_t ticks = dutyTicks(channel->duty, setting->counts);
    bool fullOrNone = ticks == 0 || ticks == setting->counts;
    if (fullOrNone) {
        quad->CHANNEL[unit].CTRL = 0;
        channel->counting = false;
        bool high = ticks != 0;
        driveAsGpio(pwm->pin, high != channel->invert ? 1 : 0);
        return;
    }

    // the high and low times
    uint32_t high = quadHighTicks(ticks, setting->counts);
    uint32_t low = setting->counts - high;

    // running: the next loads only
    if (channel->counting) {
        quad->CHANNEL[unit].LOAD = (uint16_t)(QUADTIMER_LOAD_BASE - low);
        quad->CHANNEL[unit].CMPLD1 = (uint16_t)high;
        return;
    }

    // set up and started
    quad->CHANNEL[unit].CTRL = 0;
    quad->CHANNEL[unit].CNTR = 0;
    uint16_t polarity = channel->invert ? 0 : TMR_SCTRL_OPS_MASK;
    quad->CHANNEL[unit].SCTRL = TMR_SCTRL_OEN_MASK | polarity | TMR_SCTRL_VAL_MASK | TMR_SCTRL_FORCE_MASK;
    quad->CHANNEL[unit].CSCTRL = TMR_CSCTRL_CL1(1) | TMR_CSCTRL_ALT_LOAD_MASK;
    quad->CHANNEL[unit].LOAD = (uint16_t)(QUADTIMER_LOAD_BASE - low);
    quad->CHANNEL[unit].COMP1 = 0;
    quad->CHANNEL[unit].CMPLD1 = (uint16_t)high;

    // counting, then the pad on the timer's output
    uint16_t source = (uint16_t)(QUADTIMER_PCS_IPG + setting->prescale);
    quad->CHANNEL[unit].CTRL = TMR_CTRL_CM(1) | TMR_CTRL_PCS(source) | TMR_CTRL_LENGTH_MASK |
                               TMR_CTRL_OUTMODE(QUADTIMER_SET_ON_COMPARE);
    connectPad(pwm);
    channel->counting = true;
}

/**
 * Stops a QuadTimer channel.
 */
void stopQuad(const board::PwmPin *pwm) {
    QUADTIMER[pwm->module - 1]->CHANNEL[pwm->unit].CTRL = 0;
}

/**
 * Programs an output on its generator.
 */
void program(Channel *channel) {
    if (channel->pwm->kind == board::PwmKind::flexPwm) {
        programFlex(channel);
        connectPad(channel->pwm);
    } else {
        programQuad(channel);
    }
}

/**
 * The channel of a handle, nullptr if it is not open.
 */
Channel *channelOf(int handle) {
    bool inRange = handle >= 0 && handle < board::PIN_COUNT;
    if (!inRange || channels[handle].pwm == nullptr) {
        return nullptr;
    }

    return &channels[handle];
}

} // namespace

/**
 * 1. the pin's PWM line, not open yet;
 * 2. a setting for the frequency, and a generator free or at that frequency;
 * 3. the module started, the generator taken (set up for its first output);
 * 4. the output programmed, the pad connected.
 */
Opened open(int pin, uint32_t frequency, double duty, bool invert) {
    // the pin
    const board::PwmPin *pwm = findPwm(pin);
    if (pwm == nullptr) {
        return Opened{ -1, EINVAL, {} };
    }

    if (channels[pin].pwm != nullptr) {
        return Opened{ -1, EBUSY, {} };
    }

    // the frequency, the generator
    Setting setting;
    if (!settingFor(pwm->kind, frequency, &setting)) {
        return Opened{ -1, EINVAL, {} };
    }

    Generator *generator = generatorOf(pwm);
    bool takenAtAnotherFrequency = generator->users > 0 && generator->frequency != frequency;
    if (takenAtAnotherFrequency) {
        return Opened{ -1, EBUSY, {} };
    }

    // the module, the generator
    bool flex = pwm->kind == board::PwmKind::flexPwm;
    if (flex) {
        startFlexModule(pwm->module);
    } else {
        startQuadModule(pwm->module);
    }

    if (generator->users == 0) {
        generator->frequency = frequency;
        generator->setting = setting;
        if (flex) {
            startFlexUnit(pwm);
        }
    }

    generator->users++;

    // the output
    channels[pin] = Channel{ pwm, invert, duty, false };
    program(&channels[pin]);
    return Opened{ pin, 0, outputOf(&channels[pin]) };
}

/**
 * 1. the output off, the generator freed if it was its last user;
 * 2. the pin a GPIO output at its idle level.
 */
void close(int handle) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return;
    }

    // the output, the generator
    const board::PwmPin *pwm = channel->pwm;
    Generator *generator = generatorOf(pwm);
    generator->users--;
    if (pwm->kind == board::PwmKind::flexPwm) {
        stopFlex(pwm, generator->users == 0);
    } else {
        stopQuad(pwm);
    }

    // the pin at rest
    driveAsGpio(pwm->pin, channel->invert ? 1 : 0);
    channel->pwm = nullptr;
    channel->counting = false;
}

/**
 * 1. a setting for the frequency (EINVAL when out of reach);
 * 2. refused when another output shares the generator at another frequency;
 * 3. the generator's setting, the output programmed again with its duty.
 */
int setFrequency(int handle, uint32_t frequency, Output *out) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return EINVAL;
    }

    // the setting
    Setting setting;
    if (!settingFor(channel->pwm->kind, frequency, &setting)) {
        *out = outputOf(channel);
        return EINVAL;
    }

    // shared: the other outputs keep their frequency
    Generator *generator = generatorOf(channel->pwm);
    bool sharedAtAnotherFrequency = generator->users > 1 && generator->frequency != frequency;
    if (sharedAtAnotherFrequency) {
        *out = outputOf(channel);
        return EBUSY;
    }

    // the new period, the duty cycle kept (a QuadTimer starts again with it)
    generator->frequency = frequency;
    generator->setting = setting;
    channel->counting = false;
    program(channel);
    *out = outputOf(channel);
    return 0;
}

/**
 * The duty cycle kept, the output programmed again (from its next period).
 */
int setDuty(int handle, double duty, Output *out) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return EINVAL;
    }

    channel->duty = duty;
    program(channel);
    *out = outputOf(channel);
    return 0;
}

} // namespace pwm
} // namespace hal
