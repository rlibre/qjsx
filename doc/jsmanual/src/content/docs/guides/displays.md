---
title: Displays
description: Drawing on a color display wired on SPI with stdlib/display.
---

`stdlib/display` drives a color display wired on SPI: shapes, smoothed arcs and rounded
rectangles, text in proportional fonts, images with transparency. It is written in JS over `device/spi`, and its
pixels are made by `sys/draw`, a native renderer that only the firmware of a board with a
display has (today the WT32-SC01) and the twin of such a board. The controller known
today is the ST7796 (320 x 480 pixels). Every call is described in the API reference of
[stdlib/display](../../api/stdlib/display/readme/).

## Open it

The WT32-SC01 has its 3.5" ST7796 on SPI2:

```js
import { Display } from "stdlib/display";

const lcd = new Display({
    type: "st7796",
    port: 2,
    sclk: 14, mosi: 13, cs: 15, dc: 21,
    reset: 22,          // pulsed when the display opens
    backlight: 23,      // a PWM output, fully on at first
    rotation: 90,       // landscape: 480 x 320
});

console.log(lcd.width, lcd.height);   // 480 320
lcd.backlight(0.5);                   // 0 (off) to 1 (full)
```

`rotation` turns the image clockwise: 0 is portrait with the USB connector of the
WT32-SC01 at the bottom, 90 and 270 exchange `width` and `height`. The origin (0, 0) is
the top left of the screen as the program sees it. The constructor blocks the program for
about a quarter of a second (the controller wants its waits), so open the display once,
at the start.

`close()` frees the bus and the pins; the backlight goes off, the screen keeps its image.

## Draw, then show

The drawing calls draw nothing: each one adds a command of a few bytes to a list. There
is no copy of the screen in memory (a 480 x 320 image would take 300 KB).
`await lcd.show()` hands the list to the task of the SPI bus, which renders the image a
band of 16 rows at a time while it sends them:

```js
lcd.background = 0x102030;                       // colors: 0xRRGGBB
lcd.fillRect(10, 10, 200, 40, 0xff0000);
lcd.rect(10, 60, 200, 40, 0xffffff);             // one pixel thick
lcd.line(0, 0, lcd.width - 1, lcd.height - 1, 0x00ff00);
await lcd.show();                                // now it is on the screen
```

Colors are numbers `0xRRGGBB`; the display has 16 bits per pixel (5 of red, 6 of green,
5 of blue), so close colors may look the same. Coordinates are whole pixels: a fraction
is cut, what lies outside the screen is cut too. The drawing calls check nothing (they
are called often, on a microcontroller): a wrong argument draws something wrong, it does
not throw.

The list has room for 256 commands by default (`new Display({ commands: 512, ... })` for
more; a text takes one more item per character). A command that does not fit is dropped
and counted in `lcd.dropped`: read it before `show()`, which sets it back to 0.

## The background rule

An image is made of everything drawn since the last `show()`: the display does not
remember the image before. What no command covers in the area shown takes
`lcd.background`. So each image draws again everything of the area it shows, in order,
the later commands over the earlier ones; there is nothing to erase.

## Show one area

`show(x, y, width, height)` sends one area only: the commands are drawn again there, cut
to it, and the rest of the screen keeps what it shows. A value that changes is redrawn
alone:

```js
function showValue(value) {
    lcd.text(20, 200, `${value.toFixed(1)} °C`, big, 0xffff00);
    return lcd.show(20, 200, 200, big.lineHeight);   // the rest of the area: background
}
```

An area costs its pixels: the whole 480 x 320 screen is 300 KB on a wire of 40 MHz, about
61 ms before any rendering; an area of 200 x 30 is shown in about 8 ms. Show the smallest
area that holds what changed.

## One image at a time

While an image goes out, the program goes on, and the list is already empty for the next
image. A `show()` called before the previous one is over does nothing: its promise
resolves with `false` and the list is kept. To get the most images per second, list the
next image while the previous one is sent:

```js
let sending = Promise.resolve(true);
for (;;) {
    drawGauges(lcd);       // lists the next image while the previous one goes out
    await sending;         // the previous image is on the screen
    sending = lcd.show();
}
```

## Arcs, circles and rounded rectangles

Their edges are smoothed (blended over what is below them). Angles are in degrees, 0 at
the top, clockwise; an arc goes from its `radius` (its outer edge) inward over its
`thickness`, and its ends are cut along the radius. An arc as thick as its radius has no
hole: on part of a turn it is a filled sector, a slice of pie (the way to draw one). A
gauge:

```js
function drawGauge(cx, cy, value) {          // value: 0 to 1
    const start = -135;
    const sweep = 270;
    lcd.arc(cx, cy, 100, start, start + sweep, 16, 0x303030);           // the track
    lcd.arc(cx, cy, 100, start, start + sweep * value, 16, 0x30c060);   // the value
    lcd.disc(cx, cy, 6, 0xffffff);
}
```

`circle(cx, cy, radius, thickness, color)` is a whole ring, `disc(cx, cy, radius, color)`
a filled one. `fillRoundRect(x, y, width, height, radius, color)` and `roundRect(x, y,
width, height, radius, thickness, color)` (its outline, inside its box) have the same
radius at the four corners, at most half the smaller side.

## Text

### Make a font file

The display draws fonts in LVGL's binary format, made WITHOUT compression by
[lv_font_conv](https://github.com/lvgl/lv_font_conv) (an npm package, MIT; the qjsx
repository keeps it in `tools/fonts/`). Choose the size, the characters and the bits per
pixel (4 gives smooth edges):

```bash
npx lv_font_conv --font Inter-Regular.ttf --size 16 --bpp 4 \
    -r 0x20-0x7E,0xA0-0xFF --format bin --no-compress --no-kerning \
    -o fonts/inter16.bin
```

`--no-compress` is required (a compressed file is refused); the display ignores kerning,
so `--no-kerning` only makes the file smaller. Each size is a file: Inter 16 px with
Latin-1 takes about 11 KB. Check the license of the font you convert.

### Use it

A font file is imported as bytes: it stays in the package (in flash on the board), it is
read in place and never copied to RAM.

```js
import inter16 from "./fonts/inter16.bin" with { type: "bytes" };
import inter40 from "./fonts/inter40.bin" with { type: "bytes" };

const label = lcd.font(inter16);
const big = lcd.font(inter40, { tabularDigits: true });

lcd.text(10, 10, "Température", label, 0xffffff);   // (10, 10): the top left of the line
lcd.text(10, 40, "21.5 °C", big, 0xffff00);
```

- `font()` returns a handle for `text()` and `textWidth()` of the same display, with the
  font's metrics in pixels: `size`, `ascent` (top of the line to the baseline),
  `descent` (0 or negative), `lineHeight`, `capHeight` (the height of the capitals above
  the baseline). To center a text on `cy` as the eye sees it, center its capitals, not
  its line: `y = cy - font.ascent + (font.capHeight >> 1)`.
- A text is ONE line; a character its font lacks is skipped (a line break too). The next
  line is `lineHeight` lower.
- A display takes at most 16 font files; the same file with other options counts once.
- `tabularDigits: true` gives every digit the width of the widest one: a value that
  changes does not make what follows it move.

`textWidth()` measures a text as `text()` draws it, to center or align it:

```js
const value = "21.5 °C";
const x = (lcd.width - lcd.textWidth(value, big)) >> 1;   // centered
lcd.text(x, 120, value, big, 0xffff00);
```

## Images

A PNG file of the project is imported as an image: `qjsx build` converts it on the PC
into the format the display draws, and that is what the package holds, read in place
(in flash on the board):

```js
import logoFile from "./logo.png" with { type: "image" };

const logo = lcd.image(logoFile);                       // its width and height, in pixels
lcd.drawImage(lcd.width - logo.width - 8, 8, logo);     // its top left pixel at (x, y)
```

- Its transparency is kept: a transparent pixel leaves what is under it, a partly
  transparent one (the smoothed edge of a logo) is blended over it. Draw the image after
  what it lies on.
- Its colors go down to 16 bits; they are dithered on the way, so a gradient shows no
  bands.
- Every kind of PNG is read (grey, RGB, palette, with or without alpha, 8 or 16 bits);
  an interlaced one is refused (save it again without interlacing).
- No scaling, no rotation: the image is drawn at the size of its file.
- Its size in the package depends on its content: transparent areas and areas of one
  color cost almost nothing; a logo of 96 x 96 with a gradient takes about 13 KB, a photo
  of the whole screen about 300 KB (the WT32-SC01 holds packages of 512 KB).
- A display takes at most 16 images.

Icons of one color (pictograms) are better as a font: an icon font such as Font Awesome
Free, converted by `lv_font_conv` with the icons you need (`-r 0xf015,0xf013`, `--bpp 2`
or `4`), drawn with `text()` in any color, a few hundred bytes an icon. Keep the
attribution its license asks for in your project.

## Speed

Measured on the WT32-SC01 (original ESP32 at 240 MHz, display at 40 MHz):

| What | Time |
|---|---|
| listing one command | a few microseconds |
| the whole screen (480 x 320) with shapes and text | about 70 ms |
| two gauges of 448 x 250, redrawn and shown in a loop | 15.5 images per second |
| a value of text changed in two areas | about 40 ms |
| an area of 200 x 30 | about 8 ms |

The rendering runs on the task of the SPI bus, not on the JS thread: while an image goes
out, the program lists the next one, reads its sensors, answers its links.

## The display built into the board

On a board whose display is part of it (the M5Stack Tab5: 5", 720 x 1280, MIPI-DSI),
`type: "builtin"` takes it: no bus and no pins to give, but the backlight.

```js
const lcd = new Display({ type: "builtin", backlight: 22, rotation: 90 });   // 1280 x 720
```

The drawing calls are the same. The panel shows an image held in memory; `show()` draws
the area into it on a task of its own, and its promise is settled once the screen shows
it: the program goes on meanwhile, as with a display on SPI. Measured on the Tab5: the
whole screen in about 120 ms, an area of 300 x 60 shown again and again 189 times a second
(the JS thread busy 60 % of the time). `"builtin"` on a board without such a display is a
`RangeError`.

### On a Linux board

A board that runs Linux (tried on the STM32MP157-DK2: 480 x 800 DSI panel) has no qjsx
firmware: the `qjsx` tool itself, built for the board, runs there as `qjsx agent` and takes
the display from Linux.

```bash
qjsx agent --host 0.0.0.0 --port 5555 --storage /home/root/qjsx-storage --screen /dev/dri/card0 --touch /dev/input/event1
```

From the PC the board is then a device like the others, reached by the network
(`"target": "tcp:192.168.1.17:5555"` in `qjsx.json`): `qjsx deploy`, `qjsx debug`. In the
program, the display is `"builtin"` and so is its touch panel:

```js
const lcd = new Display({ type: "builtin", rotation: 90 });       // 800 x 480 on the DK2
const touch = new Touch({ type: "builtin", display: lcd });
```

- `--screen <device>` is the display (`/dev/dri/card0`, or `card1` on a board whose
  `card0` is its GPU). Stop the desktop of the board first (Weston, a display manager):
  only one program drives a display, and the agent says so when another one has it.
- `--touch <device>` is the touch panel as the kernel reads it (`/dev/input/eventN`).
  The fingers arrive in the coordinates of the drawing, as with any `Touch`.
- `--screen-size 800x480` is the size the program sees, when it is not the display's
  own: the display is set to that mode if it has it, else to the smallest mode that holds
  it, with the image centered and black around. The touch follows the image; a finger in
  the black border is ignored. Handy on a monitor, while the panel of the product is not
  there yet.
- A mouse given as `--touch` is one finger: it is on the screen while the left button is
  held, and an arrow shows where it is.

## Touch

`stdlib/touch` reads the touch controller of a display, on I2C, and gives the fingers in
the coordinates of the drawing (the size and rotation of the display): a finger lands where
the program drew. On a Linux board the system reads the controller: `type: "builtin"`,
with no bus and no pin (see "On a Linux board" above); the rest is the same.

```js
import { Touch } from "stdlib/touch";

// WT32-SC01: FT6336, 2 fingers
const touch = new Touch({ type: "ft6336", port: 0, sda: 18, scl: 19, interrupt: 39, display: lcd });

// M5Stack Tab5: GT911, 10 fingers (its firmware starts the controller)
const touch = new Touch({ type: "gt911", port: 0, sda: 31, scl: 32, interrupt: 23, display: lcd });

touch.onChange(() => {
    for (let i = 0; i < touch.count; i++) {
        const point = touch.points[i];          // { id, x, y }
        lcd.disc(point.x, point.y, 6, 0xff4040);
    }
    lcd.show();
});
```

- `onChange()` is called when a finger comes or goes, or moves by `threshold` pixels or
  more (2 by default: a finger held still trembles by a pixel or two).
- While a finger is on the screen, the controller is read `rate` times a second at most
  (33 by default): it makes about 100 reports a second, the program does not need them
  all. With the `interrupt` pin, nothing is read while nobody touches; without it, the
  controller is read `rate` times a second, always.
- `touch.points` is made once: its objects are updated in place, nothing is made at each
  move. Keep a copy of `x` and `y` if you need a position later. `touch.points[0]` read in
  a loop of your own gives the finger of the moment, without any callback.
- `id` is the controller's number of a finger: the same while it stays on the screen, so
  that two fingers can be followed apart.
- A bus shared with other devices: open it yourself and give it as `bus`; the touch leaves
  it open at `close()`.
- A touch panel mounted otherwise than its display (fingers seen mirrored, or moving
  across when they move along): `swapXY`, `mirrorX`, `mirrorY` undo how it is mounted, the
  controller's x and y exchanged first, then mirrored. They are a setting of the board, not
  of the controller. The WT32-SC01 and the Tab5 need none.

Draw only what falls in the area you show: listing a command costs JS time (about 0.2 ms
for a disc on the WT32-SC01), and a drawing program that lists every dot it ever drew at
each move slows down as the drawing grows. Measured on the WT32-SC01 with a finger
drawing: 30 changes a second, listing and showing a small area about 6 + 10 ms.

## In the twin

The twin of the WT32-SC01 (`qjsx agent --board wt32-sc01`, or `"board": "wt32-sc01"` in
the `"twin"` section of `qjsx.json`) has `sys/draw` and the screen: the page draws it, its
backlight included. Its SPI takes the time of the wire (the bytes at the clock of the
display), so a program that shows too many pixels is as slow there as on the board. The
rendering runs at the speed of the PC: the gauges above make about 21 images per second
in the twin.

The touch panel is there too: the FT6336 of the WT32-SC01 is a part of its twin (`touch`),
and pressing, dragging and releasing the mouse on the screen of the page touches it, one
finger. A project may add a GT911 the same way, in its `"twin"` parts:

```json
{ "id": "gt", "model": "gt911", "i2c": { "sda": 18, "scl": 19 }, "address": "0x14",
  "interrupt": 27, "screen": "screen", "range": [640, 960] }
```

Both answer as the chips do (their registers, a report every 10 ms while a finger is down,
the interrupt pin), so a driver written for the real chip works there too.

The twin of the M5Stack Tab5 (`qjsx agent --board m5stack-tab5`) has its display built in
and its GT911: `type: "builtin"` draws into an image of 720 x 1280 in memory, as on the
board, and the page shows it shrunk to fit; the mouse on it touches the GT911.
