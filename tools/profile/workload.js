/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A heavier JS workload, written the way a program would be (classes,
// let / const, closures, Map, strings, arrays of objects), to measure the
// engine on more than micro-loops. Each part is timed (best of ROUNDS) and
// prints a checksum: the checksums must not change when the engine changes.
//   qjsx run tools/profile/workload.js [scale]
// 'scale' multiplies the work (default 20: about a second on a PC; about 1
// on a board).
const SCALE = Number(process.argv[2] ?? 20);
const ROUNDS = 3;

// Optional hooks around each part, set by a wrapper before this module runs
// (tools/profile/workload-profiled.js starts and stops the sampler there).
const hooks = globalThis.workloadHooks;

/**
 * Runs 'work' ROUNDS times, prints its best time and its checksum.
 */
function measure(name, work) {
    let best = Infinity;
    let checksum = 0;
    hooks?.begin(name);
    for (let round = 0; round < ROUNDS; round++) {
        const begin = Date.now();
        checksum = work();
        best = Math.min(best, Date.now() - begin);
    }

    hooks?.end(name);

    console.log(`${name.padEnd(26)} ${String(best).padStart(6)} ms   checksum ${checksum}`);
    return best;
}

// ### 1. Expressions: tokenizer, parser, evaluator ############################

class Lexer {
    constructor(text) {
        this.text = text;
        this.position = 0;
    }

    next() {
        const text = this.text;
        while (this.position < text.length && text[this.position] === " ") {
            this.position++;
        }

        if (this.position >= text.length) {
            return { kind: "end" };
        }

        const start = this.position;
        const char = text[start];
        if (char >= "0" && char <= "9") {
            while (this.position < text.length && text[this.position] >= "0" && text[this.position] <= "9") {
                this.position++;
            }

            return { kind: "number", value: Number(text.slice(start, this.position)) };
        }

        if (char >= "a" && char <= "z") {
            while (this.position < text.length && text[this.position] >= "a" && text[this.position] <= "z") {
                this.position++;
            }

            return { kind: "name", value: text.slice(start, this.position) };
        }

        this.position++;
        return { kind: "operator", value: char };
    }
}

class Parser {
    constructor(text) {
        this.lexer = new Lexer(text);
        this.token = this.lexer.next();
    }

    advance() {
        this.token = this.lexer.next();
    }

    // expression := term (("+" | "-") term)*
    expression() {
        let node = this.term();
        while (this.token.kind === "operator" && (this.token.value === "+" || this.token.value === "-")) {
            const operator = this.token.value;
            this.advance();
            node = { operator, left: node, right: this.term() };
        }

        return node;
    }

    // term := factor (("*" | "/" | "%") factor)*
    term() {
        let node = this.factor();
        while (this.token.kind === "operator" && "*/%".includes(this.token.value)) {
            const operator = this.token.value;
            this.advance();
            node = { operator, left: node, right: this.factor() };
        }

        return node;
    }

    // factor := number | name | "(" expression ")"
    factor() {
        const token = this.token;
        this.advance();
        if (token.kind === "number") {
            return { value: token.value };
        }

        if (token.kind === "name") {
            return { name: token.value };
        }

        const inner = this.expression();
        this.advance();   // ")"
        return inner;
    }
}

const OPERATIONS = {
    "+": (a, b) => a + b,
    "-": (a, b) => a - b,
    "*": (a, b) => a * b,
    "/": (a, b) => (b === 0 ? 0 : Math.trunc(a / b)),
    "%": (a, b) => (b === 0 ? 0 : a % b),
};

function evaluate(node, variables) {
    if (node.value !== undefined) {
        return node.value;
    }

    if (node.name !== undefined) {
        return variables.get(node.name) ?? 0;
    }

    return OPERATIONS[node.operator](evaluate(node.left, variables), evaluate(node.right, variables));
}

function expressions() {
    const formulas = [
        "(a + b) * (c - d) % 97 + e * 3",
        "a * a + b * b - (c + d) * (e % 7 + 1)",
        "((a + 1) * (b + 2) * (c + 3)) / (d + 4) - e",
    ];
    const trees = formulas.map(formula => new Parser(formula).expression());
    const variables = new Map();
    let checksum = 0;

    for (let i = 0; i < 4000 * SCALE; i++) {
        variables.set("a", i % 13);
        variables.set("b", i % 17);
        variables.set("c", i % 19);
        variables.set("d", i % 23);
        variables.set("e", i % 29);
        for (const tree of trees) {
            checksum = (checksum + evaluate(tree, variables)) | 0;
        }

        // parse again now and then: the tokenizer and parser work too
        if (i % 50 === 0) {
            checksum = (checksum + evaluate(new Parser(formulas[i % 3]).expression(), variables)) | 0;
        }
    }

    return checksum;
}

// ### 2. Sorting and a shortest path on a grid ################################

class PriorityQueue {
    constructor() {
        this.items = [];
    }

    push(item, priority) {
        const items = this.items;
        items.push({ item, priority });
        let index = items.length - 1;
        while (index > 0) {
            const parent = (index - 1) >> 1;
            if (items[parent].priority <= items[index].priority) {
                break;
            }

            [items[parent], items[index]] = [items[index], items[parent]];
            index = parent;
        }
    }

    pop() {
        const items = this.items;
        const top = items[0];
        const last = items.pop();
        if (items.length > 0) {
            items[0] = last;
            let index = 0;
            for (;;) {
                const left = index * 2 + 1;
                const right = left + 1;
                let smallest = index;
                if (left < items.length && items[left].priority < items[smallest].priority) {
                    smallest = left;
                }

                if (right < items.length && items[right].priority < items[smallest].priority) {
                    smallest = right;
                }

                if (smallest === index) {
                    break;
                }

                [items[smallest], items[index]] = [items[index], items[smallest]];
                index = smallest;
            }
        }

        return top.item;
    }

    get size() {
        return this.items.length;
    }
}

function shortestPath(size, seed) {
    // a grid of costs from a small pseudo-random generator
    let state = seed;
    const cost = new Uint8Array(size * size);
    for (let i = 0; i < cost.length; i++) {
        state = (state * 1103515245 + 12345) & 0x7fffffff;
        cost[i] = 1 + (state % 9);
    }

    // Dijkstra from the top left corner to the bottom right one
    const distance = new Array(size * size).fill(Infinity);
    const queue = new PriorityQueue();
    distance[0] = 0;
    queue.push(0, 0);
    while (queue.size > 0) {
        const cell = queue.pop();
        const x = cell % size;
        const y = (cell - x) / size;
        const neighbours = [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]];
        for (const [nx, ny] of neighbours) {
            if (nx < 0 || ny < 0 || nx >= size || ny >= size) {
                continue;
            }

            const next = ny * size + nx;
            const candidate = distance[cell] + cost[next];
            if (candidate < distance[next]) {
                distance[next] = candidate;
                queue.push(next, candidate);
            }
        }
    }

    return distance[size * size - 1];
}

function sortingAndPaths() {
    // records sorted by several keys
    const people = [];
    for (let i = 0; i < 3000 * SCALE; i++) {
        people.push({ name: `person${(i * 7919) % 3001}`, age: (i * 31) % 90, score: (i * 1237) % 1000 });
    }

    people.sort((a, b) => a.age - b.age || b.score - a.score || (a.name < b.name ? -1 : 1));
    let checksum = people[0].score + people[people.length - 1].age;

    // shortest paths
    for (let seed = 1; seed <= 3; seed++) {
        checksum += shortestPath(Math.round(30 * Math.sqrt(SCALE)), seed);
    }

    return checksum;
}

// ### 3. JSON round trips #####################################################

function jsonRoundTrips() {
    // a medium structure: a config-like tree with arrays and strings
    const document = { version: 3, devices: [] };
    for (let i = 0; i < 60; i++) {
        document.devices.push({
            id: `device-${i}`,
            enabled: i % 3 !== 0,
            pins: [i % 48, (i * 7) % 48, (i * 13) % 48],
            settings: { rate: 9600 * (1 + (i % 4)), label: `sensor number ${i}`, threshold: i / 7 },
        });
    }

    let checksum = 0;
    for (let i = 0; i < 40 * SCALE; i++) {
        const text = JSON.stringify(document);
        const copy = JSON.parse(text);
        copy.version = i;
        checksum = (checksum + text.length + copy.devices[i % 60].pins[1]) | 0;
    }

    return checksum;
}

// ### 4. A small ray tracer (floating point) ##################################

class Vector {
    constructor(x, y, z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    add(other) {
        return new Vector(this.x + other.x, this.y + other.y, this.z + other.z);
    }

    subtract(other) {
        return new Vector(this.x - other.x, this.y - other.y, this.z - other.z);
    }

    scale(factor) {
        return new Vector(this.x * factor, this.y * factor, this.z * factor);
    }

    dot(other) {
        return this.x * other.x + this.y * other.y + this.z * other.z;
    }

    normalized() {
        return this.scale(1 / Math.sqrt(this.dot(this)));
    }
}

function rayTracer() {
    const spheres = [
        { center: new Vector(0, 0, 5), radius: 1.5, shade: 0.9 },
        { center: new Vector(2, 1, 7), radius: 1, shade: 0.6 },
        { center: new Vector(-2, -1, 6), radius: 1.2, shade: 0.4 },
    ];
    const light = new Vector(1, 1, -1).normalized();
    const width = Math.round(64 * Math.sqrt(SCALE));
    const height = width;
    let checksum = 0;

    for (let row = 0; row < height; row++) {
        for (let column = 0; column < width; column++) {
            // the ray of this pixel
            const direction = new Vector((column - width / 2) / width, (row - height / 2) / height, 1).normalized();
            let nearest = Infinity;
            let hit = null;
            for (const sphere of spheres) {
                const offset = sphere.center.scale(-1);
                const b = offset.dot(direction);
                const c = offset.dot(offset) - sphere.radius * sphere.radius;
                const discriminant = b * b - c;
                if (discriminant >= 0) {
                    const distance = -b - Math.sqrt(discriminant);
                    if (distance > 0 && distance < nearest) {
                        nearest = distance;
                        hit = sphere;
                    }
                }
            }

            // its brightness: diffuse light on the nearest sphere
            if (hit !== null) {
                const point = direction.scale(nearest);
                const normal = point.subtract(hit.center).normalized();
                const brightness = Math.max(0, normal.dot(light)) * hit.shade;
                checksum += Math.round(brightness * 255);
            }
        }
    }

    return checksum;
}

// ### 5. Strings and bytes ####################################################

const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

function encode(bytes) {
    let text = "";
    for (let i = 0; i < bytes.length; i += 3) {
        const chunk = (bytes[i] << 16) | ((bytes[i + 1] ?? 0) << 8) | (bytes[i + 2] ?? 0);
        text += ALPHABET[(chunk >> 18) & 63] + ALPHABET[(chunk >> 12) & 63]
            + ALPHABET[(chunk >> 6) & 63] + ALPHABET[chunk & 63];
    }

    return text;
}

function decode(text) {
    const bytes = new Uint8Array((text.length / 4) * 3);
    let out = 0;
    for (let i = 0; i < text.length; i += 4) {
        const chunk = (ALPHABET.indexOf(text[i]) << 18) | (ALPHABET.indexOf(text[i + 1]) << 12)
            | (ALPHABET.indexOf(text[i + 2]) << 6) | ALPHABET.indexOf(text[i + 3]);
        bytes[out++] = (chunk >> 16) & 255;
        bytes[out++] = (chunk >> 8) & 255;
        bytes[out++] = chunk & 255;
    }

    return bytes;
}

function stringsAndBytes() {
    const bytes = new Uint8Array(3 * 400);
    for (let i = 0; i < bytes.length; i++) {
        bytes[i] = (i * 73 + 41) & 255;
    }

    let checksum = 0;
    for (let i = 0; i < 20 * SCALE; i++) {
        const text = encode(bytes);
        const back = decode(text);
        const words = text.split("A").map(word => word.toLowerCase()).filter(word => word.length > 2);
        checksum = (checksum + back[i % back.length] + words.length + text.charCodeAt(i % text.length)) | 0;
    }

    return checksum;
}

// ### All #####################################################################

let total = 0;
total += measure("expressions", expressions);
total += measure("sorting and paths", sortingAndPaths);
total += measure("JSON round trips", jsonRoundTrips);
total += measure("ray tracer", rayTracer);
total += measure("strings and bytes", stringsAndBytes);
console.log(`${"total".padEnd(26)} ${String(total).padStart(6)} ms`);
