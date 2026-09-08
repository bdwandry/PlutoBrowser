-- P25 JPEG reference harness: run CometBrowser's jpeg.lua verbatim and dump
-- the pre-dither grayscale grid (what Dither.toImage would consume).
-- Usage: lua run.lua <jpeg.bin> <out.txt>
package.path = "/Users/bwandrych/Desktop/CometBrowser/Source/?.lua;" ..
               "/Users/bwandrych/Desktop/CometBrowser/Source/?;" ..
               "/Users/bwandrych/Desktop/PlutoBrowser/tests/lua_reference/p25_jpeg/stubs/?.lua;" ..
               "/Users/bwandrych/Desktop/PlutoBrowser/tests/lua_reference/p25_jpeg/stubs/?;" ..
               package.path

-- playdate stub (Tasks needs playdate.getCurrentTimeMilliseconds)
_G.playdate = {
    getCurrentTimeMilliseconds = function() return 0 end,
    timer = { performAfterDelay = function() end, updateTimers = function() end },
    graphics = { image = { new = function() return nil end } },
}

-- SDK import shim: stubs first, then load the module file and set it as a
-- global by basename (Playdate's import() semantics for these modules).
-- Modules referenced by jpeg.lua with their global names and (for dither)
-- our capturing stub. Everything else loads verbatim.
local STUBS = {
    ["render/decoders/dither"] = function() return _G.Dither end,
    ["core/tasks"] = function() return _G.Tasks end,
    ["render/decoders/scale"] = function() return _G.Scale end,
}
local GLOBAL_NAMES = {
    ["render/decoders/dither"] = "Dither",
    ["core/tasks"] = "Tasks",
    ["render/decoders/scale"] = "Scale",
}
function _G.import(name)
    local gname = GLOBAL_NAMES[name]
    if STUBS[name] then
        local mod = STUBS[name]()
        _G[gname] = mod
        return mod
    end
    dofile("/Users/bwandrych/Desktop/CometBrowser/Source/" .. name .. ".lua")
    return _G[gname]
end

-- Load the reference modules. NOTE: scale.lua/tasks.lua set their globals
-- (Scale/Tasks) internally and return nil — do NOT assign dofile's return
-- over them.
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/core/tasks.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/scale.lua")
assert(type(_G.Tasks) == "table", "tasks.lua did not set Tasks global")
assert(type(_G.Scale) == "table", "scale.lua did not set Scale global")

-- Dither stub: capture the grayscale grid BEFORE the 1-bit dither.
_G.Dither = {
    toImage = function(getPixel, w, h)
        local rows = {}
        for y = 0, h - 1 do
            local r = {}
            for x = 0, w - 1 do
                r[x] = getPixel(x, y)
            end
            rows[y] = r
        end
        _G.P25_GRID = rows
        _G.P25_W = w
        _G.P25_H = h
        return { dummy = true }
    end,
}
package.loaded["render/decoders/dither"] = _G.Dither

local jpegPath = arg[1]
local outPath = arg[2]
local maxW = tonumber(arg[3]) or 360
local maxH = tonumber(arg[4]) or 200
local f = io.open(jpegPath, "rb")
if not f then
    print("cannot open " .. tostring(jpegPath))
    os.exit(2)
end
local data = f:read("*a")
f:close()

-- Load jpeg.lua verbatim (it expects globals Tasks/Scale/Dither and returns
-- nothing; it sets the global JPEGDecoder).
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/jpeg.lua")

local ok, img = pcall(function() return JPEGDecoder.decode(data, maxW, maxH) end)
local out = io.open(outPath, "w")
if not ok then
    out:write("ERROR\n" .. tostring(img) .. "\n")
    out:close()
    print("decode error: " .. tostring(img))
    os.exit(1)
end
if img == nil then
    out:write("NULL\n")
    out:close()
    print("decode: nil")
    os.exit(0)
end
local grid, w, h = _G.P25_GRID, _G.P25_W, _G.P25_H
out:write(w .. " " .. h .. "\n")
for y = 0, h - 1 do
    local vals = {}
    for x = 0, w - 1 do
        vals[x + 1] = tostring(grid[y][x])
    end
    out:write(table.concat(vals, " ") .. "\n")
end
out:close()
print("decode: " .. w .. "x" .. h)
