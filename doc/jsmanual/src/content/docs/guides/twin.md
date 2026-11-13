---
title: The digital twin
description: Run the program of a board on the PC, its hardware simulated.
---

The digital twin makes the PC play a board: the program written for the board runs on the
PC, deployed, monitored and debugged exactly as on the board. Its pins, parts, I2C buses,
UARTs, USB functions, PWM outputs and analog inputs are simulated, and a page of your
browser shows them.

## Start it

In the folder of a project whose `qjsx.json` has a `"twin"` section:

```bash
qjsx agent
```

From elsewhere: `qjsx agent -p <project folder>`. A bare board, without a project:
`qjsx agent --board yd-esp32-s3`.

The page of the twin is on http://127.0.0.1:8090 (`--ui-port` or `twin.ports.ui` to
change it). With `"target": "twin"` in `qjsx.json`, `qjsx deploy`, `qjsx debug` and
`qjsx monitor` talk to the twin.

## The "twin" section

```json
"target": "twin",
"twin": {
    "board": "yd-esp32-s3",
    "ports": { "agent": 5555, "ui": 8090 },
    "uarts": { "1": "COM5" },
    "parts": [
        { "id": "meteo", "model": "bme280", "label": "Weather",
          "i2c": { "sda": 8, "scl": 9 }, "address": "0x76" },
        { "id": "relay", "model": "./twin/relay", "label": "Relay", "pin": 5 }
    ]
}
```

| Key | Role |
|---|---|
| `board` | a board of qjsx (`yd-esp32-s3`) or the path of a board description (`.json`) |
| `ports` | the TCP ports of the agent (5555), the page (8090) and the links of the twin |
| `uarts` | UARTs of the board wired to serial ports of the PC from the start |
| `parts` | what is wired on the board: each part has an `id`, a `model`, a `label` and its own fields |

## Parts

A part is a model (JS, run by the twin, never by the program) and a view on the page.
qjsx brings `button`, `ws2812`, `bme280` and `potentiometer`. A project adds its own:
`"model": "./twin/relay"` loads `twin/relay/model.js` (and its optional `view.js`), written
with the public API `qjsx:twin` (pins, I2C devices, RMT, voltages).

## The page

- one folding section per device of the board: its parts, its UARTs (traffic in text or
  hexadecimal, sending, a TCP link or a serial port of the PC), its USB functions;
- the table of the pins: mode, level, what drives it, PWM;
- the analyzer: levels, PWM and RMT of the pins you choose, a trigger, a recording saved as
  a VCD file (PulseView, GTKWave);
- the console of the parts.

The twin runs at the speed of the PC, not of the board.
