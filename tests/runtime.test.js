// Tests of the runtime library: globals, fs, path, crypto, timers.
// Run with: bin/Release/qjsx run tests/runtime.test.js
import * as fs from "fs";
import path from "stdlib/path";
import { join, dirname, basename, extname, relative, normalize, isAbsolute, resolve } from "stdlib/path";
import { sha256 } from "crypto";
import { check, equal, throws, report } from "./assert.js";

// --- process
check(Array.isArray(process.argv) && process.argv.length >= 2, "process.argv");
check(process.argv[1].endsWith("runtime.test.js"), "process.argv[1] is the script");
check(typeof process.env === "object", "process.env");
check(["win32", "linux", "darwin"].includes(process.platform), "process.platform");
check(typeof process.cwd() === "string" && process.cwd().length > 0, "process.cwd()");

// --- TextEncoder / TextDecoder
const encoded = new TextEncoder().encode("é€");
check(encoded instanceof Uint8Array, "encode returns a Uint8Array");
equal(encoded.length, 5, "UTF-8 length");
equal(new TextDecoder().decode(encoded), "é€", "decode");
equal(new TextDecoder("UTF-8").encoding, "utf-8", "encoding name");
throws(() => new TextDecoder("latin1"), /RangeError/, "unsupported encoding");

// --- path
equal(join("a", "b", "../c", "d.txt"), "a/c/d.txt", "join");
equal(join(), ".", "join()");
equal(dirname("/a/b/c.js"), "/a/b", "dirname");
equal(dirname("c.js"), ".", "dirname of a file name");
equal(basename("/a/b/c.js"), "c.js", "basename");
equal(basename("/a/b/c.js", ".js"), "c", "basename with extension");
equal(extname("c.test.js"), ".js", "extname");
equal(extname(".profile"), "", "extname of a dot file");
equal(normalize("a//b/./c/.."), "a/b", "normalize");
equal(normalize("../a"), "../a", "normalize keeps leading ..");
check(isAbsolute("/x"), "isAbsolute");
check(!isAbsolute("x/y"), "relative path");
equal(relative("/a/b/c", "/a/d"), "../../d", "relative");
check(isAbsolute(resolve("x")), "resolve gives an absolute path");
equal(path.join("x", "y"), "x/y", "default export");
if (process.platform === "win32") {
    equal(normalize("C:\\a\\b\\..\\c"), "C:/a/c", "Windows separators and drive");
    check(isAbsolute("C:/x"), "drive letter absolute");
}

// --- fs
const directory = join(process.cwd(), "build", "test-fs");
fs.rmSync(directory, { recursive: true, force: true });
fs.mkdirSync(join(directory, "sub", "deeper"), { recursive: true });
check(fs.existsSync(join(directory, "sub", "deeper")), "mkdirSync recursive");
const file = join(directory, "sub", "hello.txt");
fs.writeFileSync(file, "héllo");
equal(fs.readFileSync(file, "utf8"), "héllo", "text round trip");
equal(fs.readFileSync(file, { encoding: "utf8" }), "héllo", "options object");
const raw = fs.readFileSync(file);
check(raw instanceof Uint8Array && raw.length === 6, "bytes read");
fs.writeFileSync(join(directory, "data.bin"), new Uint8Array([1, 2, 3]));
equal(fs.statSync(join(directory, "data.bin")).size, 3, "statSync size");
check(fs.statSync(directory).isDirectory(), "isDirectory");
check(fs.statSync(file).isFile(), "isFile");
const names = fs.readdirSync(directory).sort();
equal(names.join(","), "data.bin,sub", "readdirSync");
fs.renameSync(join(directory, "data.bin"), join(directory, "moved.bin"));
check(fs.existsSync(join(directory, "moved.bin")) && !fs.existsSync(join(directory, "data.bin")), "renameSync");
fs.unlinkSync(join(directory, "moved.bin"));
check(!fs.existsSync(join(directory, "moved.bin")), "unlinkSync");
const missing = throws(() => fs.readFileSync(join(directory, "nope.txt")), /ENOENT/, "missing file");
equal(missing && missing.code, "ENOENT", "error code");
throws(() => fs.rmSync(directory), /EISDIR/, "rmSync on a directory without recursive");
fs.rmSync(directory, { recursive: true });
check(!fs.existsSync(directory), "rmSync recursive");

// --- crypto
const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, "0")).join("");
equal(hex(sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256('')");
equal(hex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256('abc')");
const long = "a".repeat(1000);
equal(hex(sha256(long)), hex(sha256(new TextEncoder().encode(long))), "string and bytes give the same hash");

// --- timers and promises
const order = [];
setTimeout(() => order.push("timeout 20"), 20);
setTimeout(() => order.push("timeout 0"), 0);
Promise.resolve().then(() => order.push("microtask"));
const cancelled = setTimeout(() => order.push("cancelled"), 5);
clearTimeout(cancelled);
let ticks = 0;
const interval = setInterval(() => {
    ticks++;
    if (ticks === 3) {
        clearInterval(interval);
    }
}, 1);
setTimeout((a, b) => order.push(`args ${a} ${b}`), 30, 1, 2);
setTimeout(() => {
    equal(order.join(" | "), "microtask | timeout 0 | timeout 20 | args 1 2", "timer order");
    equal(ticks, 3, "setInterval cleared from its callback");
    report("runtime");
}, 60);
