The API of qjsx, module by module, generated from the typings that `qjsx build` writes in
your project for VS Code.

| Modules | What they are |
|---|---|
| `globals` | `console`, the timers, `TextEncoder` / `TextDecoder`, `crypto`, `process` |
| `sys/...` | the system: files, hashes and random bytes, network, compiler, drawing |
| `device/...` | the hardware the program runs on |
| `stdlib/...` | the standard library, written in JS |

## Compatibility

| Module | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
|---|---|---|---|---|---|
| globals | ✓ | ✓ | ✓ | ✓ | ✓ |
| `sys/fs` | ✓ | ✓ /data | ✓ ¹ | ✓ ¹ | ✓ its data folder |
| `sys/crypto` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `sys/net` | ✓ | — | — | — | — |
| `sys/compiler` | ✓ the qjsx tool | — | — | — | — |
| `sys/draw` | — | ✓ boards with a display ⁴ | — | — | ✓ ⁵ |
| `device/board` | — | ✓ | — | ✓ | ✓ |
| `device/gpio` | — | ✓ | ✓ | ✓ | ✓ |
| `device/pwm` | — | ✓ | — | ✓ | ✓ |
| `device/adc` | — | ✓ ADC1 | — | — | ✓ |
| `device/dac` | — | ✓ ESP32, S2 ² | — | — | ✓ ³ |
| `device/i2c` | — | ✓ | — | — | ✓ |
| `device/spi` | — | ✓ | — | — | ✓ |
| `device/rmt` | — | ✓ | — | — | ✓ |
| `device/serial` | ✓ ports of the PC | ✓ UARTs | — | — | ✓ UARTs |
| `device/usb` | — | ✓ S2, S3, P4 | — | — | ✓ |
| `stdlib/display` | — | ✓ boards with a display ⁴ | — | — | ✓ ⁵ |
| `stdlib/fonts/inter16`, `inter24`, `inter40` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `stdlib/path` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `stdlib/rgbled` | — | ✓ | — | — | ✓ |
| `stdlib/tasks` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `stdlib/touch` | — | ✓ boards with a touch controller ⁶ | — | — | ✓ ⁶ |
| `stdlib/usb` | — | ✓ S2, S3, P4 | — | — | ✓ |

¹ The module is there, but the RP2350 and the i.MX RT1062 have no file system yet: every
call throws (`code` `"ENOSYS"`), except `existsSync`, which returns false.

² Only the chips that have a DAC: the original ESP32 and the ESP32-S2 (not the S3).

³ The twin of a board whose chip has one (`esp32-devkitc-wroom32`).

⁴ Only a firmware built for a board with a display has `sys/draw`, the renderer of
`stdlib/display`: the WT32-SC01 (ST7796 over SPI) and the two ESP32-P4 boards, the M5Stack
Tab5 and the Waveshare ESP32-P4-Module-DEV-KIT (panel built into the board).

⁵ The twin of such a board (`wt32-sc01`, `m5stack-tab5`): its screen is drawn in the page.

⁶ A touch controller on I2C: the WT32-SC01 (FT6336), the M5Stack Tab5 (GT911) and the
Waveshare ESP32-P4-Module-DEV-KIT (GT9271, read as a GT911). In the twins of the first two
boards, the mouse on the screen of the page touches it (one finger).

PC: `qjsx run` and the programs of the PC. i.MX RT1062: the Teensy 4.0. Twin: the digital twin of a board offers the
native modules of that board (here the YD-ESP32-S3, which has every module of the
ESP32-S3; `device/dac` in the twin of an original ESP32). A board offers only the native modules of its firmware: `qjsx deploy` refuses a
program that imports another one.

A Linux board (the STM32MP157-DK2) has no column: it runs the `qjsx` tool as `qjsx agent
--screen ... --touch ...`. A program has there the modules of the tool (the globals,
`sys/fs`, `sys/crypto`, `sys/net`, `device/serial`, `device/board`, the `stdlib` modules
written in JS), plus `sys/draw`, `stdlib/display` and `stdlib/touch`, both with the type
`"builtin"`.
