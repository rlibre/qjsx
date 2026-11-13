The API of qjsx, module by module, generated from the typings that `qjsx build` writes in
your project for VS Code.

| Modules | What they are |
|---|---|
| `globals` | `console`, the timers, `TextEncoder` / `TextDecoder`, `process` |
| `sys/...` | the system: files, hashes, network, compiler |
| `device/...` | the hardware the program runs on |
| `stdlib/...` | the standard library, written in JS |

## Compatibility

| Module | PC | ESP32 | RP2350 | Twin |
|---|---|---|---|---|
| globals | ✓ | ✓ | ✓ | ✓ |
| `sys/fs` | ✓ | ✓ ¹ | ✓ ¹ | ✓ its data folder |
| `sys/crypto` | ✓ | ✓ | ✓ | ✓ |
| `sys/net` | ✓ | — | — | — |
| `sys/compiler` | ✓ the qjsx tool | — | — | — |
| `device/gpio` | — | ✓ | ✓ | ✓ |
| `device/pwm` | — | ✓ | — | ✓ |
| `device/adc` | — | ✓ ADC1 | — | ✓ |
| `device/i2c` | — | ✓ | — | ✓ |
| `device/rmt` | — | ✓ | — | ✓ |
| `device/serial` | ✓ ports of the PC | ✓ UARTs | — | ✓ UARTs |
| `device/usb` | — | ✓ S2, S3, P4 | — | ✓ |
| `stdlib/path` | ✓ | ✓ | ✓ | ✓ |
| `stdlib/rgbled` | — | ✓ | — | ✓ |
| `stdlib/usb` | — | ✓ S2, S3, P4 | — | ✓ |

¹ The module is there, but the boards have no file system yet: every call fails.

PC: `qjsx run` and the programs of the PC. Twin: the digital twin of a board offers the
native modules of that board (here the YD-ESP32-S3, which has every module of the
ESP32). A board offers only the native modules of its firmware: `qjsx deploy` refuses a
program that imports another one.
