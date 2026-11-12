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
//   1. a push-pull output of the program drives its latch;
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

#include "hal_i2c.h"
#include "hal_serial.h"
#include "hal_usb.h"
#include "loop.h"
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
    bool    adc;         // the program has its analog input open (hal::adc)
    int16_t voltageMv;   // the voltage a component sets on it, in mV; -1: none (its level gives it)
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

// ### Analog inputs (twin_adc.cpp) ###########################################
//
// The board description says which pins have an analog input ("adc": {
// "pins", "bits", "spans" (the ranges its converter can read, in mV, from
// the smallest), "vref" (volts: the level 1 of a pin) }). The voltage on a
// pin is what a component sets (a potentiometer), else its level times vref.

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

// ### I2C buses (twin_i2c.cpp) ###############################################
//
// The buses the program opens, and the trace of its transfers for the page:
// the last I2C_TRACE_RECORDS of them, every bus together, numbered.

const int I2C_BUSES = 2;                 // as the ESP32-S3
const size_t I2C_TRACE_BYTES = 32;       // bytes kept of what a transfer writes, and of what it reads
const int I2C_TRACE_RECORDS = 256;       // transfers kept

// What the page reads of a bus.
const int I2C_MAX_FREQUENCIES = 4;       // distinct frequencies of a bus the page is told

struct I2cBusState {
    bool     open;                       // the program has it open
    int      sda;                        // its pins (open)
    int      scl;
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
    uint8_t  written[I2C_TRACE_BYTES];
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

// ### Analyzer of the pins (twin_scope.cpp) ##################################
//
// Every change on a pin (its level, or the voltage a component sets on it)
// is recorded with its time, for the analyzer panel of the page; the page's
// server reads them every 50 ms (scopeRead). Between two reads, a pin keeps
// at most SCOPE_RECORDS changes: past that (a pin toggled faster than the
// page follows), the changes are only counted, and the page draws them as a
// "busy" band. Times are those of the PC, in µs since the twin started.

const int SCOPE_RECORDS = 256;           // changes of a pin kept between two reads

// A change of a pin, or its state now.
struct ScopeRecord {
    int64_t timeUs;                      // when, in µs since the twin started
    int     level;                       // the level on the line after it
    int     millivolts;                  // the voltage on the line after it
};

// What a pin did since the last read.
struct ScopeChanges {
    int         count;                   // entries of records
    ScopeRecord records[SCOPE_RECORDS];  // the first changes, oldest first
    uint32_t    dropped;                 // changes after the records were full: only counted
    int64_t     droppedUntilUs;          // time of the last dropped change
};

// Called by scopeRead() for each pin: what it did since the last read
// (count and dropped 0: nothing) and its state now.
typedef void ScopeVisitor(void *context, int pin, const ScopeChanges *changes, const ScopeRecord *now);

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
 * Calls 'visit' for each of the first 'count' pins with what it did since
 * the last read, then forgets it. One reader only (the page's server).
 * Returns the time of the read, in µs since scopeStart().
 */
int64_t scopeRead(int count, ScopeVisitor *visit, void *context);

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

// ### Bridge to the components (twin_bridge.cpp) #############################
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

// ### USB device (twin_usb.cpp) ##############################################
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

// ### UARTs (twin_uart.cpp) ##################################################
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
};

// What the page reads of a UART.
struct UartState {
    bool                  link;      // the agent's link of the real board: never opened
    bool                  open;      // the program has it open
    TcpState              tcp;       // its TCP link (all 0 for the agent's link)
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
 * The page sends bytes to the program on UART 'uart'. False (dropped) if the
 * program does not have it open.
 */
bool uartSend(int uart, const uint8_t *bytes, size_t size);

/**
 * The web page of the twin (host/twin/ui/dist) as a zip archive with stored
 * entries, embedded in the executable: 'size' bytes, static. Written by
 * tools/embed-twin-ui.lua.
 */
const uint8_t *uiArchive(size_t *size);

} // namespace twin
