--          _
--  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
-- / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
-- \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
--    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

-- Embeds a file in a C++ file that registers it as an embedded file of bytes
-- of the runtime (see rt::EmbeddedBytesRegistration in
-- common/runtime/modules/modules.h): what a module embedded as source imports
-- with { type: "bytes" } (the fonts of the standard library).
--
--   premake5 --file=tools/embed-bytes.lua embed-bytes --input=a.bin --output=a.bin.cpp --name=stdlib/fonts/a.bin
--
-- Used as a custom build rule by premake5.lua: make regenerates the C++ file
-- only when the file changes.

-- Bytes written on one line of the C++ array.
local BYTES_PER_LINE = 16

-- The bytes of 'input', read as they are (binary mode: no line end turned
-- into another on Windows).
local function readBytes(input)
    local file = io.open(input, "rb")
    if file == nil then
        error("cannot read " .. input)
    end
    local data = file:read("*a")
    file:close()
    return data
end

-- The lines of the C++ array that holds 'data', one hex byte after another.
local function arrayLines(data)
    local lines = {}
    for start = 1, #data, BYTES_PER_LINE do
        local chunk = data:sub(start, start + BYTES_PER_LINE - 1)
        local values = {}
        for index = 1, #chunk do
            table.insert(values, string.format("0x%02x,", chunk:byte(index)))
        end
        table.insert(lines, "    " .. table.concat(values, " "))
    end

    -- an array of C++ cannot be empty: one byte, not counted in the size
    if #lines == 0 then
        table.insert(lines, "    0x00,")
    end
    return lines
end

-- The C++ file of 'input', registered under 'name'.
local function embedBytes(input, output, name)
    -- the array, aligned on 8 bytes as in a package, then the registration
    local data = readBytes(input)
    local content = table.concat({
        "// Generated from " .. input .. " by tools/embed-bytes.lua: do not edit.",
        '#include "modules/modules.h"',
        "",
        "alignas(8) static const uint8_t data[] = {",
        table.concat(arrayLines(data), "\n"),
        "};",
        "",
        'static rt::EmbeddedBytesRegistration registration("' .. name .. '", data, ' .. #data .. ");",
        "",
    }, "\n")

    -- written only on change, so that make does not rebuild for nothing
    os.mkdir(path.getdirectory(output))
    if io.readfile(output) ~= content then
        io.writefile(output, content)
    end
end

newoption { trigger = "input", value = "FILE", description = "file to embed" }
newoption { trigger = "output", value = "FILE", description = "C++ file to write" }
newoption { trigger = "name", value = "NAME", description = "module name (as normalized from its import)" }

newaction {
    trigger = "embed-bytes",
    description = "Embed a file as bytes imported by a module of the runtime",
    execute = function()
        embedBytes(_OPTIONS["input"], _OPTIONS["output"], _OPTIONS["name"])
    end,
}
