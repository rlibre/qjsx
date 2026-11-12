/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The settings of the page: what they are, the page that edits them (a
// SettingsView in a Dialog), and the theme they choose. They live in a State
// kept by the browser (TwinView.loadPState); the components watch it.
import { Application, BoxProps, Checkbox, Dialog, SettingItem, SettingsView, Select, StateProxy } from "x4js";
import { tx } from "./texts";

export type ThemeChoice = "system" | "light" | "dark";

// A type, not an interface: a State needs an implicit index signature.
export type TwinSettings = {
    theme:        ThemeChoice;   // the theme of the page
    usedPinsOnly: boolean;       // the pins table hides the pins nothing uses
};

export const DEFAULT_SETTINGS: TwinSettings = {
    theme: "system",
    usedPinsOnly: true,
};

/**
 * Applies a theme choice: "system" follows the system (now; a change of the
 * system theme is seen at the next load).
 */
export function applyTheme(theme: ThemeChoice) {
    const systemDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
    const dark = theme === "dark" || (theme === "system" && systemDark);
    Application.setTheme(dark ? "dark" : "");
}

class SettingsPage extends SettingsView {
    declare protected refs: { theme: Select; usedPinsOnly: Checkbox };

    /**
     * The sections of the settings; each control writes its setting at once.
     */
    constructor(props: BoxProps, settings: StateProxy<TwinSettings>) {
        super(props);

        // the items of the theme choice
        const themes = [
            { id: "system", text: tx.twin.theme_system },
            { id: "light", text: tx.twin.theme_light },
            { id: "dark", text: tx.twin.theme_dark },
        ];

        this.setSections([
            { id: "display", title: tx.twin.display, content: [
                new SettingItem({ title: tx.twin.theme, description: tx.twin.theme_description, content: [
                    this.refs.theme = new Select({
                        items: themes,
                        value: settings.theme,
                        change: () => {
                            settings.theme = this.refs.theme.getValue() as ThemeChoice;
                        },
                    }),
                ] }),
            ] },
            { id: "pins", title: tx.twin.pins, content: [
                new SettingItem({ title: tx.twin.used_pins_only, description: tx.twin.used_pins_only_description, content: [
                    this.refs.usedPinsOnly = new Checkbox({
                        checked: settings.usedPinsOnly,
                        change: () => {
                            settings.usedPinsOnly = this.refs.usedPinsOnly.getCheck();
                        },
                    }),
                ] }),
            ] },
        ]);
    }
}

/**
 * Opens the settings page in a dialog; the changes apply at once.
 */
export async function openSettings(settings: StateProxy<TwinSettings>) {
    const page = new SettingsPage({ cls: "settings-page" }, settings);
    const dialog = new Dialog({
        title: tx.twin.settings,
        buttons: ["ok.default"],
        closable: true,
        sizable: true,
        content: page,
    });
    await dialog.showAsync();
}
