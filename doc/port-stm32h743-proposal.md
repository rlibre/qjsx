# Proposal: a qjsx port for the STM32H743 (WeAct Studio MiniSTM32H7xx)

> **Status: proposal set aside by the user on 2026-11-18, to take up later.** Nothing is
> coded, nothing is downloaded. The user's answers so far are given below; the questions
> of section 4 are still open. Replaces, for this board, `doc/port-stm32h755-proposal.md`
> (a Nucleo-H755 never bought); read `ports/imxrt1062/` (Teensy 4.0, another Cortex-M7
> without PSRAM) before any code.

## 1. The board

WeAct Studio MiniSTM32H7xx with an **STM32H743VIT6** (user, 2026-11-18): Cortex-M7 at up
to 480 MHz, 2 MB of internal flash (sectors of 128 KB), about 1 MB of RAM in several
blocks, LQFP100.

Facts read on 2026-11-18 in WeAct's repository (github.com/WeActStudio/MiniSTM32H7xx,
the CubeMX `.ioc` files of `SDK/HAL/STM32H743/`; the schematic PDF could not be read).
WeAct's code is GPL-3.0: only these facts are taken, no code.

| What | Fact |
|---|---|
| Revisions | V1.0, V1.1, V1.2 (current files labelled V12) |
| Clocks | HSE crystal 25 MHz (PH0 / PH1), LSE 32.768 kHz (PC14 / PC15) |
| Supply | `PWR_LDO_SUPPLY` in the examples (the H743 has no SMPS) |
| QSPI flash | 8 MB, QUADSPI bank 1: CLK PB2, NCS PB6, IO0 PD11, IO1 PD12, IO2 PE2, IO3 PD13; mode 3; memory-mapped at 0x90000000; part number not named (a W25Q64 probably: read its JEDEC id) |
| SPI flash | a second 8 MB on SPI1: SCK PB3, MISO PB4, MOSI PD7, CS PD6 |
| USB | USB-C on USB OTG FS (PA11 / PA12), device only; no USB-to-serial chip; no VBUS sense pin found |
| microSD | SDMMC1, 4 bits: D0 PC8, D1 PC9, D2 PC10, D3 PC11, CK PC12, CMD PD2; no card-detect pin found |
| Buttons | K1 on PC13 (pull-down in the examples: high when pressed, to check), BOOT0, NRST |
| LED | probably PE3, active level unknown (the examples disagree) |
| Screen | 0.96" ST7735 (160 x 80?) on SPI4, transmit only: SCK PE12, MOSI PE14, CS PE11, DC PE13, backlight PE10 (TIM1_CH2N PWM, active low); no reset pin found |
| Camera | 8-bit DCMI (D0 PC6, D1 PC7, D2 PE0, D3 PE1, D4 PE4, D5 PD3, D6 PE5, D7 PE6, HSYNC PA4, VSYNC PB7, PIXCLK PA6, XCLK PA8 on MCO1), SCCB on I2C1 (SCL PB8, SDA PB9) |
| Debug | a 4-pin SWD header (PA13 / PA14) |

To check on the board itself: the LED pin and level, the flash part numbers, the LCD reset.

## 2. Answers of the user (2026-11-18)

1. Base: **STM32CubeH7** (HAL / LL), only the parts needed.
2. Link to the host: **native USB CDC**.
3. Flashing: **DFU** over USB.
4. Packages: in the **QSPI flash**, read in place.
5. Order: **the Teensy's** (minimal firmware, speed measures, then the hardware modules).

## 3. The proposal

- **Layout**: `ports/stm32h7/` with `boards/weact-h743/`; CMake + Ninja and a
  `tools/stm32.ps1` on the model of `tools/teensy.ps1`. The Arm compiler and FreeRTOS are
  in `C:\tools\pico` already (FreeRTOS port `ARM_CM7` r0p1, or `ARM_CM4F` as the Teensy).
- **ST drivers**: the HAL / LL driver (BSD-3) and the CMSIS description of the device
  (Apache-2.0), a pinned version, licences checked before the download.
- **USB**: TinyUSB (MIT), already on the disk with the pico-sdk, which supports the USB OTG
  of the STM32H7; ST's USB library avoided (its licence to check).
- **Memory**:

  | Region | Use |
  |---|---|
  | AXI SRAM, 512 KB at 0x24000000 | JS heap (RegionHeap) |
  | DTCM, 128 KB | .data / .bss, stacks |
  | ITCM, 64 KB | hot code of the engine |
  | SRAM1-3 (D2), 288 KB | C heap, USB buffers |

  Caches on (MPU), the DMA buffers in a region without cache.
- **Flash**: the firmware in the internal flash; the two package slots in the QSPI, read in
  place (memory-mapped), `/data` later (LittleFS on the QSPI, or the microSD). While the
  QSPI is written it leaves the memory-mapped mode: the agent stops the program before an
  upload already.
- **Clock**: 400 MHz to start (VOS1, always safe), 480 MHz once the revision of the chip is
  known (480 needs revision V and VOS0).
- **Flashing**: the first time in DFU with BOOT0 held; then without the button, as the
  Teensy: the host opens the USB port at 1200 bauds, the firmware jumps into the system
  bootloader of the chip, the DFU tool writes.
- **Order**: 1. minimal firmware (agent, upload to the QSPI, run in place, the debugger /
  proxy / cdp suites); 2. speed measures; 3. `device/board` (96-bit unique id),
  `device/gpio` (LED, K1), random numbers (the RNG of the chip), `device/pwm`.

## 4. Questions still open

1. TinyUSB rather than ST's USB stack?
2. Flashing tool: `dfu-util` (small, GPL for the tool only) or STM32CubeProgrammer
   (official, heavy)?
3. 400 MHz first, 480 MHz later?
4. The downloads to approve (the exact list, provenance and known vulnerabilities shown
   first).
