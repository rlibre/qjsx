/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The view of a display part of qjsx (host/twin/parts/st7796-320x480): its
// pixels, one canvas pixel per pixel of the screen, and a line under them
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
import { BoxProps, Component, Label, VBox } from "x4js";
import type { PartHandle } from "./partview";
import { tx } from "./texts";

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

/**
 * The bytes of a base64 text.
 */
function decodeBase64(text: string) {
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
        return parts.join(" · ");
    }
}
