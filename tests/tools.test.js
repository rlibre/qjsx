// Tests of the JS modules of the tool: zip, glob, project.
// Run with: bin/Release/qjsx run tests/tools.test.js
import * as fs from "fs";
import { join } from "stdlib/path";
import { writeZip, readZip, crc32 } from "qjsx:zip";
import { globToRegExp, matchFiles } from "qjsx:glob";
import { loadProject } from "qjsx:project";
import { compileModule, normalizeImport, warningText, BYTECODE_VERSION } from "qjsx:compile";
import { libraryOf } from "qjsx:library";
import { check, equal, throws, report } from "./assert.js";

const encode = text => new TextEncoder().encode(text);
const decode = bytes => new TextDecoder().decode(bytes);

// --- zip
equal(crc32(encode("123456789")), 0xcbf43926, "crc32 check value");
const archive = writeZip([
    { name: "a.txt", data: encode("alpha") },
    { name: "dossier/é.bin", data: new Uint8Array([0, 1, 2, 255]) },
    { name: "empty", data: new Uint8Array(0) },
]);
const entries = readZip(archive);
equal(entries.size, 3, "entry count");
equal(decode(entries.get("a.txt")), "alpha", "text entry");
equal(Array.from(entries.get("dossier/é.bin")).join(","), "0,1,2,255", "binary entry, UTF-8 name");
equal(entries.get("empty").length, 0, "empty entry");
equal(writeZip([{ name: "x", data: encode("y") }]).join(","), writeZip([{ name: "x", data: encode("y") }]).join(","),
      "same content gives the same archive");
throws(() => readZip(encode("not a zip at all, definitely not")), /not a zip archive/, "bad archive");

// --- glob
check(globToRegExp("src/**/*.js").test("src/main.js"), "** matches no directory");
check(globToRegExp("src/**/*.js").test("src/a/b/c.js"), "** matches several directories");
check(!globToRegExp("src/*.js").test("src/a/c.js"), "* does not cross /");
check(!globToRegExp("src/**/*.js").test("srcx/main.js"), "literal prefix");
check(globToRegExp("a?.js").test("ab.js") && !globToRegExp("a?.js").test("a/.js"), "?");
check(globToRegExp("file.(1).js").test("file.(1).js"), "special characters are literal");
const demo = "tests/fixtures/demo";
equal(matchFiles(demo, ["src/**/*.js"], ["src/**/*.test.js"]).join(","),
      "src/lib/greet.js,src/lib/math/add.js,src/main.js", "matchFiles with exclude");

// --- project
const project = loadProject(demo);
equal(project.name, "demo", "project name");
equal(project.output, "build/packages", "default output");
equal(project.keep, 3, "keep");
const directory = join("build", "test-project");
fs.rmSync(directory, { recursive: true, force: true });
fs.mkdirSync(directory, { recursive: true });
throws(() => loadProject(directory), /no qjsx\.json/, "missing project file");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "mian": "x.js" }');
throws(() => loadProject(directory), /unknown setting 'mian'/, "typo in a setting");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "keep": "3" }');
throws(() => loadProject(directory), /'keep' must be a number/, "wrong type");
fs.writeFileSync(join(directory, "qjsx.json"), "{ broken");
throws(() => loadProject(directory), /qjsx\.json/, "invalid JSON");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "env": ["A"] }');
throws(() => loadProject(directory), /'env' must be a object/, "env is not an object");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "env": null }');
throws(() => loadProject(directory), /'env' must be a object/, "env is null");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "env": { "PORT": 80 } }');
throws(() => loadProject(directory), /'env\.PORT' must be a string/, "env value is not a string");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "env": { "PORT": "80" } }');
equal(loadProject(directory).env.PORT, "80", "env of strings");
fs.writeFileSync(join(directory, "qjsx.json"), "{}");
equal(loadProject(directory).name, "test-project", "default name is the directory name");
equal(loadProject(directory).libraries.length, 0, "no library by default");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "libraries": [3] }');
throws(() => loadProject(directory), /'libraries' must be an array of paths/, "library that is not a path");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "modules": { "../x": "src/x.js" } }');
throws(() => loadProject(directory), /module name '\.\.\/x'/, "module name that leaves the library");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "modules": { "x": 3 } }');
throws(() => loadProject(directory), /'modules\.x' must be the path/, "module without a source file");
fs.writeFileSync(join(directory, "qjsx.json"), '{ "version": "1.0.0", "modules": { "drivers/x": "src/x.js" } }');
equal(loadProject(directory).modules["drivers/x"], "src/x.js", "modules of a library project");
fs.rmSync(directory, { recursive: true });

// --- imports of a compiled module
equal(normalizeImport("src/main.js", "./lib/a.js"), "src/lib/a.js", "relative import");
equal(normalizeImport("src/lib/a.js", "../b.js"), "src/b.js", "import from the parent folder");
equal(normalizeImport("main.js", "./a.js"), "a.js", "relative import at the root");
equal(normalizeImport("main.js", "../a.js"), "../a.js", "nothing above the root");
equal(normalizeImport("sensors/bme280", "./filter"), "sensors/filter", "neighbour module of a library");
equal(normalizeImport("src/main.js", "stdlib/path"), "stdlib/path", "a name is taken as it is");
equal(libraryOf("stdlib/path"), "stdlib", "library of a module name");
equal(libraryOf("fs"), undefined, "a name without '/' belongs to no library");

const importing = 'import { a } from "./a.js";\nexport { b } from "stdlib/path";\nimport "fs";\nimport { c } from "./a.js";\n';
const compiledImports = compileModule("src/main.js", importing).imports;
equal(compiledImports.map(imported => imported.name).join(","), "src/a.js,stdlib/path,fs", "imports of a module, each one once");
equal(compiledImports[0].specifier, "./a.js", "import as written in the source");
const dynamic = throws(() => compileModule("src/main.js", 'const x = 1;\nconst m = await import("./a.js");\n'),
    /SyntaxError: 'import\(\)' is not allowed/, "import() is refused");
check(dynamic.stack.includes("src/main.js:2"), "import() is reported at its line");

// --- what a module asks of the compiler at run time (not possible on a device)
check(Number.isInteger(BYTECODE_VERSION) && BYTECODE_VERSION > 0, "the bytecode version is a number");
const usesCompiler = [
    "const a = 1;",
    "const b = eval('a + 1');",
    "const f = new Function('x', 'return x');",
    "const g = Function('return 1');",
    "const h = new Function;",
    "const o = { eval(x) { return x; } };",
    "o.eval(1);",
    "globalThis.Function.name;",
].join("\n");
const found = compileModule("src/main.js", usesCompiler).warnings;
equal(found.map(warning => `${warning.kind}@${warning.line}`).join(","), "eval@2,Function@3,Function@4,Function@5",
    "eval() and the Function constructor are reported, not a method or a property of these names");
equal(found[0].column, 15, "column of the warning: the call of eval");
check(warningText(found[0]).startsWith("eval() needs the compiler"), "text of the eval warning");
check(warningText(found[1]).startsWith("the Function constructor needs the compiler"), "text of the Function warning");
equal(compileModule("src/main.js", "export const x = 1;\n").warnings.length, 0, "no warning for an ordinary module");

report("tools");
