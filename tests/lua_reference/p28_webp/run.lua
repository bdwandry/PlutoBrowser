-- P28/P29 harness: decode a WebP file through the REAL Lua reference
-- (WebPDecoder._testDecodeRaw) and dump: width, height, and a rolling
-- 32-bit checksum over the pixel/byte array, plus the first 4 entries.
-- VP8 files return the flat rgb BYTE array, VP8L returns ARGB words —
-- we detect and dump both ways, plus the first 32 raw entries.
-- Usage: lua run.lua <file.webp> <out.txt>
local function main()
    local stubs = {
        dither = {},
        scale = {},
        tasks = { yieldCheck = function() end, reportProgress = function() end },
        webp_vp8_data = dofile("/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/webp_vp8_data.lua"),
    }
    local function fakeImport(path)
        local name = tostring(path):match("[%w_]+$")
        if stubs[name] then return stubs[name] end
        return dofile("/Users/bwandrych/Desktop/CometBrowser/Source/" .. path .. ".lua")
    end
    _G.import = fakeImport
    _G.Tasks = stubs.tasks -- webp.lua references the global Tasks at runtime

    if os.getenv("P29CK") then _G.P29CK = true end
    if os.getenv("P29CKMB") then _G.P29CKMB = true end
    if os.getenv("P29COEF") then _G.P29COEF = true end
    if os.getenv("P29PIX") then _G.P29PIX = true end
    if os.getenv("P29DBG") then
        _G.P29DBG = function(dec, mbY)
            if mbY ~= 0 then return end
            local n = math.min(dec.mbW, 8)
            for i = 0, n - 1 do
                local b = dec.mbData[i]
                print(string.format("MB %d uv=%d y0=%d nzY=%d nzUV=%d q=%d", i,
                    b.uvMode, b.imodes[1], b.nonZeroY, b.nonZeroUv, dec.quantAdd or -1))
            end
        end
    end
    dofile(os.getenv("WEBP_LUA") or "/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/webp.lua")
    local WD = _G.WebPDecoder
    assert(WD and WD._testDecodeRaw, "no _testDecodeRaw")

    local f = assert(io.open(arg[1], "rb"))
    local data = f:read("*a")
    f:close()

    local pix, w, h, alphaPlane = WD._testDecodeRaw(data)
    if os.getenv("P29FILT") and _G.FILTLIMITS then
        for _, l in ipairs(_G.FILTLIMITS) do print("FL " .. l) end
        print("FL rows=" .. tostring(_G.FILTCOUNT))
    end
    local out = assert(io.open(arg[2], "w"))
    if not pix then
        out:write("NULL\n")
        out:close()
        print("wrote " .. arg[2] .. " (NULL)")
        return
    end
    local n = w * h
    local function sumAndFirst4(count, fmt)
        local ck = 0
        local first4 = {}
        for i = 0, count - 1 do
            local v = pix[i] % 4294967296
            ck = (ck + v) % 4294967296
            if i < 4 then first4[#first4 + 1] = string.format(fmt, v) end
        end
        return ck, table.concat(first4, " ")
    end
    local bytesFmt = "%02x"
    -- Detect rgb-byte arrays: decodeVP8Payload returns flat bytes (w*h entries
    -- of 0..255), VP8L returns ARGB words (w*h entries). Heuristic: a byte
    -- array of size n has pix[n-1] <= 255 AND pix[n] == nil for n < #pix+1...
    -- Simpler: VP8 files always return rgb bytes; detect via RIFF chunk scan.
    local isVP8 = false
    if #data >= 16 and data:sub(9, 12) == "WEBP" then
        local pos = 13
        while pos + 8 <= #data do
            local cid = data:sub(pos, pos + 3)
            local s4, s5, s6 = data:byte(pos + 4, pos + 6)
            local size = (s4 or 0) | ((s5 or 0) << 8) | ((s6 or 0) << 16)
            if cid == "VP8 " then isVP8 = true; break end
            if cid == "ANMF" then isVP8 = false; break end
            pos = pos + 8 + size + (size & 1)
        end
    end
    local ck, first4
    if isVP8 then
        ck, first4 = sumAndFirst4(n, bytesFmt)
    else
        ck, first4 = sumAndFirst4(n, "%08x")
    end
    out:write(string.format("%d %d %08x %s\n", w, h, ck, first4))
    -- Raw dump of the first 32 entries for byte-level diffing.
    local raw = {}
    for i = 0, 31 do
        if pix[i] then
            raw[#raw + 1] = string.format("%02x", pix[i] % 256)
        end
    end
    out:write("raw32 " .. table.concat(raw, " ") .. "\n")
    -- Per-channel sums (rgb-byte arrays: 3 cycles; word arrays: ARGB shifts).
    if isVP8 then
        local rs, gs, bs = 0, 0, 0
        for i = 0, n - 1 do
            local o = i * 3
            rs = (rs + (pix[o] or 0)) % 4294967296
            gs = (gs + (pix[o + 1] or 0)) % 4294967296
            bs = (bs + (pix[o + 2] or 0)) % 4294967296
        end
        out:write(string.format("chan %08x %08x %08x\n", rs, gs, bs))
    else
        local as, rs, gs, bs = 0, 0, 0, 0
        for i = 0, n - 1 do
            local v = pix[i] % 4294967296
            as = (as + ((v >> 24) & 0xFF)) % 4294967296
            rs = (rs + ((v >> 16) & 0xFF)) % 4294967296
            gs = (gs + ((v >> 8) & 0xFF)) % 4294967296
            bs = (bs + (v & 0xFF)) % 4294967296
        end
        out:write(string.format("chan %08x %08x %08x %08x\n", as, rs, gs, bs))
    end
    if isVP8 then
        local all = {}
        for i = 0, n * 3 - 1 do all[#all + 1] = string.format("%02x", (pix[i] or 0) % 256) end
        out:write("all " .. table.concat(all, " ") .. "\n")
    end
    if alphaPlane then
        local asum = 0
        for i = 0, n - 1 do asum = (asum + (alphaPlane[i] or 0)) % 4294967296 end
        local af = {}
        for i = 0, 15 do af[#af + 1] = string.format("%02x", (alphaPlane[i] or 0) % 256) end
        out:write(string.format("alpha %08x %s\n", asum, table.concat(af, " ")))
    else
        out:write("alpha none\n")
    end
    out:close()
    print("wrote " .. arg[2] .. " OK")
end

main()
