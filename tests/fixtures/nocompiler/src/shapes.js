/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Imported module: classes, getters, static members, private fields.
export class Shape {
    static count = 0;
    #name;
    constructor(name) {
        this.#name = name;
        Shape.count++;
    }
    get name() {
        return this.#name;
    }
    area() {
        return 0;
    }
}

export class Rectangle extends Shape {
    constructor(width, height) {
        super("rectangle");
        this.width = width;
        this.height = height;
    }
    area() {
        return this.width * this.height;
    }
}
