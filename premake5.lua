-- espjs build (desktop).
--   premake5 gmake --cc=gcc --shell=posix
--   make -C build config=release        (or config=debug)
--
-- Projects:
--   qjsx         the engine (engine/qjsx: QuickJS split and modified for espjs), static library
--   qjsx-target  the same without compiler (no eval), like on the targets
--   espjs        the host tool
--   espjs-tests  espjs + the native test modules (tests/native)
--   espjs-target espjs without the compiler module, linked with qjsx-target
--
-- Folders: engine/ (QuickJS), common/ (portable code, also built for the
-- ESP: jsbind, runtime, agent), host/ (the desktop tool only).
-- Headers are included by their name alone: every folder below is in the
-- include path, and no two headers share a name.

local QJSX = "engine/qjsx"
local QUICKJS_VERSION = io.readfile(QJSX .. "/VERSION"):gsub("%s+", "")
local EMBED_TOOL = path.getabsolute("tools/embed-js.lua")

workspace "espjs"
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

engineLibrary("qjsx", true)
engineLibrary("qjsx-target", false)

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
            "host",
            "host/modules",
            "host/platform",
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
            "host/*.h",
            "host/*.cpp",
            "host/modules/*.h",
            "host/modules/*.cpp",
            "host/platform/*.h",
            "host/platform/*.cpp",
            "host/js/*.js",
            "lib/stdlib/src/*.js",
        }
        files(extraFiles)
        removefiles(excludedFiles)
        links { engine }

        -- The JS modules of the tool (host/js/name.js) are embedded in the
        -- executable as the modules "espjs:name".
        filter "files:host/js/*.js"
            buildmessage "embedding %{file.name}"
            buildcommands {
                'premake5 --file="' .. EMBED_TOOL .. '" embed-js --input="%{file.abspath}"'
                    .. ' --output="%{cfg.objdir}/%{file.basename}.js.cpp" --name="espjs:%{file.basename}"',
            }
            buildoutputs { "%{cfg.objdir}/%{file.basename}.js.cpp" }
            buildinputs { EMBED_TOOL }   -- regenerate when the tool changes
            compilebuildoutputs "On"

        -- The modules of the standard library (lib/stdlib/src/name.js) are
        -- embedded too, as "stdlib/name": the tool needs them for its own
        -- commands and for "espjs run script.js". A program built by "espjs
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

hostApplication("espjs", "qjsx", {}, {}, {})
    -- The standard library is delivered as a file next to the executable
    -- (stdlib.qjslib): the tool just linked makes it from lib/stdlib. Every
    -- executable of the folder finds it there (espjs-tests, espjs-target).
    postbuildmessage "making stdlib.qjslib"
    postbuildcommands {
        '"%{cfg.buildtarget.abspath}" mklib "' .. path.getabsolute("lib/stdlib")
            .. '" --output "%{cfg.targetdir}/stdlib.qjslib"',
    }

hostApplication("espjs-tests", "qjsx", { "tests/native/*.cpp" }, {}, { "ESPJS_TESTS" })
-- The tool linked with the engine without compiler: checks that everything
-- but the "compiler" module builds without it, and runs packages like a
-- target (tests/run-tests.sh). Its commands written in JS cannot run (they
-- are embedded as source).
hostApplication("espjs-target", "qjsx-target", {}, { "host/modules/compiler_module.cpp" }, { "ESPJS_NO_COMPILER" })
