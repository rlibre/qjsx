# Changelog

What changes for who uses qjsx, release by release. The version in progress comes first,
without a date: it is completed as the work goes, and `tools/release.sh` dates it when it
is released, then opens the next one above it.

Version numbers: `0.Y.Z`. `Y` changes when a release may require a change in a program,
or a firmware to be flashed again; `Z` for fixes only.

## 0.3.0

A program that uses what is new here needs the firmware of this version on its board.

### Added

- `qjsx flash --port <port> (--board <name> | --firmware <folder>)`: writes the firmware
  of an ESP32 board through its serial port, with nothing else to install (no Python, no
  esptool, no ESP-IDF). Tried on an ESP32-P4 and an original ESP32.
- Boards: M5Stack Tab5 (ESP32-P4, its built-in 5" display and touch), Waveshare
  ESP32-P4-Module-DEV-KIT with its 7-DSI-TOUCH-A display, and boards that run Linux
  (tried on the STM32MP157-DK2): `qjsx agent --screen <DRM device> --touch <input
  device>`, with `--screen-size` to center a smaller image.
- `stdlib/display`: the display built into a board (`type: "builtin"`); images
  (`import logo from "./logo.png" with { type: "image" }`, `lcd.image()`,
  `lcd.drawImage()`); `capHeight` in the font handles, to center a text on its capitals.
- `stdlib/touch`: touch controllers GT911 and FT6336, and the touch panel a Linux board
  reads itself (`type: "builtin"`).
- `device/serial`: `setBaudRate()` on an open port; on the PC, `setSignals({ dtr, rts })`.
- Twin: the M5Stack Tab5 with its display and touch; touch panels touched with the mouse.

### Changed

- A display on SPI receives each image as one transaction: faster small updates.
- Waveshare ESP32-P4: the backlight stays off while the display starts.
- `qjsx.exe` (Windows) is one static executable: it needs no MinGW DLL next to it.

### Fixed

- A synchronous I2C call (`writeSync`, `readSync`...) made right after a queued transfer
  on the same bus could block the program for good, on every board.
- A package whose bytecode ends in the middle of an instruction is refused instead of
  being read past its end.
- Twin: two quick clicks on a GT911 touch panel were seen as one touch.

## 0.2.0 - 2026-11-17

Displays (`stdlib/display`, `sys/draw`: text with fonts, arcs, rounded rectangles),
`device/dac`, `device/spi`, files imported as bytes.
