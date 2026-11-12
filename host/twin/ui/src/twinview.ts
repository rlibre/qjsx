/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The main view of the page: the header, then a DockingView with the board
// in the middle (its parts and devices in folding sections), the table of
// the pins on the right, the console of the parts and the analyzer of the
// pins at the bottom. It
// follows the messages of the twin (messages.ts), keeps the settings (kept
// by the browser, like the layout of the panels), and offers the panels in a
// context menu (right click).
import { BoxProps, DockingView, EvMessage, Menu, StateProxy, VBox } from "x4js";
import { AppHeader } from "./appheader";
import { BoardView } from "./boardview";
import { ConsoleView } from "./consoleview";
import { BoardInfo, I2cData, LogLine, MSG, PartInfo, PartStateMessage, ScopeData, TwinState, UartData } from "./messages";
import { PinTable } from "./pintable";
import { ScopeView } from "./scopeview";
import { applyTheme, DEFAULT_SETTINGS, openSettings, TwinSettings } from "./settings";
import { tx } from "./texts";

// Names under which the browser keeps the settings and the layout.
const SETTINGS_KEY = "qjsx.twin.settings";
const LAYOUT_KEY = "qjsx.twin.layout";

// The panels of the docking view, in the order of the context menu.
const PANEL_IDS = ["board", "pins", "console", "scope"];

export class TwinView extends VBox {
    declare protected refs: { header: AppHeader; board: BoardView; pins: PinTable; console: ConsoleView; scope: ScopeView; dock: DockingView };

    private _settings: StateProxy<TwinSettings>;   // kept by the browser

    /**
     * Builds the view; the board comes with the first message of the twin.
     */
    constructor(props: BoxProps) {
        super(props);

        // the settings, as kept by the browser, and the theme they choose
        this._settings = this.loadPState(SETTINGS_KEY, DEFAULT_SETTINGS);
        applyTheme(this._settings.theme);
        this._settings.watch("theme", () => applyTheme(this._settings.theme));

        // the panels of the docking view
        const board = this.refs.board = new BoardView({ cls: "board" });
        const pins = this.refs.pins = new PinTable({ cls: "pins" }, this._settings);
        const console = this.refs.console = new ConsoleView({ cls: "console" });
        const scope = this.refs.scope = new ScopeView({ cls: "scope" });

        this.setContent([
            this.refs.header = new AppHeader({ settings: () => openSettings(this._settings) }),
            this.refs.dock = new DockingView({
                flex: true,
                persist: LAYOUT_KEY,
                panels: [
                    { id: "board", title: tx.twin.board, kind: "content", content: board },
                    { id: "pins", title: tx.twin.pins, content: pins },
                    { id: "console", title: tx.twin.console, content: console, sides: ["bottom"] },
                    { id: "scope", title: tx.twin.scope, content: scope, sides: ["bottom"] },
                ],
                layout: {
                    left: { size: 0, root: null },
                    right: { size: 460, root: { type: "stack", size: 0, panels: ["pins"], active: "pins" } },
                    bottom: { size: 200, root: { type: "stack", size: 0, panels: ["console", "scope"], active: "scope" } },
                    content: { type: "stack", size: 0, panels: ["board"], active: "board" },
                    floats: [],
                    hidden: [],
                },
            }),
        ]);

        // the messages of the twin, the context menu of the panels
        this.onGlobalEvent((event: EvMessage) => this._onMessage(event));
        this.addDOMEvent("contextmenu", (event: MouseEvent) => this._showPanelMenu(event));
    }

    /**
     * The twin went away (agent stopped): says so until the page reconnects.
     */
    showDisconnected() {
        this.refs.header.showDisconnected();
    }

    /**
     * One message of the twin: the board, the state now, the traffic of a
     * UART, the parts, what a part published, a line of a part, what the
     * pins did (the analyzer).
     */
    private _onMessage(event: EvMessage) {
        if (event.msg === MSG.board) {
            const board = event.params as BoardInfo;
            this.refs.header.setBoard(board.name, board.chip);
            this.refs.board.setBoard(board);
            this.refs.pins.setBoard(board);
            this.refs.scope.setBoard(board);
        } else if (event.msg === MSG.state) {
            const state = event.params as TwinState;
            this.refs.board.setUsb(state.usb);
            this.refs.board.setUarts(state.uarts);
            this.refs.board.setI2c(state.i2c);
            this.refs.pins.setPins(state.pins);
            this.refs.board.setPins(state.pins);
            this.refs.scope.setPins(state.pins);
        } else if (event.msg === MSG.uartData) {
            const data = event.params as UartData;
            this.refs.board.addUartRecords(data.uart, data.records);
        } else if (event.msg === MSG.parts) {
            const parts = event.params as PartInfo[];
            this.refs.board.setParts(parts);
            this.refs.pins.setParts(parts);
            this.refs.scope.setParts(parts);
        } else if (event.msg === MSG.partState) {
            const message = event.params as PartStateMessage;
            this.refs.board.setPartState(message.id, message.state);
        } else if (event.msg === MSG.log) {
            this.refs.console.addLine(event.params as LogLine);
        } else if (event.msg === MSG.i2cData) {
            this.refs.board.addI2cRecords((event.params as I2cData).records);
        } else if (event.msg === MSG.scopeData) {
            this.refs.scope.addData(event.params as ScopeData);
        }
    }

    /**
     * The context menu of the panels, at the pointer: each panel shown or
     * hidden, then back to the first layout.
     */
    private _showPanelMenu(event: MouseEvent) {
        event.preventDefault();
        const dock = this.refs.dock;

        // a line per panel, checked when shown
        const titles: Record<string, string> = { board: tx.twin.board, pins: tx.twin.pins, console: tx.twin.console, scope: tx.twin.scope };
        const panelItems = PANEL_IDS.map(id => ({
            text: titles[id],
            checked: dock.isPanelVisible(id),
            click: () => dock.showPanel(id, !dock.isPanelVisible(id)),
        }));

        const menu = new Menu({
            items: [
                ...panelItems,
                "-",
                { text: tx.twin.reset_layout, click: () => dock.resetLayout() },
            ],
        });
        menu.displayAt(event.clientX, event.clientY);
    }
}
