--          _
--  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
-- / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
-- \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
--    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

-- Embeds the web page of the digital twin (host/twin/ui, an x4js project) in
-- the qjsx executable:
--
--   premake5 --file=tools/embed-twin-ui.lua embed-twin-ui --input=host/twin/ui --output=twin-ui.cpp
--
-- 1. The sources of the page (src/, package.json, package-lock.json,
--    x4.config.json, tsconfig.json) are summed up by a CRC-32. dist/ keeps
--    the CRC-32 of the sources it was built from (dist/sources.crc32).
-- 2. They differ: when the npm packages are installed (node_modules/), the
--    page is built again ("npm run build") and the new CRC-32 written;
--    otherwise a warning, and the dist/ of the repository is used as it is
--    (a machine without Node builds qjsx with it).
-- 3. The files of dist/ (but sources.crc32) become a zip archive with stored
--    (uncompressed) entries, written in the C++ file as a byte array,
--    returned by twin::uiArchive() (host/twin/sim/twin.h).
--
-- Used as a custom build rule by premake5.lua: make runs it when a source of
-- the page or a file of dist/ changes. The C++ file is rewritten only when
-- the archive changes.

local SOURCE_FILES = { "src/**", "package.json", "package-lock.json", "x4.config.json", "tsconfig.json" }
local CRC_FILE = "sources.crc32"
local BYTES_PER_LINE = 24

-- ### CRC-32 ##################################################################

-- The table of the CRC-32 of zip (polynomial 0xEDB88320).
local crcTable = {}
for index = 0, 255 do
    local crc = index
    for _ = 1, 8 do
        if crc & 1 == 1 then
            crc = (crc >> 1) ~ 0xEDB88320
        else
            crc = crc >> 1
        end
    end
    crcTable[index] = crc
end

-- The CRC-32 of 'text', continuing 'crc' (0 to start).
local function crc32(text, crc)
    crc = (crc or 0) ~ 0xFFFFFFFF
    for index = 1, #text do
        local byte = text:byte(index)
        crc = crcTable[(crc ~ byte) & 0xFF] ~ (crc >> 8)
    end
    return crc ~ 0xFFFFFFFF
end

-- ### Files ###################################################################

-- The files of 'folder' matching 'patterns', as paths relative to it, sorted.
local function listFiles(folder, patterns)
    local names = {}
    for _, pattern in ipairs(patterns) do
        for _, file in ipairs(os.matchfiles(path.join(folder, pattern))) do
            table.insert(names, path.getrelative(folder, file))
        end
    end
    table.sort(names)
    return names
end

-- The CRC-32 of the sources of the page: names and contents, in order, as
-- 8 hexadecimal digits.
local function sourcesCrc(folder)
    local crc = 0
    for _, name in ipairs(listFiles(folder, SOURCE_FILES)) do
        crc = crc32(name, crc)
        crc = crc32(io.readfile(path.join(folder, name)) or "", crc)
    end
    return string.format("%08x", crc)
end

-- ### Build of the page #######################################################

-- Builds the page again when its sources changed since dist/ was built.
local function buildIfChanged(folder)
    -- the sources, against what dist/ was built from
    local current = sourcesCrc(folder)
    local crcPath = path.join(folder, "dist", CRC_FILE)
    local built = (io.readfile(crcPath) or ""):gsub("%s+", "")
    if built == current then
        return
    end

    -- no npm packages: the dist/ of the repository, with a warning
    if not os.isdir(path.join(folder, "node_modules")) then
        print("warning: the sources of the twin's page changed but its npm packages are not installed"
            .. " (npm install in " .. folder .. "): the page of dist/ is embedded as it is")
        return
    end

    -- build it, then keep the CRC-32 of its sources (x4js empties dist/ first)
    print("building the twin's page (npm run build in " .. folder .. ")")
    local here = os.getcwd()
    os.chdir(folder)
    local succeeded = os.execute("npm run build")
    os.chdir(here)
    if not succeeded then
        error("the build of the twin's page failed")
    end
    io.writefile(crcPath, current .. "\n")
end

-- ### Zip #####################################################################

-- 'value' as 'count' bytes, little endian.
local function littleEndian(value, count)
    local bytes = {}
    for index = 1, count do
        bytes[index] = string.char((value >> ((index - 1) * 8)) & 0xFF)
    end
    return table.concat(bytes)
end

-- A zip archive of the files 'names' of 'folder', entries stored, no time.
local function makeZip(folder, names)
    local parts = {}         -- local headers and data, in order
    local central = {}       -- the central directory entries
    local offset = 0         -- offset of the next local header

    for _, name in ipairs(names) do
        -- one entry: local header, then the data as is
        local data = io.readfile(path.join(folder, name))
        local crc = crc32(data)
        local localHeader = table.concat({
            littleEndian(0x04034b50, 4),    -- signature
            littleEndian(20, 2),            -- version needed: 2.0
            littleEndian(0x0800, 2),        -- flags: UTF-8 names
            littleEndian(0, 2),             -- method: stored
            littleEndian(0, 2),             -- time
            littleEndian(0x21, 2),          -- date: 1980-01-01
            littleEndian(crc, 4),
            littleEndian(#data, 4),         -- compressed size
            littleEndian(#data, 4),         -- size
            littleEndian(#name, 2),
            littleEndian(0, 2),             -- extra field
            name,
        })
        table.insert(parts, localHeader)
        table.insert(parts, data)

        -- its central directory entry
        table.insert(central, table.concat({
            littleEndian(0x02014b50, 4),    -- signature
            littleEndian(20, 2),            -- version made by
            littleEndian(20, 2),            -- version needed
            littleEndian(0x0800, 2),        -- flags: UTF-8 names
            littleEndian(0, 2),             -- method: stored
            littleEndian(0, 2),             -- time
            littleEndian(0x21, 2),          -- date
            littleEndian(crc, 4),
            littleEndian(#data, 4),
            littleEndian(#data, 4),
            littleEndian(#name, 2),
            littleEndian(0, 2),             -- extra field
            littleEndian(0, 2),             -- comment
            littleEndian(0, 2),             -- disk
            littleEndian(0, 2),             -- internal attributes
            littleEndian(0, 4),             -- external attributes
            littleEndian(offset, 4),        -- offset of the local header
            name,
        }))
        offset = offset + #localHeader + #data
    end

    -- the central directory, then its end record
    local directory = table.concat(central)
    local endRecord = table.concat({
        littleEndian(0x06054b50, 4),        -- signature
        littleEndian(0, 2),                 -- disk
        littleEndian(0, 2),                 -- disk of the directory
        littleEndian(#names, 2),            -- entries on this disk
        littleEndian(#names, 2),            -- entries
        littleEndian(#directory, 4),
        littleEndian(offset, 4),            -- offset of the directory
        littleEndian(0, 2),                 -- comment
    })
    return table.concat(parts) .. directory .. endRecord
end

-- ### C++ #####################################################################

-- The C++ file: the archive as a byte array, and twin::uiArchive().
local function cppSource(archive, folder)
    local lines = {
        "// Generated from " .. folder .. "/dist by tools/embed-twin-ui.lua: do not edit.",
        '#include "twin.h"',
        "",
        "static const uint8_t archive[] = {",
    }
    for start = 1, #archive, BYTES_PER_LINE do
        local bytes = { archive:byte(start, math.min(start + BYTES_PER_LINE - 1, #archive)) }
        table.insert(lines, "    " .. table.concat(bytes, ",") .. ",")
    end
    table.insert(lines, "};")
    table.insert(lines, "")
    table.insert(lines, "const uint8_t *twin::uiArchive(size_t *size) {")
    table.insert(lines, "    *size = sizeof(archive);")
    table.insert(lines, "    return archive;")
    table.insert(lines, "}")
    table.insert(lines, "")
    return table.concat(lines, "\n")
end

-- Steps 1 to 3 (see the top of the file).
local function embedTwinUi(folder, output)
    -- the page, built again if needed
    buildIfChanged(folder)

    -- dist/ as a zip
    local dist = path.join(folder, "dist")
    local names = {}
    for _, name in ipairs(listFiles(dist, { "**" })) do
        if name ~= CRC_FILE then
            table.insert(names, name)
        end
    end
    if #names == 0 then
        error("the twin's page is not built: " .. dist .. " is empty (npm install, then npm run build in " .. folder .. ")")
    end
    local content = cppSource(makeZip(dist, names), folder)

    -- rewrite only on change, so that make does not recompile for nothing
    os.mkdir(path.getdirectory(output))
    if io.readfile(output) ~= content then
        io.writefile(output, content)
    elseif os.touchfile ~= nil then
        os.touchfile(output)
    end
end

newoption { trigger = "input", value = "FOLDER", description = "folder of the twin's page (x4js project)" }
newoption { trigger = "output", value = "FILE", description = "C++ file to write" }

newaction {
    trigger = "embed-twin-ui",
    description = "Embed the web page of the digital twin as a zip archive",
    execute = function()
        embedTwinUi(_OPTIONS["input"], _OPTIONS["output"])
    end,
}
