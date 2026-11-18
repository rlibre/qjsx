/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The view of a display part of qjsx (host/twin/parts/st7796-320x480,
// ili9881c-720x1280): its pixels, one canvas pixel per pixel of the screen
// (shrunk to fit the page when larger, then smoothed), and a line under them
// (its size, asleep / off / the backlight, where the USB connector of the
// board is).
//
// The model keeps the glass as seen with the board held USB connector at the
// bottom, and publishes what changed: { width, height, awake, on, inverted,
// brightness, turn, rects: [{ x, y, width, height, pixels }] }, the pixels
// in base64, 16 bits each (5 red, 6 green, 5 blue), high byte first. They
// are copied into an ImageData of the whole glass, kept between updates,
// then into a canvas of the glass's size, off the page, by one putImageData
// per update (the box of its rectangles); the canvas
// shown draws it turned back by 'turn', so that the program's image is
// upright, as the board is held to read it: rotation 90 of stdlib/display
// shows the USB connector at the right. Asleep or off: black; the backlight
// dims the image (its brightness as the opacity over black).
//
// A touch part over the screen (touchlink.ts): the mouse pressed, moved and
// released on it sends "press", "move" with { x, y, width, height } (the
// pixel of the glass under the mouse, and the glass's size) and "release" to
// that part. One finger.
import { BoxProps, Component, Label, VBox } from "x4js";
import type { PartHandle } from "./partview";
import { tx } from "./texts";
import { isTouchable, touchScreen } from "./touchlink";

// A rectangle of the glass the model sends.
interface ScreenRect {
    x:      number;
    y:      number;
    width:  number;
    height: number;
    pixels: string;     // base64: 16 bits a pixel, high byte first
}

// What the model publishes.
interface ScreenState {
    width:      number;     // the glass, pixels
    height:     number;
    awake:      boolean;    // out of sleep
    on:         boolean;    // display on
    inverted:   boolean;    // colors inverted
    brightness: number;     // backlight, 0 to 1
    turn:       number;     // the program's image turned on the glass: 0, 90, 180, 270 (clockwise)
    rects:      ScreenRect[];   // the pixels changed
}

// Uint8Array.fromBase64 of the browser (native: 2 ms for the 2.4 MB of a
// whole 720 x 1280 image, against about 110 ms by atob and a loop); not in
// the TypeScript library yet, hence this type.
type Base64Decoder = { fromBase64?: (text: string) => Uint8Array };

/**
 * The bytes of a base64 text: by the browser's decoder when it has one.
 */
function decodeBase64(text: string) {
    // native
    const decoder = Uint8Array as unknown as Base64Decoder;
    if (typeof decoder.fromBase64 === "function") {
        return decoder.fromBase64(text);
    }

    // by hand
    const binary = atob(text);
    const bytes = new Uint8Array(binary.length);

    for (let index = 0; index < binary.length; index++) {
        bytes[index] = binary.charCodeAt(index);
    }

    return bytes;
}

export class ScreenView extends VBox {
    private _screen: Component;                 // the canvas shown
    private _caption: Label;                    // the line under it
    private _glass: HTMLCanvasElement = null;   // the pixels as on the glass, off the page
    private _image: ImageData = null;           // the same pixels, as RGBA, to update the glass canvas
    private _look: ScreenState = null;          // the last state
    private _part: PartHandle;                  // the part, to ask it for the whole glass
    private _whole = false;                     // the whole glass came at least once
    private _askedAt = 0;                       // Date.now() of the last ask for it
    private _pressed = false;                   // the mouse down on it, a touch part told

    /**
     * An empty screen; the model is asked for all its pixels.
     */
    constructor(props: BoxProps, part: PartHandle) {
        super({ ...props, cls: "screen-view" });

        // the canvas, then the line under it
        this._screen = new Component({ tag: "canvas", cls: "screen" });
        this._caption = new Label({ cls: "screen-caption", text: "" });
        this.setContent([this._screen, this._caption]);

        // each state shown; the whole glass asked for (a state kept by the
        // twin may hold only the last rectangles)
        this._part = part;
        part.onState(state => this._show(state as unknown as ScreenState));
        this._askWhole();

        // the mouse: a finger on the touch panel over it, if any; shown
        // smaller than its pixels: smoothed
        const canvas = this._screen.dom as HTMLCanvasElement;
        new ResizeObserver(() => this._markScaled()).observe(canvas);
        canvas.addEventListener("pointerdown", event => this._onPress(event));
        canvas.addEventListener("pointermove", event => this._onMove(event));
        canvas.addEventListener("pointerup", () => this._onRelease());
        canvas.addEventListener("pointercancel", () => this._onRelease());
    }

    /**
     * The canvas is shown smaller than its pixels (the page shrinks it to
     * fit): class "scaled", smoothed.
     */
    private _markScaled() {
        const canvas = this._screen.dom as HTMLCanvasElement;
        const isShrunk = canvas.clientWidth > 0 && canvas.clientWidth < canvas.width;
        canvas.classList.toggle("scaled", isShrunk);
    }

    /**
     * The mouse pressed on the screen: the finger put there (the mouse
     * captured until it is released).
     */
    private _onPress(event: PointerEvent) {
        // no touch panel over it, or nothing shown yet
        const point = this._glassPoint(event);
        if (!isTouchable(this._part.id) || point === null) {
            return;
        }

        (this._screen.dom as HTMLElement).setPointerCapture(event.pointerId);
        this._pressed = true;
        touchScreen(this._part.id, "press", point);
    }

    /**
     * The mouse moved, pressed: the finger moved.
     */
    private _onMove(event: PointerEvent) {
        const point = this._pressed ? this._glassPoint(event) : null;
        if (point !== null) {
            touchScreen(this._part.id, "move", point);
        }
    }

    /**
     * The mouse released (or lost): the finger lifted.
     */
    private _onRelease() {
        if (this._pressed) {
            this._pressed = false;
            touchScreen(this._part.id, "release");
        }
    }

    /**
     * The pixel of the glass under the mouse, with the glass's size: the
     * canvas shows the glass turned back by 'turn' around its center, so the
     * point is turned by 'turn' around the center to find it. null before
     * the first state.
     */
    private _glassPoint(event: PointerEvent) {
        // nothing shown yet
        const look = this._look;
        if (look === null) {
            return null;
        }

        // the pixel of the canvas (shown maybe at another size)
        const canvas = this._screen.dom as HTMLCanvasElement;
        const shownX = (event.offsetX * canvas.width) / canvas.clientWidth;
        const shownY = (event.offsetY * canvas.height) / canvas.clientHeight;

        // from the center of the canvas, turned by 'turn' (clockwise), to the
        // center of the glass
        const angle = (look.turn * Math.PI) / 180;
        const dx = shownX - canvas.width / 2;
        const dy = shownY - canvas.height / 2;
        const glassX = dx * Math.cos(angle) - dy * Math.sin(angle) + look.width / 2;
        const glassY = dx * Math.sin(angle) + dy * Math.cos(angle) + look.height / 2;

        // a pixel of the glass
        const x = Math.max(0, Math.min(look.width - 1, Math.floor(glassX)));
        const y = Math.max(0, Math.min(look.height - 1, Math.floor(glassY)));
        return { x, y, width: look.width, height: look.height };
    }

    /**
     * Asks the model for the whole glass, once a second at most.
     */
    private _askWhole() {
        this._askedAt = Date.now();
        this._part.send("refresh");
    }

    /**
     * A state of the model: its pixels kept, the screen drawn again. Until
     * the whole glass came, it is asked again (an ask may be lost).
     */
    private _show(state: ScreenState) {
        // the whole glass: one rectangle as large as it
        const isWhole = state.rects.some(rect => rect.width === state.width && rect.height === state.height);
        this._whole = this._whole || isWhole;
        const isAskDue = Date.now() - this._askedAt > 1000;
        if (!this._whole && isAskDue) {
            this._askWhole();
        }

        // the glass at its size
        const isNewSize = this._glass === null || this._glass.width !== state.width || this._glass.height !== state.height;
        if (isNewSize) {
            this._glass = document.createElement("canvas");
            this._glass.width = state.width;
            this._glass.height = state.height;
            this._image = new ImageData(state.width, state.height);
        }

        // the pixels changed, then the screen and its line
        this._putRects(state.rects);
        this._look = state;
        this._paint();
        this._caption.setText(this._captionText());
    }

    /**
     * Copies the rectangles into the image of the glass, then the box of
     * them all into the glass canvas, at once.
     */
    private _putRects(rects: ScreenRect[]) {
        // none: nothing to copy
        if (rects.length === 0) {
            return;
        }

        // each one into the image
        for (const rect of rects) {
            this._putRect(rect);
        }

        // the box of them all, into the glass canvas
        const left = Math.min(...rects.map(rect => rect.x));
        const top = Math.min(...rects.map(rect => rect.y));
        const right = Math.max(...rects.map(rect => rect.x + rect.width));
        const bottom = Math.max(...rects.map(rect => rect.y + rect.height));
        this._glass.getContext("2d").putImageData(this._image, 0, 0, left, top, right - left, bottom - top);
    }

    /**
     * Copies a rectangle of 16-bit pixels into the image of the glass.
     */
    private _putRect(rect: ScreenRect) {
        const bytes = decodeBase64(rect.pixels);
        const data = this._image.data;
        const glassWidth = this._image.width;

        // each pixel as red, green, blue (the 5 or 6 bits spread to 8), opaque
        for (let row = 0; row < rect.height; row++) {
            for (let column = 0; column < rect.width; column++) {
                const from = (row * rect.width + column) * 2;
                const pixel = (bytes[from] << 8) | bytes[from + 1];
                const red = pixel >> 11;
                const green = (pixel >> 5) & 0x3f;
                const blue = pixel & 0x1f;
                const to = ((rect.y + row) * glassWidth + rect.x + column) * 4;
                data[to] = (red << 3) | (red >> 2);
                data[to + 1] = (green << 2) | (green >> 4);
                data[to + 2] = (blue << 3) | (blue >> 2);
                data[to + 3] = 255;
            }
        }
    }

    /**
     * Draws the glass on the canvas shown: black, then, when the display is
     * on, the glass turned back by 'turn', as bright as the backlight.
     */
    private _paint() {
        // the canvas shown: the glass's size, exchanged when on its side
        const look = this._look;
        const onItsSide = look.turn === 90 || look.turn === 270;
        const width = onItsSide ? look.height : look.width;
        const height = onItsSide ? look.width : look.height;
        const canvas = this._screen.dom as HTMLCanvasElement;
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
            this._markScaled();
        }

        // black: asleep, off, or no light
        const ctx = canvas.getContext("2d");
        ctx.save();
        ctx.fillStyle = "#000";
        ctx.fillRect(0, 0, width, height);
        const isLit = look.awake && look.on;
        if (!isLit) {
            ctx.restore();
            return;
        }

        // the glass, turned back around the center, dimmed by the backlight
        ctx.globalAlpha = look.brightness;
        ctx.filter = look.inverted ? "invert(1)" : "none";
        ctx.translate(width / 2, height / 2);
        ctx.rotate((-look.turn * Math.PI) / 180);
        ctx.drawImage(this._glass, -look.width / 2, -look.height / 2);
        ctx.restore();
    }

    /**
     * The line under the screen: "320 × 480 · backlight 100 % · USB
     * connector at the right".
     */
    private _captionText() {
        // its size
        const look = this._look;
        const parts = [`${look.width} × ${look.height}`];

        // asleep, off, or the backlight
        if (!look.awake) {
            parts.push(tx.twin.screen_asleep);
        } else if (!look.on) {
            parts.push(tx.twin.screen_off);
        } else {
            parts.push(`${tx.twin.screen_backlight} ${Math.round(look.brightness * 100)} %`);
        }

        // where the USB connector is, the image upright
        const side = tx.twin.screen_sides[(look.turn / 90) & 3];
        parts.push(`${tx.twin.screen_usb} ${side}`);

        // a touch panel over it
        if (isTouchable(this._part.id)) {
            parts.push(tx.twin.screen_touch);
        }

        return parts.join(" · ");
    }
}
