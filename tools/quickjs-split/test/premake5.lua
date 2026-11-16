--          _
--  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
-- / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
-- \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
--    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

-- Test build: qjs linked with the original QuickJS (engine/qjs-bellard) and with the
-- split one (engine/qjsx), to compare behaviour and speed.
--
-- premake5 gmake --cc=gcc --shell=posix --upstream=<release dir> --builddir=<build dir> [--lto]
--
-- <release dir> is an extracted QuickJS release of the same version
-- (it provides qjs.c, qjsc.c, quickjs-libc.c, repl.js and tests/).

newoption {
    trigger = "upstream",
    value = "DIR",
    description = "Extracted QuickJS release (qjs.c, qjsc.c, quickjs-libc.c, repl.js, tests/)",
}

newoption {
    trigger = "builddir",
    value = "DIR",
    description = "Where the makefiles, objects and executables go (default: ./build)",
}

newoption {
    trigger = "lto",
    description = "Link time optimization (-flto) for both engines and both qjs",
}

local function absolute(dir)
    if path.isabsolute(dir) then
        return dir
    end
    return path.join(_WORKING_DIR, dir)
end

if not _OPTIONS["upstream"] then
    error("missing --upstream=DIR (extracted QuickJS release)")
end

local UPSTREAM = absolute(_OPTIONS["upstream"])
local PROJECT = path.getabsolute(path.join(_SCRIPT_DIR, "../../.."))
local ORIGINAL = PROJECT .. "/engine/qjs-bellard"
local SPLIT = PROJECT .. "/engine/qjsx"
local BUILD = absolute(_OPTIONS["builddir"] or "build")
local REPL_C = BUILD .. "/generated/repl.c"
local VERSION = io.readfile(ORIGINAL .. "/VERSION"):gsub("%s+", "")

-- The programs of the release (qjs.c, quickjs-libc.c, run-test262.c...) are
-- compiled from a copy: next to their own quickjs.h they would include it
-- instead of the header of the engine they are linked with, and its inline
-- functions (JS_FreeValue...) are not the same in engine/qjsx.
local SOURCES = BUILD .. "/upstream-src"
os.mkdir(SOURCES)
for _, name in ipairs({ "qjs.c", "qjsc.c", "quickjs-libc.c", "quickjs-libc.h", "run-test262.c" }) do
    os.copyfile(UPSTREAM .. "/" .. name, SOURCES .. "/" .. name)
end

workspace "quickjs-split-test"
    configurations { "Release" }
    location(BUILD)
    language "C"
    cdialect "gnu11"
    optimize "Speed"
    symbols "On"
    defines { "_GNU_SOURCE", "__USE_MINGW_ANSI_STDIO", 'CONFIG_VERSION="' .. VERSION .. '"' }
    buildoptions { "-fwrapv", "-Wall", "-Wno-array-bounds", "-Wno-format-truncation",
                   "-Wno-infinite-recursion", "-Werror=implicit-function-declaration" }
    targetdir(BUILD .. "/bin")

    -- link time optimization: the whole engine seen at once at link time, as
    -- in a single file (the static libraries keep regular code too: premake
    -- archives them with ar, which does not read the LTO part)
    if _OPTIONS["lto"] then
        buildoptions { "-flto", "-ffat-lto-objects" }
        linkoptions { "-flto" }
    end

local function engine(name, root, sources)
    project(name)
        kind "StaticLib"
        targetdir(BUILD .. "/lib")
        objdir(BUILD .. "/obj/" .. name)
        includedirs { root }
        files(sources)
end

engine("engine-original", ORIGINAL, {
    ORIGINAL .. "/quickjs.c", ORIGINAL .. "/dtoa.c", ORIGINAL .. "/libregexp.c",
    ORIGINAL .. "/libunicode.c", ORIGINAL .. "/cutils.c",
})

engine("engine-split", SPLIT, {
    SPLIT .. "/core/*.c", SPLIT .. "/bytecode/*.c", SPLIT .. "/builtins/*.c", SPLIT .. "/compiler/*.c",
    SPLIT .. "/dtoa.c", SPLIT .. "/libregexp.c", SPLIT .. "/libunicode.c", SPLIT .. "/cutils.c",
})

-- qjsc compiles repl.js into repl.c, needed by qjs
project "qjsc"
    kind "ConsoleApp"
    objdir(BUILD .. "/obj/qjsc")
    includedirs { ORIGINAL }
    files { SOURCES .. "/qjsc.c", SOURCES .. "/quickjs-libc.c" }
    links { "engine-original", "m", "pthread" }
    postbuildcommands {
        "mkdir -p " .. BUILD .. "/generated",
        "cd " .. UPSTREAM .. " && " .. BUILD .. "/bin/qjsc -s -c -o " .. REPL_C .. " -m repl.js",
    }

local function qjs(name, engineName, root)
    project(name)
        kind "ConsoleApp"
        objdir(BUILD .. "/obj/" .. name)
        dependson { "qjsc" }
        includedirs { root }
        files { SOURCES .. "/qjs.c", SOURCES .. "/quickjs-libc.c", REPL_C }
        links { engineName, "m", "pthread" }
end

qjs("qjs-original", "engine-original", ORIGINAL)
qjs("qjs-split", "engine-split", SPLIT)

-- run-test262, Bellard's runner of the ECMAScript conformance suite (test262,
-- cloned in <release dir>/test262 at the commit of its Makefile), with each
-- engine: from <release dir>, "run-test262-split -m -c test262.conf"
local function runTest262(name, engineName, root)
    project(name)
        kind "ConsoleApp"
        objdir(BUILD .. "/obj/" .. name)
        includedirs { root }
        files { SOURCES .. "/run-test262.c", SOURCES .. "/quickjs-libc.c" }
        links { engineName, "m", "pthread" }
end

runTest262("run-test262-original", "engine-original", ORIGINAL)
runTest262("run-test262-split", "engine-split", SPLIT)
