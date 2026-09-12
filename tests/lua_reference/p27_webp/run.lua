-- P27 harness: call WebPDecoder._testBuildTable on vectors and dump the
-- flat (bits<<16)|value entries as "idx:bits:value" lines (idx = 0-based,
-- matching the C array indexing).
-- Usage: lua run.lua <vector.txt> <out.txt>
-- vector.txt format (one directive per line):
--   rootbits <n>
--   lengths <n1> <n2> ... (code lengths, index = symbol)
local function main()
    local vecPath, outPath = arg[1], arg[2]
    local stubDir = "/Users/bwandrych/Desktop/PlutoBrowser/tests/lua_reference/p27_webp/stubs/"

    local stubs = {
        dither = { M = {}, M_1 = {} }, -- never used by the machinery under test
        scale = {},
        tasks = {},
        webp_vp8_data = dofile("/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/webp_vp8_data.lua"),
    }

    -- import shim identical in spirit to the P19/P25 harnesses: prefer
    -- registered stubs, fall back to the real module file.
    local function fakeImport(path)
        local name = tostring(path):match("[%w_]+$")
        if stubs[name] then return stubs[name] end
        local real = "/Users/bwandrych/Desktop/CometBrowser/Source/" .. path .. ".lua"
        return dofile(real)
    end
    _G.import = fakeImport

    dofile("/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/webp.lua")
    local WD = _G.WebPDecoder
    assert(WD and WD._testBuildTable, "webp.lua did not expose _testBuildTable")

    local lines = {}
    for line in io.lines(vecPath) do
        local function toks()
            local t = {}
            for w in line:gmatch("%S+") do t[#t + 1] = w end
            return t
        end
        local t = toks()
        if t[1] == "rootbits" then
            stubs._rootbits = tonumber(t[2])
        elseif t[1] == "lengths" then
            local lens = {}
            for i = 2, #t do lens[#lens + 1] = tonumber(t[i]) end
            -- Lua 1-based call: _testBuildTable expects the reference's own
            -- 0-based numeric indexing inside, but Lua tables passed in are
            -- indexed [0..size-1] by buildHuffmanTable — so shift: table[t]
            -- must be readable at 0. Build with 0 at index 0.
            local lt = {}
            for i = 0, #lens - 1 do lt[i] = lens[i + 1] end
            local tab = WD._testBuildTable(lt, #lens, stubs._rootbits or 8)
            if tab then
                local maxIdx = 0
                for k in pairs(tab) do
                    if type(k) == "number" and k > maxIdx then maxIdx = k end
                    if type(k) == "number" and k < 0 then
                        io.write("NEG INDEX ", k, "\n")
                    end
                end
                lines[#lines + 1] = string.format("TABLE %d", maxIdx + 1)
                for i = 0, maxIdx do
                    local e = tab[i] or 0
                    lines[#lines + 1] = string.format("%d %d %d",
                        i, e >> 16, e & 0xFFFF)
                end
            else
                lines[#lines + 1] = "TABLE nil"
            end
        end
    end

    local f = assert(io.open(outPath, "w"))
    f:write(table.concat(lines, "\n"), "\n")
    f:close()
    print("wrote " .. outPath)
end

main()
