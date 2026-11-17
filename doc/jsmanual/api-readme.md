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
| `stdlib/usb` | — | ✓ S2, S3, P4 | — | — | ✓ |

¹ The module is there, but the RP2350 and the i.MX RT1062 have no file system yet: every
call throws (`code` `"ENOSYS"`), except `existsSync`, which returns false.

² Only the chips that have a DAC: the original ESP32 and the ESP32-S2 (not the S3).

³ The twin of a board whose chip has one (`esp32-devkitc-wroom32`).

⁴ Only a firmware built for a board with a display has `sys/draw`, the renderer of
`stdlib/display`: today the WT32-SC01.

⁵ The twin of such a board (`wt32-sc01`): its screen is drawn in the page.

PC: `qjsx run` and the programs of the PC. i.MX RT1062: the Teensy 4.0. Twin: the digital twin of a board offers the
native modules of that board (here the YD-ESP32-S3, which has every module of the
ESP32-S3; `device/dac` in the twin of an original ESP32). A board offers only the native modules of its firmware: `qjsx deploy` refuses a
program that imports another one.
