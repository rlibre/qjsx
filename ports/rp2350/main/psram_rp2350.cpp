/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Set-up of the QSPI PSRAM on chip select 1 of the QMI (see psram_rp2350.h).
//
// The chips of these boards (APS6404L and alike: 8 MB, 3.3 V, QPI) answer
// the SPI "read ID" command with a "known good die" byte of 0x5D. The QMI
// is driven in direct mode to talk to the chip (exit QPI, read its ID, enter
// QPI), then its window M1 gets the chip's timings and its read (0xEB) and
// write (0x38) commands, all four bits wide. While direct mode is on, the
// XIP (the flash) cannot be read: that part runs from RAM, interrupts
// masked, before anything else uses the flash.
#include "psram_rp2350.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"

namespace board {

namespace {

// Commands of the chip.
const uint32_t COMMAND_READ_ID = 0x9f;       // SPI: 3 address bytes, then MFID, KGD, EID...
const uint32_t COMMAND_ENTER_QPI = 0x35;     // SPI -> QPI (4 bits per clock for everything)
const uint32_t COMMAND_EXIT_QPI = 0xf5;      // QPI -> SPI (sent 4 bits wide)
const uint32_t COMMAND_QUAD_READ = 0xeb;     // QPI fast read, 6 dummy clocks
const uint32_t COMMAND_QUAD_WRITE = 0x38;    // QPI write
const uint8_t  KNOWN_GOOD_DIE = 0x5d;        // byte 6 of the ID answer
const int      ID_ANSWER_BYTES = 7;          // command, 3 address bytes, MFID, KGD, EID
const int      KGD_INDEX = 5;                // where KGD comes in the answer
const uint32_t READ_DUMMY_UNITS = 6;         // dummy phase of the read, 4-bit units: 6 clocks

// Timings of the chip and of direct mode.
const uint32_t DIRECT_CLOCK_DIVISOR = 30;    // slow and safe for the ID (5 MHz at 150 MHz)
const uint32_t MAX_SELECT_NS = 8000;         // tCEM: chip select held at most 8 us (refresh)
const uint32_t MIN_DESELECT_NS = 18;         // tCPH: chip select released at least 18 ns
const uint32_t FAST_CLOCK_HZ = 100000000;    // above this, one more clock of read delay

// The values of the M1 registers, computed before going into RAM code.
struct WindowSetup {
    uint32_t timing;   // QMI M1_TIMING
    uint32_t rfmt;     // read format: widths, prefix, dummy
    uint32_t wfmt;     // write format
};

/**
 * Waits until the direct-mode transfer is over.
 */
static inline void __attribute__((always_inline)) waitDirect() {
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }
}

/**
 * Talks to the chip and programs window M1 (RAM, interrupts masked):
 * 1. direct mode on (the XIP is stopped from here);
 * 2. leave QPI, in case the chip is still in it (warm reset);
 * 3. read the ID: no known good die, direct mode off and false;
 * 4. enter QPI;
 * 5. window M1: timings, formats, commands; direct mode off; writes on.
 */
bool __no_inline_not_in_flash_func(setUpChip)(const WindowSetup *setup) {
    // direct mode, slow clock; wait for the last XIP access to cool down
    qmi_hw->direct_csr = DIRECT_CLOCK_DIVISOR << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    waitDirect();

    // leave QPI: the command 4 bits wide, its answer dropped
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | COMMAND_EXIT_QPI;
    waitDirect();
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // read the ID, one byte per transfer; keep the known good die byte
    uint8_t knownGoodDie = 0;
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (int index = 0; index < ID_ANSWER_BYTES; index++) {
        qmi_hw->direct_tx = index == 0 ? COMMAND_READ_ID : 0xff;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
        }

        waitDirect();
        uint8_t received = (uint8_t)qmi_hw->direct_rx;
        if (index == KGD_INDEX) {
            knownGoodDie = received;
        }
    }

    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    if (knownGoodDie != KNOWN_GOOD_DIE) {
        qmi_hw->direct_csr = 0;
        return false;
    }

    // enter QPI (no answer to collect)
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | COMMAND_ENTER_QPI;
    waitDirect();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // window M1
    qmi_hw->m[1].timing = setup->timing;
    qmi_hw->m[1].rfmt = setup->rfmt;
    qmi_hw->m[1].rcmd = COMMAND_QUAD_READ;
    qmi_hw->m[1].wfmt = setup->wfmt;
    qmi_hw->m[1].wcmd = COMMAND_QUAD_WRITE;

    // the XIP back, and writes to window M1 allowed
    qmi_hw->direct_csr = 0;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    return true;
}

/**
 * The register values of window M1 for the current system clock:
 *   - clock divisor: the chip's maximum, and at least 2 above 100 MHz;
 *   - read delay: the divisor, one more when the chip runs above 100 MHz;
 *   - max select (units of 64 system clocks) and min deselect (system
 *     clocks, less half a chip clock) from the chip's tCEM and tCPH;
 *   - pages of 1 KB, one clock of cool down;
 *   - every phase 4 bits wide, 8-bit command, 6 dummy clocks to read.
 */
WindowSetup computeSetup(uint32_t maxClockHz) {
    uint32_t systemHz = clock_get_hz(clk_sys);

    // clock and read delay
    uint32_t divisor = (systemHz + maxClockHz - 1) / maxClockHz;
    if (divisor < 2 && systemHz > FAST_CLOCK_HZ) {
        divisor = 2;
    }

    uint32_t readDelay = divisor;
    if (systemHz / divisor > FAST_CLOCK_HZ) {
        readDelay++;
    }

    // chip select limits, in system clocks
    uint64_t systemPerMicro = systemHz / 1000000;
    uint32_t maxSelect = (uint32_t)(MAX_SELECT_NS * systemPerMicro / 1000 / 64);
    uint32_t deselectClocks = (uint32_t)((MIN_DESELECT_NS * (uint64_t)systemHz + 999999999) / 1000000000);
    uint32_t halfChipClock = (divisor + 1) / 2;
    uint32_t minDeselect = deselectClocks > halfChipClock ? deselectClocks - halfChipClock : 0;

    WindowSetup setup;
    setup.timing = 1u << QMI_M1_TIMING_COOLDOWN_LSB
        | QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB
        | maxSelect << QMI_M1_TIMING_MAX_SELECT_LSB
        | minDeselect << QMI_M1_TIMING_MIN_DESELECT_LSB
        | readDelay << QMI_M1_TIMING_RXDELAY_LSB
        | divisor << QMI_M1_TIMING_CLKDIV_LSB;

    // formats: quad everywhere, 8-bit command prefix
    setup.rfmt = QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB
        | QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB
        | QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB
        | QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB
        | QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB
        | QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB
        | READ_DUMMY_UNITS << QMI_M1_RFMT_DUMMY_LEN_LSB;

    setup.wfmt = QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB
        | QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB
        | QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB
        | QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB
        | QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB
        | QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;

    return setup;
}

/**
 * Writes a pattern at the start, the middle and the end of the PSRAM, past
 * the cache, and reads it back. False if one word differs.
 */
bool checkPsram(size_t size) {
    volatile uint32_t *memory = (volatile uint32_t *)PSRAM_UNCACHED_WINDOW;
    size_t words = size / sizeof(uint32_t);
    size_t places[] = { 0, 1, words / 2, words - 1 };

    // write, then read everything back
    for (size_t place : places) {
        memory[place] = 0xa5c30000u ^ (uint32_t)place;
    }

    for (size_t place : places) {
        if (memory[place] != (0xa5c30000u ^ (uint32_t)place)) {
            return false;
        }
    }

    return true;
}

} // namespace

/**
 * 1. the pin becomes chip select 1;
 * 2. the window's registers, computed from flash code;
 * 3. the chip, from RAM, interrupts masked;
 * 4. a short write / read check.
 */
bool startPsram(int csPin, uint32_t maxClockHz, size_t size) {
    // the pin
    gpio_set_function((uint)csPin, GPIO_FUNC_XIP_CS1);

    // the registers
    WindowSetup setup = computeSetup(maxClockHz);

    // the chip
    uint32_t interrupts = save_and_disable_interrupts();
    bool answered = setUpChip(&setup);
    restore_interrupts(interrupts);
    if (!answered) {
        return false;
    }

    // what is written stays
    return checkPsram(size);
}

} // namespace board
