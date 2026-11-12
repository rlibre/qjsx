--          _
--  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
-- / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
-- \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
--    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

-- qjsx build (desktop).
--   premake5 gmake --cc=gcc --shell=posix
--   make -C build config=release        (or config=debug)
--
-- Projects:
--   qjsx-engine         the engine (engine/qjsx: QuickJS split and modified for qjsx), static library
--   qjsx-engine-target  the same without compiler (no eval), like on the targets
--   qjsx                the host tool
--   qjsx-tests          qjsx + the native test modules (tests/native)
--   qjsx-target         qjsx without the compiler module, linked with qjsx-engine-target
--
-- Folders: engine/ (QuickJS), common/ (portable code, also built for the
-- ESP: jsbind, runtime, agent), host/ (the desktop tool only).
-- Headers are included by their name alone: every folder below is in the
-- include path, and no two headers share a name.

local QJSX = "engine/qjsx"
local QUICKJS_VERSION = io.readfile(QJSX .. "/VERSION"):gsub("%s+", "")
local EMBED_TOOL = path.getabsolute("tools/embed-js.lua")
local EMBED_TWIN_UI_TOOL = path.getabsolute("tools/embed-twin-ui.lua")
local TWIN_UI = path.getabsolute("host/twin/ui")

-- What the embedded page of the twin depends on: its tool, its sources and
-- its build (dist/), but dist/sources.crc32, which "npm run build" removes
-- (x4js empties dist/). A file added there needs premake again.
local TWIN_UI_INPUTS = table.join(
    { EMBED_TWIN_UI_TOOL, TWIN_UI .. "/package.json", TWIN_UI .. "/tsconfig.json" },
    os.matchfiles(TWIN_UI .. "/src/**"),
    os.matchfiles(TWIN_UI .. "/dist/**"))
TWIN_UI_INPUTS = table.filter(TWIN_UI_INPUTS, function(file)
    return path.getname(file) ~= "sources.crc32"
end)

workspace "qjsx"
    configurations { "Debug", "Release" }
    location "build"
    targetdir "bin/%{cfg.buildcfg}"
    objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"
        optimize "Off"

    filter "configurations:Release"
        defines { "NDEBUG" }
        symbols "On"
        optimize "Speed"

    filter {}

-- The engine as a static library. 'withCompiler' false: no compiler/ (no
-- eval), like the targets (ESP32, RP2350), which get their code as bytecode.
local function engineLibrary(name, withCompiler)
    project(name)
        kind "StaticLib"
        language "C"
        cdialect "gnu11"
        includedirs { QJSX }
        defines { "_GNU_SOURCE", 'CONFIG_VERSION="' .. QUICKJS_VERSION .. '"' }
        buildoptions { "-fwrapv", "-Wall", "-Wno-array-bounds", "-Wno-format-truncation", "-Wno-infinite-recursion" }
        files {
            QJSX .. "/core/*.c",
            QJSX .. "/bytecode/*.c",
            QJSX .. "/builtins/*.c",
            QJSX .. "/cutils.c",
            QJSX .. "/dtoa.c",
            QJSX .. "/libregexp.c",
            QJSX .. "/libunicode.c",
        }
        if withCompiler then
            files { QJSX .. "/compiler/*.c" }
        else
            defines { "QJSX_NO_COMPILER" }
        end
        filter "system:windows"
            defines { "__USE_MINGW_ANSI_STDIO" }
        filter {}
end

engineLibrary("qjsx-engine", true)
engineLibrary("qjsx-engine-target", false)

-- 'engine': the engine library linked; 'excludedFiles': sources left out.
local function hostApplication(name, engine, extraFiles, excludedFiles, extraDefines)
    project(name)
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++17"
        includedirs {
            QJSX,
            "common/jsbind",
            "common/runtime",
            "common/runtime/modules",
            "common/agent",
            "common/board",
            "common/hal",
            "common/device",
            "host",
            "host/modules",
            "host/platform",
            "host/twin/sim",
        }
        defines { 'CONFIG_VERSION="' .. QUICKJS_VERSION .. '"' }
        defines(extraDefines)
        buildoptions { "-fno-exceptions", "-fno-rtti", "-Wall" }
        files {
            "common/jsbind/*.h",
            "common/runtime/*.h",
            "common/runtime/*.cpp",
            "common/runtime/modules/*.h",
            "common/runtime/modules/*.cpp",
            "common/agent/*.h",
            "common/agent/*.cpp",
            "common/board/*.h",
            "common/hal/*.h",
            "common/device/*.h",
            "common/device/*.cpp",
            "host/*.h",
            "host/*.cpp",
            "host/modules/*.h",
            "host/modules/*.cpp",
            "host/platform/*.h",
            "host/platform/*.cpp",
            "host/twin/sim/*.h",
            "host/twin/sim/*.cpp",
            "host/js/*.js",
            "host/twin/js/*.js",
            "host/twin/api/twin.js",
            "host/twin/parts/*/model.js",
            "host/twin/ui/x4.config.json",
            "lib/stdlib/src/*.js",
        }
        files(extraFiles)
        removefiles(excludedFiles)
        links { engine }

        -- The JS modules of the tool (host/js/name.js) are embedded in the
        -- executable as the modules "qjsx:name".
        filter "files:host/js/*.js"
            buildmessage "embedding %{file.name}"
            buildcommands {
                'premake5 --file="' .. EMBED_TOOL .. '" embed-js --input="%{file.abspath}"'
                    .. ' --output="%{cfg.objdir}/%{file.basename}.js.cpp" --name="qjsx:%{file.basename}"',
            }
            buildoutputs { "%{cfg.objdir}/%{file.basename}.js.cpp" }
            buildinputs { EMBED_TOOL }   -- regenerate when the tool changes
            compilebuildoutputs "On"

        -- The JS modules of the digital twin (host/twin/js/name.js) are
        -- embedded as "qjsx:twin/name".
        filter "files:host/twin/js/*.js"
            buildmessage "embedding twin/%{file.name}"
            buildcommands {
                'premake5 --file="' .. EMBED_TOOL .. '" embed-js --input="%{file.abspath}"'
                    .. ' --output="%{cfg.objdir}/twin-%{file.basename}.js.cpp" --name="qjsx:twin/%{file.basename}"',
            }
            buildoutputs { "%{cfg.objdir}/twin-%{file.basename}.js.cpp" }
            buildinputs { EMBED_TOOL }   -- regenerate when the tool changes
            compilebuildoutputs "On"

        -- The public API of the models of the twin's parts (host/twin/api/twin.js):
        -- "qjsx:twin".
        filter "files:host/twin/api/twin.js"
            buildmessage "embedding qjsx:twin"
            buildcommands {
                'premake5 --file="' .. EMBED_TOOL .. '" embed-js --input="%{file.abspath}"'
                    .. ' --output="%{cfg.objdir}/twin-api.js.cpp" --name="qjsx:twin"',
            }
            buildoutputs { "%{cfg.objdir}/twin-api.js.cpp" }
            buildinputs { EMBED_TOOL }
            compilebuildoutputs "On"

        -- The models of the parts of qjsx (host/twin/parts/<model>/model.js):
        -- "qjsx:twin/parts/<model>", the name of their folder.
        filter "files:host/twin/parts/*/model.js"
            buildmessage "embedding twin/parts/%{path.getname(file.directory)}"
            buildcommands {
                'premake5 --file="' .. EMBED_TOOL .. '" embed-js --input="%{file.abspath}"'
                    .. ' --output="%{cfg.objdir}/twin-part-%{path.getname(file.directory)}.js.cpp"'
                    .. ' --name="qjsx:twin/parts/%{path.getname(file.directory)}"',
            }
            buildoutputs { "%{cfg.objdir}/twin-part-%{path.getname(file.directory)}.js.cpp" }
            buildinputs { EMBED_TOOL }
            compilebuildoutputs "On"

        -- The web page of the twin (host/twin/ui, an x4js project), built
        -- again when its sources changed and Node is there, embedded as a
        -- zip archive (twin::uiArchive). The rule hangs on its config file.
        filter "files:host/twin/ui/x4.config.json"
            buildmessage "embedding the twin's page"
            buildcommands {
                'premake5 --file="' .. EMBED_TWIN_UI_TOOL .. '" embed-twin-ui --input="' .. TWIN_UI .. '"'
                    .. ' --output="%{cfg.objdir}/twin-ui.cpp"',
            }
            buildoutputs { "%{cfg.objdir}/twin-ui.cpp" }
            buildinputs(TWIN_UI_INPUTS)
            compilebuildoutputs "On"

        -- The modules of the standard library (lib/stdlib/src/name.js) are
        -- embedded too, as "stdlib/name": the tool needs them for its own
        -- commands and for "qjsx run script.js". A program built by "qjsx
        -- build" gets them from stdlib.qjslib instead (see below).
        filter "files:lib/stdlib/src/*.js"
            buildmessage "embedding stdlib/%{file.name}"
            buildcommands {
                'premake5 --file="' .. EMBED_TOOL .. '" embed-js --input="%{file.abspath}"'
                    .. ' --output="%{cfg.objdir}/stdlib-%{file.basename}.js.cpp" --name="stdlib/%{file.basename}"',
            }
            buildoutputs { "%{cfg.objdir}/stdlib-%{file.basename}.js.cpp" }
            buildinputs { EMBED_TOOL }   -- regenerate when the tool changes
            compilebuildoutputs "On"
        filter {}
        filter "system:windows"
            links { "ws2_32" }
        filter "system:not windows"
            links { "pthread", "m" }
        filter {}
end

hostApplication("qjsx", "qjsx-engine", {}, {}, {})
    -- The standard library is delivered as a file next to the executable
    -- (stdlib.qjslib): the tool just linked makes it from lib/stdlib. Every
    -- executable of the folder finds it there (qjsx-tests, qjsx-target).
    -- The board descriptions of the digital twin ("qjsx agent --board
    -- <name>") go next to it too, in boards/.
    postbuildmessage "making stdlib.qjslib, copying the boards"
    postbuildcommands {
        '"%{cfg.buildtarget.abspath}" mklib "' .. path.getabsolute("lib/stdlib")
            .. '" --output "%{cfg.targetdir}/stdlib.qjslib"',
        'mkdir -p "%{cfg.targetdir}/boards"',
        'cp -f "' .. path.getabsolute("boards") .. '"/*.json "%{cfg.targetdir}/boards/"',
    }

hostApplication("qjsx-tests", "qjsx-engine", { "tests/native/*.cpp" }, {}, { "QJSX_TESTS" })
    -- after qjsx: its stdlib.qjslib, and one build of the twin's page at a
    -- time (tools/embed-twin-ui.lua may run "npm run build")
    dependson { "qjsx" }
-- The tool linked with the engine without compiler: checks that everything
-- but the "compiler" module builds without it, and runs packages like a
-- target (tests/run-tests.sh). Its commands written in JS cannot run (they
-- are embedded as source).
hostApplication("qjsx-target", "qjsx-engine-target", {}, { "host/modules/compiler_module.cpp" }, { "QJSX_NO_COMPILER" })
    -- after qjsx: one build of the twin's page at a time
    dependson { "qjsx" }
-- The runner of a program made into an executable of the PC ("qjsx build
-- --executable" appends the package to a copy of it): the engine without
-- compiler, the runtime and the native modules of the PC; not the tool's
-- commands, its JS modules nor the standard library sources (a package
-- holds the library modules it uses). Its own main: host/runtime/.
-- device/serial (common/device/serial_module.cpp) stays: the PC's ports; the
-- other modules of common/device are those of a board (the twin).
hostApplication("qjsx-runtime", "qjsx-engine-target", { "host/runtime/*.cpp" },
    { "host/main.cpp", "host/agent_command.cpp", "host/modules/compiler_module.cpp",
      "host/twin/sim/*.cpp", "common/device/gpio_module.cpp", "common/device/rmt_module.cpp",
      "common/device/usb_module.cpp", "common/device/i2c_module.cpp", "common/device/adc_module.cpp",
      "host/js/*.js", "host/twin/js/*.js",
      "host/twin/api/twin.js", "host/twin/parts/*/model.js", "host/twin/ui/x4.config.json", "lib/stdlib/src/*.js" },
    { "QJSX_NO_COMPILER" })
    -- it is copied into every executable made: no debug symbols in Release
    filter "configurations:Release"
        linkoptions { "-s" }
    filter {}
