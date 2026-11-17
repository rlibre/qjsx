/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A meter of a voltage, between a panel voltmeter and a digital one: a wide
// and low dial graduated in volts from 0 to its full scale, a band of
// segments under the graduations lit up to the voltage, a short needle on
// the graduations, and under them the name of what is measured with the
// voltage in digits. The pivot of the needle is far under the dial (not
// drawn): that is what keeps the dial low. Drawn flat, in the colors of the
// theme (main.scss), at a fixed size: one unit of the drawing is one pixel,
// so its texts have the size of the text of the page.
import { Component, ComponentProps, SvgBuilder, SvgComponent, SvgPath, SvgProps, SvgText } from "x4js";

// The drawing, in pixels.
const WIDTH = 160;
const HEIGHT = 78;

// The pivot of the needle (under the drawing), and the graduated arc around
// it.
const PIVOT_X = WIDTH / 2;
const PIVOT_Y = 150;
const SCALE_RADIUS = 112;
const SWEEP = 35;                           // degrees on each side of the top

// The marks, from the arc outward; the numbers beyond them.
const MAJOR_MARK = 7;
const MINOR_MARK = 4;
const NUMBER_GAP = 9;                       // from the end of a major mark to the middle of its number

// The band of segments, inside the arc.
const SEGMENT_COUNT = 33;
const BAND_GAP = 7;                         // from the arc to the middle of the band
const BAND_WIDTH = 6;
const SEGMENT_SPACE = 0.5;                  // degrees left empty between two segments

// The needle: only its end, across the band and the graduations.
const NEEDLE_FROM = SCALE_RADIUS - 16;
const NEEDLE_TO = SCALE_RADIUS + 3;

// The line of text under the dial: the name ends, and the voltage starts,
// on each side of the middle.
const TEXT_Y = HEIGHT - 9;
const TEXT_GAP = 3;                         // from the middle to each text

// The steps a dial may count by, in volts: the first that gives at most
// MOST_NUMBERS numbers is taken.
const STEPS = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50];
const MOST_NUMBERS = 5;

// Marks per step: one at the number, one halfway to the next (user: no
// more).
const MARKS_PER_STEP = 2;

// What the digits show when there is no voltage to show.
const NO_VOLTAGE = "-";

/**
 * The point at 'radius' from the pivot, 'angle' degrees from the top
 * (clockwise).
 */
function pointAt(angle: number, radius: number) {
    const radians = angle * Math.PI / 180;
    return { x: PIVOT_X + radius * Math.sin(radians), y: PIVOT_Y - radius * Math.cos(radians) };
}

export class Meter extends Component<ComponentProps> {
    static classPrefix = "twin";

    private _fullScale: number;             // volts at the end of the dial
    private _segments:  SvgPath[];          // the band, from 0 to the full scale
    private _needle:    SvgPath;            // drawn pointing up, turned around the pivot
    private _digits:    SvgText;            // the voltage in digits

    /**
     * A dial from 0 to 'fullScale' volts named 'name', with no voltage yet.
     */
    constructor(props: ComponentProps, fullScale: number, name: string) {
        super(props);
        this._fullScale = fullScale;

        // the dial, the needle over it, the texts under it
        const drawing = new SvgBuilder();
        this._segments = this._drawBand(drawing);
        this._drawScale(drawing);
        this._needle = this._drawNeedle(drawing);
        this._digits = this._drawTexts(drawing, name);

        // at its fixed size
        const svg = new SvgComponent<SvgProps>({ viewbox: `0 0 ${WIDTH} ${HEIGHT}`, svg: drawing });
        svg.setAttribute("width", WIDTH);
        svg.setAttribute("height", HEIGHT);
        this.setContent(svg);

        this.setVoltage(null);
    }

    /**
     * Shows 'volts' (held at the ends of the dial; the digits show it as it
     * is); null: no voltage to show (the needle at 0, no segment lit, no
     * digits).
     */
    setVoltage(volts: number | null) {
        // how far along the dial, 0 to 1
        const shown = volts ?? 0;
        const share = Math.min(1, Math.max(0, shown / this._fullScale));

        // the needle
        this._needle.rotate(this._angleOf(share * this._fullScale), PIVOT_X, PIVOT_Y);

        // the segments, lit up to there
        const litCount = Math.round(share * SEGMENT_COUNT);
        this._segments.forEach((segment, index) => {
            if (index < litCount) {
                segment.addClass("lit");
            } else {
                segment.removeClass("lit");
            }
        });

        // the digits (x4's SvgText has no method to change its text)
        this._digits.getDom().textContent = volts === null ? NO_VOLTAGE : `${volts.toFixed(2)} V`;
    }

    /**
     * Where 'volts' is on the dial, in degrees from the top.
     */
    private _angleOf(volts: number) {
        return -SWEEP + 2 * SWEEP * volts / this._fullScale;
    }

    /**
     * The band of segments under the graduations, along the whole dial,
     * none lit.
     */
    private _drawBand(drawing: SvgBuilder) {
        const segmentSweep = 2 * SWEEP / SEGMENT_COUNT;
        const segments: SvgPath[] = [];

        for (let index = 0; index < SEGMENT_COUNT; index++) {
            // its share of the dial, less the space before the next
            const from = -SWEEP + index * segmentSweep;
            const to = from + segmentSweep - SEGMENT_SPACE;

            const segment = drawing.path()
                .arc(PIVOT_X, PIVOT_Y, SCALE_RADIUS - BAND_GAP, from, to)
                .no_fill()
                .strokeWidth(BAND_WIDTH)
                .addClass("segment");
            segments.push(segment);
        }

        return segments;
    }

    /**
     * The graduations: the arc, a mark at each step with its number, a
     * shorter one halfway between two.
     */
    private _drawScale(drawing: SvgBuilder) {
        // the arc
        drawing.path()
            .arc(PIVOT_X, PIVOT_Y, SCALE_RADIUS, -SWEEP, SWEEP)
            .no_fill()
            .addClass("scale");

        // the step between two numbers, and how its numbers are written
        const step = STEPS.find(candidate => this._fullScale / candidate <= MOST_NUMBERS) ?? STEPS[STEPS.length - 1];
        const decimals = step < 1 ? 1 : 0;

        // the marks, counted in half steps (whole numbers: no rounding
        // drift), up to the full scale
        const markCount = Math.floor(this._fullScale / step * MARKS_PER_STEP + 1e-9);

        for (let mark = 0; mark <= markCount; mark++) {
            const volts = mark * step / MARKS_PER_STEP;
            const angle = this._angleOf(volts);
            const isMajor = mark % MARKS_PER_STEP === 0;
            const length = isMajor ? MAJOR_MARK : MINOR_MARK;

            // the mark, from the arc outward
            const from = pointAt(angle, SCALE_RADIUS);
            const to = pointAt(angle, SCALE_RADIUS + length);
            drawing.path()
                .moveTo(from.x, from.y)
                .lineTo(to.x, to.y)
                .addClass("scale");

            // its number, beyond a major mark
            if (isMajor) {
                const at = pointAt(angle, SCALE_RADIUS + MAJOR_MARK + NUMBER_GAP);
                drawing.text(at.x, at.y, volts.toFixed(decimals))
                    .textAlign("center")
                    .verticalAlign("center")
                    .addClass("number");
            }
        }
    }

    /**
     * The end of the needle, pointing up, across the band and the
     * graduations.
     */
    private _drawNeedle(drawing: SvgBuilder) {
        return drawing.path()
            .moveTo(PIVOT_X, PIVOT_Y - NEEDLE_FROM)
            .lineTo(PIVOT_X, PIVOT_Y - NEEDLE_TO)
            .addClass("needle");
    }

    /**
     * The line under the dial: 'name', then the voltage in digits (returned,
     * empty).
     */
    private _drawTexts(drawing: SvgBuilder, name: string) {
        drawing.text(PIVOT_X - TEXT_GAP, TEXT_Y, name)
            .textAlign("right")
            .verticalAlign("center")
            .addClass("name");

        return drawing.text(PIVOT_X + TEXT_GAP, TEXT_Y, "")
            .textAlign("left")
            .verticalAlign("center")
            .addClass("digits");
    }
}
