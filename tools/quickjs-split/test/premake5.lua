--          _
--  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
-- / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
-- \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
--    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

-- Test build: qjs linked with the original QuickJS (engine/qjs-bellard) and with the
-- split one (engine/qjsx), to compare behaviour and speed.
--
-- premake5 gmake --cc=gcc --shell=posix --upstream=<release dir> --builddir=<build dir>
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
    includedirs { ORIGINAL, UPSTREAM }
    files { UPSTREAM .. "/qjsc.c", UPSTREAM .. "/quickjs-libc.c" }
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
        includedirs { root, UPSTREAM }
        files { UPSTREAM .. "/qjs.c", UPSTREAM .. "/quickjs-libc.c", REPL_C }
        links { engineName, "m", "pthread" }
end

qjs("qjs-original", "engine-original", ORIGINAL)
qjs("qjs-split", "engine-split", SPLIT)
