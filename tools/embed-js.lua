-- Embeds a JS file in a C++ file that registers it as an embedded module of
-- the runtime (see rt::EmbeddedRegistration in common/runtime/modules/modules.h).
--
--   premake5 --file=tools/embed-js.lua embed-js --input=a.js --output=a.js.cpp --name=espjs:a
--
-- Used as a custom build rule by premake5.lua: make regenerates the C++ file
-- only when the JS file changes.

-- C++ raw string delimiter (16 characters at most).
local DELIMITER = "espjs_js"

local function embedJs(input, output, name)
    local source = io.readfile(input)
    if source == nil then
        error("cannot read " .. input)
    end
    if source:find(")" .. DELIMITER .. '"', 1, true) then
        error(input .. " contains the raw string delimiter " .. DELIMITER)
    end
    local content = table.concat({
        "// Generated from " .. input .. " by tools/embed-js.lua: do not edit.",
        '#include "modules/modules.h"',
        "",
        "static const char source[] = R\"" .. DELIMITER .. "(" .. source .. ")" .. DELIMITER .. "\";",
        "",
        'static rt::EmbeddedRegistration registration("' .. name .. '", source);',
        "",
    }, "\n")
    os.mkdir(path.getdirectory(output))
    -- Rewrite only on change, so that make does not rebuild for nothing.
    if io.readfile(output) ~= content then
        io.writefile(output, content)
    end
end

newoption { trigger = "input", value = "FILE", description = "JS file to embed" }
newoption { trigger = "output", value = "FILE", description = "C++ file to write" }
newoption { trigger = "name", value = "NAME", description = "module name (import specifier)" }

newaction {
    trigger = "embed-js",
    description = "Embed a JS file as a module of the runtime",
    execute = function()
        embedJs(_OPTIONS["input"], _OPTIONS["output"], _OPTIONS["name"])
    end,
}
