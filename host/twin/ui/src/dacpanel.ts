/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section "DAC" of a board shows: a meter (controls/meter.ts) per
// analog output of the board, side by side, each with its pin and the
// voltage the program's DAC drives (device/dac). The meter of an output the
// program does not drive is dimmed, with no voltage.
import { BoxProps, HBox } from "x4js";
import { Meter } from "./controls/meter";
import { DacInfo, PinState } from "./messages";

// Volts of the highest code, for a board that does not tell.
const DEFAULT_FULL_SCALE = 3.3;

// One output and its meter.
interface Output {
    pin:   number;
    meter: Meter;
}

/**
 * Whether a DAC of the program drives a pin (not known yet: no).
 */
function isDriven(state: PinState | undefined): state is PinState {
    return state !== undefined && state.dac;
}

/**
 * The title line of the section: each output and its voltage ("GPIO25 1.20
 * V, GPIO26 -").
 */
export function dacSummary(dac: DacInfo, pins: PinState[]) {
    return dac.pins.map(pin => {
        const state = pins[pin];
        const voltage = isDriven(state) ? `${state.voltage.toFixed(2)} V` : "-";
        return `GPIO${pin} ${voltage}`;
    }).join(", ");
}

export class DacPanel extends HBox {
    private _outputs: Output[];     // one per analog output of the board

    /**
     * A meter per output of 'dac', none driven yet.
     */
    constructor(props: BoxProps, dac: DacInfo) {
        super({ ...props, cls: "dac-outputs" });

        // each output: its meter, named by its pin
        const fullScale = dac.fullScale ?? DEFAULT_FULL_SCALE;
        this._outputs = dac.pins.map(pin => {
            const meter = new Meter({}, fullScale, `GPIO${pin}`);
            return { pin, meter };
        });

        this.setContent(this._outputs.map(output => output.meter));
        this.setPins([]);
    }

    /**
     * Shows the outputs as the pins are now.
     */
    setPins(pins: PinState[]) {
        for (const output of this._outputs) {
            // its voltage; dimmed with none when not driven
            const state = pins[output.pin];
            const driven = isDriven(state);
            output.meter.setVoltage(driven ? state.voltage : null);
            output.meter.setClass("idle", !driven);
        }
    }
}
