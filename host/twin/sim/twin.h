/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The digital twin of a board on the desktop ("qjsx agent --board <name>"):
// the board described by boards/<name>.json, its hardware simulated behind
// the interfaces of common/hal (twin.cpp: hal::gpio, twin_rmt.cpp:
// hal::rmt), so that the modules of common/device run unchanged, and the
// state the web page of the twin shows (host/twin/js/server.js, through the native
// module of twin_module.cpp).
//
// A pin is a line whose level comes from what drives it, in this order:
//   1. a push-pull output of the program drives its latch, or a PWM output
//      of the program its square signal (the level at that instant);
//   2. an open drain of the program at 0 drives 0;
//   3. the web page forces 0 or 1;
//   4. a component (the model of a part, host/twin/js/twin.js) drives 0 or 1:
//      a button pressed;
//   5. the pulls: the program's (up or down), else a component's (the
//      pull-up of an active low button);
//   6. else the line floats and reads 0.
// A program's output and a component driving the other level is a short on
// a real board: the pin is marked 'conflict' (nothing breaks).
//
// Threads: the hal functions on the device's JS thread, the page functions
// and the components on the twin's thread; a lock protects the state. Change
// handlers of watched pins are called on the thread that changed the level
// (never with the lock held); the components hear of a change through the
// bridge (below), on their own thread.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal_i2c.h"
#include "hal_rmt.h"
#include "hal_serial.h"
#include "hal_spi.h"
#include "hal_usb.h"
#include "loop.h"
#include "quickjs.h"
#include "twin_tcp.h"

namespace twin {

// What the page reads of a pin.
struct PinState {
    bool    configured;  // the program configured it (hal::gpio::configure)
    uint8_t mode;        // hal::gpio::Mode as a number: 0 input, 1 output, 2 open drain
    uint8_t pull;        // hal::gpio::Pull as a number: 0 none, 1 up, 2 down
    uint8_t level;       // 0 or 1, the level on the line now
    int8_t  forced;      // what the page forces: 0, 1, or -1 (nothing)
    int8_t  partDrive;   // what a component drives: 0, 1, or -1 (nothing)
    uint8_t partPull;    // a component's pull: 0 none, 1 up, 2 down
    bool    conflict;    // the program drives a level and a component the other
    bool    rmt;         // a channel of hal::rmt drives it (open)
    bool    i2c;         // a bus of hal::i2c is open on it (sda or scl)
    bool    spi;         // a bus or a device of hal::spi uses it (clock, data, chip select, DC)
    bool    adc;         // the program has its analog input open (hal::adc)
    bool    dac;         // a DAC output of hal::dac drives it (open): its voltage wins
    int16_t voltageMv;   // the voltage a component sets on it, in mV; -1: none (its level gives it)
    bool    pwm;         // an output of hal::pwm drives it (open)
    bool    pwmInvert;   // that output is active low (pwm)
    double  pwmDuty;     // its duty cycle, 0 to 1: the share of a period it is active (pwm)
    // its frequency, Hz, as the hardware makes it (pwm)
    double  pwmFrequency;
};

/**
 * Reads the board description 'path' (JSON) and makes this process its twin.
 * False after printing why (unreadable, not JSON, a field missing).
 */
bool load(const char *path);

/**
 * True once load() succeeded.
 */
bool active();

/**
 * The board description, as read (JSON text, for the page).
 */
const char *boardJson();

/**
 * Name of the board ("YD-ESP32-S3").
 */
const char *name();

/**
 * Chip of the board ("ESP32-S3"); its name if the description has none.
 */
const char *chip();

/**
 * process.platform of the board ("esp32"...).
 */
const char *platform();

/**
 * Number of pins (GPIO 0 to pinCount() - 1).
 */
int pinCount();

/**
 * The native modules of the board (names, nativeCount() of them).
 */
const char *const *natives();

/**
 * How many native modules natives() gives.
 */
int nativeCount();

/**
 * Reads the integers of the array property 'name' of 'object' (a part of the
 * board description) into 'out', at most 'capacity'; the count read, -1 if
 * an item is not an integer from 'low' to 'high'. A missing property reads
 * 0 items. In load().
 */
int readIntegers(JSContext *ctx, JSValueConst object, const char *name, int low, int high, int *out, int capacity);

// ### Choices of the page (twin_choices.cpp) ##################################
//
// What the page chooses for a link (the TCP port of a UART, the PC serial
// port it is wired to...) is kept in <storage>/twin.json, one JSON object per
// key ("uart1", "usb0", "serial1"), and read again at the next start. Each
// link writes only its own key: the others keep what the file held.

// Called by choicesRead() with the object kept for a key (a short-lived
// context: copy what is needed).
typedef void ChoiceReader(void *context, JSContext *ctx, JSValueConst choice);

/**
 * Reads '<storage>/twin.json' (a missing or broken file: no choice).
 * 'storage' must live as long as the process. Before the links are set up.
 */
void choicesLoad(const char *storage);

/**
 * Calls 'read' with the object kept for 'key'; false (nothing called) when
 * there is none. Any thread.
 */
bool choicesRead(const char *key, ChoiceReader *read, void *context);

/**
 * Keeps 'json' (an object, as JSON text) for 'key' and writes the file (a
 * failure is said on stderr). Any thread.
 */
void choicesWrite(const char *key, const char *json);

/**
 * Copies the state of the first 'count' pins into 'pins' (page thread).
 */
void snapshot(PinState *pins, int count);

/**
 * The page forces a pin: level 0 or 1, or -1 to let it go (page thread).
 */
void force(int pin, int level);

/**
 * A channel of hal::rmt takes 'pin' (open) or lets it go (close)
 * (twin_rmt.cpp).
 */
void setRmt(int pin, bool driven);

/**
 * A bus of hal::i2c takes 'pin' (open) or lets it go (close) (twin_i2c.cpp).
 */
void setI2c(int pin, bool used);

/**
 * A bus or a device of hal::spi uses 'pin', or not any more (twin_spi.cpp).
 */
void setSpi(int pin, bool used);

// ### Analog inputs (twin_adc.cpp) ############################################
//
// The board description says which pins have an analog input ("adc": {
// "pins", "bits", "spans" (the ranges its converter can read, in mV, from
// the smallest), "vref" (volts: the level 1 of a pin) }). The voltage on a
// pin is what the program's DAC drives (below), else what a component sets
// (a potentiometer), else its level times vref.

/**
 * Whether 'pin' has an analog input.
 */
bool adcPin(int pin);

/**
 * Resolution of the converter, in bits (12 by default).
 */
int adcBits();

/**
 * The span the converter reads for a range: the first of "spans" that
 * covers 'rangeMv', the largest up to vref; 0 when it cannot read it.
 */
int adcSpanFor(int rangeMv);

/**
 * The voltage on 'pin' now, in mV.
 */
int pinVoltage(int pin);

/**
 * The program opens (or closes) the analog input of 'pin' (twin_adc.cpp).
 */
void setAdc(int pin, bool open);

/**
 * A component sets the voltage on 'pin' in mV, or not any more (-1) (twin's
 * thread).
 */
void partVoltage(int pin, int millivolts);

// ### Analog outputs (twin_dac.cpp) ###########################################
//
// The board description says which pins have a DAC ("dac": { "pins",
// "bits", "fullScale" (volts of the highest code) }, the original ESP32:
// [25, 26], 8, 3.3). Without the section, the board has no DAC (and the
// twin does not offer device/dac). A pin a DAC drives has the voltage of its
// code: the program's output wins over what a component sets, as for the
// levels; its level is 1 from half of vref ("adc.vref") up. The parts read
// that voltage (pin.voltage of qjsx:twin), the page and the analyzer show
// it.

/**
 * Reads "dac" of the description (optional); false after printing what is
 * wrong (load(), twin.cpp).
 */
bool dacRead(JSContext *ctx, JSValueConst description, const char *path);

/**
 * Whether the board has a DAC ("dac" of the description).
 */
bool dacPresent();

/**
 * A DAC output of hal::dac drives 'pin' at 'millivolts' (opened, or a new
 * code) (twin_dac.cpp, JS thread).
 */
void dacDrive(int pin, int millivolts);

/**
 * The DAC output on 'pin' stopped: the pin is given back, as a pin nobody
 * configured (hal::gpio::reset).
 */
void dacStop(int pin);

// ### PWM outputs (twin_pwm.cpp) ##############################################
//
// The board description says what its PWM has ("pwm": { "outputs",
// "timers", "clock" (Hz), "maxBits" }, the LEDC of the ESP32-S3: 8, 4,
// 80000000, 14); hal::pwm of the twin rounds and refuses as that hardware
// (common/hal/pwm_timers.h). Without the section, the board has no PWM
// output. The signal is not simulated edge by edge: a pin knows its
// frequency, duty cycle and the time they were set, and its level is
// computed at the instant it is asked.

/**
 * Reads "pwm" of the description (optional); false after printing what is
 * wrong (load(), twin.cpp).
 */
bool pwmRead(JSContext *ctx, JSValueConst description, const char *path);

/**
 * An output of hal::pwm drives 'pin' at 'frequency' (Hz) and 'duty' (0 to 1
 * active): from now when it starts or changes frequency, from its next
 * period when only the duty cycle changes, as LEDC (twin_pwm.cpp, JS
 * thread).
 */
void pwmDrive(int pin, bool invert, double frequency, double duty);

/**
 * The output on 'pin' stopped: the pin stays a program's output at its idle
 * level (0, or 1 inverted), as on the board.
 */
void pwmStop(int pin, bool invert);

// ### Traces for the page #####################################################
//
// The transfers of a bus are kept as numbered records (from 1) in a ring:
// the newest TraceRing::CAPACITY of them, the oldest dropped. The page reads
// the records after the last one it got. The ring has no lock of its own:
// its owner holds one around each call.

template <typename Record, int Capacity>
class TraceRing {
public:
    // Called by visit() for each record.
    typedef void Visitor(void *context, const Record *record);

    /**
     * The slot of a new record, zeroed, its 'sequence' set; it overwrites
     * the oldest record when the ring is full.
     */
    Record   *add() {
        _newest++;
        Record *entry = &_records[_newest % Capacity];
        memset(entry, 0, sizeof(*entry));
        entry->sequence = _newest;
        return entry;
    }

    /**
     * Calls 'visit' for each record numbered after 'after' still in the
     * ring, oldest first. Returns the number of the newest record (0: none
     * yet).
     */
    uint32_t  visit(uint32_t after, Visitor *visit, void *context) const {
        // the oldest record still in the ring, and the first one asked (on 64
        // bits: the largest 'after', which the page's server gives to skip
        // every record, must not wrap to 0 and give them all)
        bool ringFull = _newest > (uint32_t)Capacity;
        uint64_t oldest = ringFull ? _newest - Capacity + 1 : 1;
        uint64_t next = (uint64_t)after + 1;
        uint64_t first = next > oldest ? next : oldest;

        // each one, oldest first
        for (uint64_t sequence = first; sequence <= _newest; sequence++) {
            visit(context, &_records[sequence % Capacity]);
        }

        return _newest;
    }

private:
    Record   _records[Capacity];   // the ring, indexed by sequence % Capacity
    uint32_t _newest = 0;          // number of the newest record; 0: none
};

// ### I2C buses (twin_i2c.cpp) ################################################
//
// The buses the program opens, and the trace of its transfers for the page:
// the last I2C_TRACE_RECORDS of them, every bus together, numbered.

const int I2C_BUSES = 2;                 // as the ESP32-S3
const size_t I2C_TRACE_BYTES = 32;       // bytes kept of what a transfer writes, and of what it reads
const int I2C_TRACE_RECORDS = 256;       // transfers kept

const int I2C_MAX_FREQUENCIES = 4;       // distinct frequencies of a bus the page is told

// What the page reads of a bus.
struct I2cBusState {
    bool     open;                       // the program has it open
    int      sda;                        // its data pin (open)
    int      scl;                        // its clock pin (open)
    uint32_t frequencies[I2C_MAX_FREQUENCIES];   // of the devices on it, distinct, in Hz
    int      frequencyCount;             // entries of frequencies
};

// A transfer of the program (a probe writes and reads nothing).
struct I2cRecord {
    uint32_t sequence;                   // its number, from 1
    int64_t  timeMs;                     // when, in ms since the twin started
    int      bus;                        // the bus number of the program
    int      address;                    // 7-bit address
    int      status;                     // hal::i2c::Status as a number
    uint32_t frequencyHz;                // the frequency of the device; 0: a probe (no device)
    size_t   writeSize;                  // bytes written (the first I2C_TRACE_BYTES kept)
    size_t   readSize;                   // bytes asked (the first I2C_TRACE_BYTES of the answer kept)
    uint8_t  written[I2C_TRACE_BYTES];   // what was written (the first bytes)
    uint8_t  read[I2C_TRACE_BYTES];      // what came back (ok); zeros otherwise
};

// Called by i2cTrace() for each record, with the lock held.
typedef void I2cTraceVisitor(void *context, const I2cRecord *record);

/**
 * Copies the state of the buses into 'states' (I2C_BUSES of them).
 */
void i2cBusStates(I2cBusState *states);

/**
 * Calls 'visit' for each record numbered after 'after', oldest first.
 * Returns the number of the newest record (0: none yet).
 */
uint32_t i2cTrace(uint32_t after, I2cTraceVisitor *visit, void *context);

// ### SPI buses (twin_spi.cpp) ################################################
//
// The controllers of the board description ("spi": { "ports": [2, 3],
// "reserved": [0, 1] }: the ports the program may open, and those kept for
// the flash, refused as busy, as on the ESP32-S3); without the section the
// board has no SPI. The buses the program opens, their devices, and the trace
// of its transactions for the page: the last SPI_TRACE_RECORDS of them,
// every bus together, numbered.

const int SPI_MAX_PORTS = 4;             // controllers a board may list ("ports" and "reserved" each)
const int SPI_MAX_DEVICES = 6;           // devices on one bus (chip selects of a controller, SPI2 of the ESP32-S3)
const int SPI_TRACE_RECORDS = 256;       // transactions kept
const int SPI_TRACE_SEGMENTS = 8;        // segments kept of a transaction
const size_t SPI_TRACE_BYTES = 16;       // bytes kept of what a segment writes, and of what it reads

// What a segment of a transaction does (hal::spi::Segment has no field for
// it: its pointers say it).
enum class SpiKind {
    write,                               // bytes sent
    read,                                // bytes received, the fill byte sent
    transfer,                            // bytes sent and received at the same time
    delay,                               // a wait, the chip select held
};

/**
 * The kind of a segment, from which of its pointers are set.
 */
inline SpiKind spiKindOf(const hal::spi::Segment *segment) {
    // both: a transfer; only one: a write or a read; none: a delay
    bool writes = segment->write != nullptr;
    bool reads = segment->read != nullptr;
    if (writes && reads) {
        return SpiKind::transfer;
    }

    if (writes) {
        return SpiKind::write;
    }

    return reads ? SpiKind::read : SpiKind::delay;
}

// How a transaction went for the components.
enum class SpiOutcome {
    answered,                            // a component on its chip select answered it
    nobody,                              // no component there: every byte read is 0xFF
    timeout,                             // the component did not answer in time: 0xFF read
};

// A device on a bus, as the page sees it.
struct SpiDeviceState {
    int      cs;                         // chip select pin; -1: driven by the program
    int      dc;                         // data / command pin; -1: none
    uint32_t frequencyHz;                // the clock the hardware makes
    uint8_t  mode;                       // 0..3: bit 1 CPOL, bit 0 CPHA
    bool     lsbFirst;                   // bit order
};

// What the page reads of a bus.
struct SpiBusState {
    int            port;                 // the controller's number ("ports" of the description)
    bool           open;                 // the program has it open
    int            sclk;                 // its clock pin (open)
    int            mosi;                 // its data out pin (open); -1: not wired
    int            miso;                 // its data in pin (open); -1: not wired
    int            deviceCount;          // entries of devices
    SpiDeviceState devices[SPI_MAX_DEVICES];   // its devices, in the order of their making
};

// A segment of a transaction, as the trace keeps it.
struct SpiSegmentRecord {
    uint8_t  kind;                       // SpiKind as a number
    uint8_t  dc;                         // the level of the DC pin during it
    uint32_t size;                       // its bytes (the first SPI_TRACE_BYTES kept)
    uint32_t delayUs;                    // a delay: microseconds
    uint8_t  written[SPI_TRACE_BYTES];   // the first bytes sent (write, transfer)
    uint8_t  read[SPI_TRACE_BYTES];      // the first bytes received (read, transfer)
};

// A transaction of the program.
struct SpiRecord {
    uint32_t         sequence;           // its number, from 1
    int64_t          timeUs;             // when, in µs since the twin started (scopeNowUs)
    int              port;               // the controller
    int              cs;                 // chip select pin of the device; -1: driven by the program
    int              dc;                 // DC pin of the device; -1: none
    uint32_t         frequencyHz;        // the clock of the device
    uint8_t          mode;               // 0..3
    bool             lsbFirst;           // bit order
    uint8_t          outcome;            // SpiOutcome as a number
    uint32_t         segmentCount;       // its segments (the first SPI_TRACE_SEGMENTS kept)
    SpiSegmentRecord segments[SPI_TRACE_SEGMENTS];   // the first segments
};

// Called by spiTrace() for each record, with the lock held.
typedef void SpiTraceVisitor(void *context, const SpiRecord *record);

/**
 * Reads "spi" of the description (optional); false after printing what is
 * wrong (load(), twin.cpp).
 */
bool spiRead(JSContext *ctx, JSValueConst description, const char *path);

/**
 * Copies the state of the board's buses ("ports" of the description, in
 * their order) into 'states' (SPI_MAX_PORTS at most); returns how many.
 */
int spiBusStates(SpiBusState *states);

/**
 * Calls 'visit' for each record numbered after 'after', oldest first.
 * Returns the number of the newest record (0: none yet).
 */
uint32_t spiTrace(uint32_t after, SpiTraceVisitor *visit, void *context);

// ### Analyzer of the pins (twin_scope.cpp) ###################################
//
// Every change on a pin (its level, or the voltage a component sets on it)
// is recorded with its time, for the analyzer panel of the page; the page's
// server reads them every 50 ms (scopeRead). Between two reads, a pin keeps
// at most SCOPE_RECORDS changes: past that (a pin toggled faster than the
// page follows), the changes are only counted, and the page draws them as a
// "busy" band. Times are those of the PC, in µs since the twin started.
//
// A PWM output is not recorded edge by edge (at 20 kHz the records would be
// full at once): its changes are recorded instead (started, another
// frequency or duty cycle, stopped), and the page draws the signal itself
// from them (user, 2026-11-13). While a PWM drives a pin, its level is not
// recorded.
//
// An RMT send neither: it is kept as the hal got it (pulses, or bytes and
// the timing of their bits) with the time it started, and the page computes
// its edges (user, 2026-11-13: the pulses of a WS2812 last 0.4 µs, less
// than the µs of the records). Between two reads, at most SCOPE_TRAINS sends
// are kept (every pin together), the others dropped.

const int SCOPE_RECORDS = 256;           // changes of a pin kept between two reads
const int SCOPE_TRAINS = 1024;           // RMT sends kept between two reads

// An RMT send, for the analyzer.
struct ScopeTrain {
    int                 pin;             // the pin of the channel
    int64_t             timeUs;          // when the send started, µs since the twin started
    uint32_t            resolutionHz;    // ticks per second
    bool                bytes;           // data: bytes (sendBytes); else pulses, 5 bytes each (ticks u32 LE, level)
    hal::rmt::BitTiming bit0;            // bytes: how a 0 is sent (high, then low)
    hal::rmt::BitTiming bit1;            // bytes: how a 1 is sent
    uint32_t            resetTicks;      // bytes: low after them
    uint8_t            *data;            // owned by the analyzer
    size_t              size;            // bytes of data
};

// Called by scopeReadTrains() for each send.
typedef void TrainVisitor(void *context, const ScopeTrain *train);

// A change of a pin, or its state now: of its level (pwm false), or of the
// PWM output that drives it (pwm true).
struct ScopeRecord {
    int64_t timeUs;                      // when, in µs since the twin started; a PWM: the start of its first period
    int     level;                       // the level on the line after it (pwm false)
    int     millivolts;                  // the voltage on the line after it (pwm false)
    bool    pwm;                         // a change of the PWM output: the fields below
    double  pwmFrequency;                // Hz; 0: the output stopped
    double  pwmDuty;                     // 0 to 1, the share of a period it is active
    bool    pwmInvert;                   // active low: low during the active share
};

// What a pin did since the last read.
struct ScopeChanges {
    int         count;                   // entries of records
    ScopeRecord records[SCOPE_RECORDS];  // the first changes, oldest first
    uint32_t    dropped;                 // changes after the records were full: only counted
    int64_t     droppedUntilUs;          // time of the last dropped change
};

// Called by scopeRead() for each pin: what it did since the last read
// (count and dropped 0: nothing), its level now, and its PWM now (the last
// PWM change; pwmFrequency 0: none).
typedef void ScopeVisitor(void *context, int pin, const ScopeChanges *changes, const ScopeRecord *now,
                          const ScopeRecord *pwmNow);

/**
 * Sets the time 0 of the analyzer (load(), before any change).
 */
void scopeStart();

/**
 * µs since scopeStart(). Any thread.
 */
int64_t scopeNowUs();

/**
 * The state of 'pin' at rest, without recording a change (load()).
 */
void scopeSetInitial(int pin, int level, int millivolts);

/**
 * Records a change of 'pin': its level and its voltage after it (twin.cpp,
 * any thread, the twin's state lock held).
 */
void scopeRecord(int pin, int level, int millivolts);

/**
 * Records a change of the PWM output of 'pin': from 'timeUs' (the start of
 * its first period) at 'frequency' (Hz; 0: it stopped) and 'duty' active
 * (twin.cpp, the twin's state lock held).
 */
void scopeRecordPwm(int pin, int64_t timeUs, double frequency, double duty, bool invert);

/**
 * Calls 'visit' for each of the first 'count' pins with what it did since
 * the last read, then forgets it. One reader only (the page's server).
 * Returns the time of the read, in µs since scopeStart().
 */
int64_t scopeRead(int count, ScopeVisitor *visit, void *context);

/**
 * Records an RMT send (its data copied); dropped past SCOPE_TRAINS sends
 * since the last read (twin_rmt.cpp, JS thread).
 */
void scopeRecordTrain(const ScopeTrain *train);

/**
 * Calls 'visit' for each RMT send since the last read, oldest first, then
 * forgets them. One reader only (the page's server).
 */
void scopeReadTrains(TrainVisitor *visit, void *context);

/**
 * A component drives 'pin' to 0 or 1, or lets it go (-1) (twin's thread).
 */
void partDrive(int pin, int level);

/**
 * A component pulls 'pin': 0 none, 1 up, 2 down (twin's thread).
 */
void partPull(int pin, int pull);

/**
 * A component watches the level of 'pin' (a BridgeEvent::pinChange at each
 * change) or stops (twin's thread).
 */
void partWatch(int pin, bool watched);

// ### Bridge to the components (twin_bridge.cpp) ##############################
//
// The models of the parts run in the twin's JS runtime (host/twin/js/twin.js);
// the simulated hardware tells them what the program does through messages
// posted in that runtime's loop (the bridge, an object of qjsx:twinboard).
// Without a bridge, the events are dropped.
//
// An I2C transfer is synchronous for the program: its thread posts the
// request, then waits for the answer of the model (twin::i2cAnswer, on the
// twin's thread) at most the timeout of the transfer. The twin's thread never
// waits for the program's.

// What a message of the bridge tells (Message::kind).
enum class BridgeEvent {
    pinChange = 1,                  // value: pin; data: [level]
    rmtBytes,                       // value: pin; data: the bytes of sendBytes
    rmtPulses,                      // value: pin; data: [ticks u32 LE, level u8] per pulse
    i2cTransfer,                    // value: request; data: [sda, scl, address, 0, readSize u32 LE, written...]
    spiTransaction,                 // value: request; data: see spiTransaction()
};

/**
 * The bridge of the twin's runtime: the messages go to 'source' of the loop
 * owning 'mailbox' (retained here). Twin's thread.
 */
void bridgeAttach(rt::Mailbox *mailbox, rt::SourceId source);

/**
 * No bridge any more (its object was collected): events dropped.
 */
void bridgeDetach();

/**
 * Posts an event to the bridge (a copy of 'data'); false if there is none.
 * Any thread.
 */
bool bridgePost(BridgeEvent kind, int64_t value, const void *data, size_t size);

/**
 * A component answers at 'address' on the bus of pins 'sda' / 'scl', or not
 * any more (twin's thread).
 */
void i2cAttach(int sda, int scl, int address, bool attached);

/**
 * The program's transfer with 'address' on the bus of pins 'sda' / 'scl':
 * writes 'written', reads 'readSize' bytes into 'read'. Answered by the
 * component there: noAck when none answers, timeout when its answer did not
 * come within 'timeoutMs'. Program's thread.
 */
hal::i2c::Status i2cTransfer(int sda, int scl, int address, const uint8_t *written, size_t writeSize,
                             uint8_t *read, size_t readSize, int timeoutMs);

/**
 * The answer of a component to the transfer 'request': hal::i2c::Status as a
 * number, and the bytes read (copied; at most what was asked). Twin's
 * thread; an answer that comes too late is dropped.
 */
void i2cAnswer(uint32_t request, int status, const uint8_t *bytes, size_t size);

const size_t SPI_MESSAGE_HEADER = 16;    // bytes of the header of a spiTransaction message
const size_t SPI_MESSAGE_SEGMENT = 12;   // bytes of the descriptor of a segment in it

// An SPI transaction, as the component on its chip select hears it.
struct SpiTransfer {
    int      port;                  // the controller of the program
    int      cs;                    // the chip select pin of the component
    uint32_t frequencyHz;           // the clock of the device
    int      mode;                  // 0..3
    bool     lsbFirst;              // bit order
};

/**
 * A component sits on chip select pin 'cs' of controller 'port', or not any
 * more (twin's thread).
 */
void spiAttach(int port, int cs, bool attached);

/**
 * The chip select of the component that hears a transaction of a device on
 * 'port' whose chip select is 'cs': the component on 'cs'; for a device
 * without one (-1: the program drives it with device/gpio), the first
 * component of the controller whose chip select pin is low now. -1: none.
 * Program's thread.
 */
int spiListener(int port, int cs);

/**
 * The program's transaction: posted to the bridge, then waits for the
 * component's answer at most 'timeoutMs'. The bytes it reads are written
 * into the read buffers of the segments; 0xFF where the component gives
 * none, or when it did not answer in time (timeout). nobody when there is
 * no bridge. Program's thread (or the "twin spi" thread).
 *
 * The data of the message: [port u8, cs u8, mode u8, lsbFirst u8, frequency
 * u32 LE, segment count u32 LE, bytes read u32 LE], then per segment [kind
 * u8 (SpiKind), dc u8, 0, 0, size u32 LE, delayUs u32 LE], then the bytes
 * of the write and transfer segments, one after the other.
 */
SpiOutcome spiTransaction(const SpiTransfer *transfer, const hal::spi::Segment *segments, size_t count, int timeoutMs);

/**
 * The answer of a component to the transaction 'request': the bytes of its
 * read and transfer segments, one after the other (copied; 0xFF after them
 * up to what was asked). Twin's thread; a late answer is dropped.
 */
void spiAnswer(uint32_t request, const uint8_t *bytes, size_t size);

/**
 * The twin's runtime says whether its parts are built: 'error' nullptr when
 * they are, else why not (copied). The first report counts. Twin's thread.
 */
void readyReport(const char *error);

/**
 * Waits for readyReport() at most 'timeoutMs': false on timeout. *error
 * receives the reason of a failure (owned by the caller), nullptr when the
 * parts are built. The agent's thread, before the program runs.
 */
bool readyWait(int64_t timeoutMs, char **error);

// ### USB device (twin_usb.cpp) ###############################################
//
// The serial functions of the USB device are TCP links (twin_tcp.h), which
// listen only while the program has the device started.

// What the page reads of the USB device.
struct UsbState {
    bool                  started;                          // the program started it
    hal::usb::Descriptors descriptors;                      // of the last start
    int                   serials;                          // its serial functions (started)
    TcpState              tcp[hal::usb::MAX_SERIALS];       // the link of each function
};

/**
 * Sets up the TCP links of the serial functions ("usb<n>"): listening from
 * the start on 'port' + n once the device starts when 'port' is given (not
 * saved), else as the page chooses. Before startTcp().
 */
void setupUsb(const char *host, int port);

/**
 * The state of the USB device (page thread).
 */
void usbState(UsbState *state);

// ### UARTs (twin_uart.cpp) ###################################################
//
// The UARTs of the board ("uarts" of the description) are the ports of
// device/serial in the twin: the program opens them by number, the pins are
// only recorded. Each UART (except the agent's link of the real board,
// "linkUart", refused as on the board) is reachable from outside twice: a TCP
// link (twin_tcp.h, one client at a time) and its panel on the page. What the
// program writes goes to both; what either sends reaches the program, mixed
// in the order of arrival. Nothing is timed: the bytes pass at once, at any
// baud rate. The page reads the traffic of each UART as numbered records.

const int MAX_UARTS = 8;            // UARTs a board may have

// Where a record of the traffic comes from.
enum class UartSource {
    program,                        // the program wrote it
    page,                           // the page sent it to the program
    tcp,                            // the TCP client sent it to the program
    serial,                         // the PC serial port it is wired to sent it to the program
};

const int SERIAL_NAME_SIZE = 64;    // bytes of the name of a PC serial port
const int SERIAL_ERROR_SIZE = 64;   // bytes of SerialState.error

// The PC serial port a UART is wired to (twin_uart_serial.cpp).
struct SerialState {
    char port[SERIAL_NAME_SIZE];    // the port chosen ("COM5", "/dev/ttyUSB0"); "": none yet
    bool connect;                   // wiring asked
    bool realSpeed;                 // the program's writes take the time the real port takes
    bool connected;                 // the port is open now (asked, and the program has the UART open)
    bool fixed;                     // set by the command line or qjsx.json: not saved
    char error[SERIAL_ERROR_SIZE];  // why the last opening failed; "": none
};

// What the page reads of a UART.
struct UartState {
    bool                  link;      // the agent's link of the real board: never opened
    bool                  open;      // the program has it open
    TcpState              tcp;       // its TCP link (all 0 for the agent's link)
    SerialState           serial;    // the PC serial port it is wired to (all 0 for the agent's link)
    hal::serial::Settings settings;  // as the program opened it (open)
};

// Called by uartTraffic() for each record, with the UART's lock held.
typedef void TrafficVisitor(void *context, uint32_t sequence, UartSource source, const uint8_t *bytes,
                            size_t size);

/**
 * UARTs of the board (0: none).
 */
int uartCount();

/**
 * The UART of the agent's link on the real board, refused to the program;
 * -1: none.
 */
int linkUartNumber();

/**
 * The driver of device/serial in the twin (static).
 */
const hal::serial::Driver *serialDriver();

/**
 * Sets up the TCP links of the UARTs ("uart<n>", the agent's link
 * excepted): listening from the start on 'port' + n when 'port' is given
 * (not saved), else as the page chooses. Before startTcp().
 */
void setupUarts(const char *host, int port);

/**
 * Copies the state of the first 'count' UARTs into 'states' (page thread).
 */
void uartStates(UartState *states, int count);

/**
 * Calls 'visit' for each record of UART 'uart' numbered after 'after' (the
 * oldest ones are dropped past 16 KB). Returns the number of the last record
 * of the UART (0: none yet).
 */
uint32_t uartTraffic(int uart, uint32_t after, TrafficVisitor *visit, void *context);

/**
 * Bytes for the program on UART 'uart', from 'source' (the page, the PC
 * serial port). False (dropped) if the program does not have it open. Any
 * thread.
 */
bool uartReceive(int uart, UartSource source, const uint8_t *bytes, size_t size);

// ### UARTs wired to PC serial ports (twin_uart_serial.cpp) ###################
//
// A UART of the twin can be wired to a real serial port of the PC (a USB
// serial adapter...), its other end: the port is open while the program has
// the UART open, with the program's settings; what the program writes goes
// to it (as to the TCP client and the page), what it receives goes to the
// program. "Real speed": the program's write waits the time the bytes take
// on the line; otherwise it returns at once and a thread sends them (64 KB
// waiting at most, the rest lost and said on stderr). Chosen on the page
// (kept in twin.json), or fixed by "--uart-serial n=PORT" / "twin.uarts" of
// qjsx.json (real speed, not saved).

/**
 * Wires UART 'uart' to 'port' from the start, not saved (the command line,
 * qjsx.json). Before setupSerial(); the caller checked the number.
 */
void serialFix(int uart, const char *port, bool realSpeed);

/**
 * Sets up the wiring of each UART but the agent's link (the choices kept
 * for the others) and starts their threads. After choicesLoad() and
 * setupUarts(). False after printing why.
 */
bool setupSerial();

/**
 * The page wires UART 'uart' to 'port', or not ('connect' false), and
 * chooses the speed; kept in twin.json unless the wiring is fixed. Page
 * thread.
 */
void serialRequest(int uart, const char *port, bool connect, bool realSpeed);

/**
 * The state of the wiring of UART 'uart'.
 */
void serialState(int uart, SerialState *state);

/**
 * The program opened UART 'uart' with 'settings' (twin_uart.cpp, its thread).
 */
void serialUartOpened(int uart, const hal::serial::Settings *settings);

/**
 * The program closed UART 'uart' (twin_uart.cpp).
 */
void serialUartClosed(int uart);

/**
 * The program writes on UART 'uart': to the PC port when wired (waits the
 * time of the line at real speed). Program's thread.
 */
void serialUartWrite(int uart, const uint8_t *data, size_t size);

/**
 * Calls 'add' with the name of each serial port of the PC (number -1).
 */
void serialPorts(hal::serial::ListHandler *add, void *context);

/**
 * The web page of the twin (host/twin/ui/dist) as a zip archive with stored
 * entries, embedded in the executable: 'size' bytes, static. Written by
 * tools/embed-twin-ui.lua.
 */
const uint8_t *uiArchive(size_t *size);

} // namespace twin
