-- Authoritative capture of the P18 helper functions (verbatim from document.lua)
Entities = { encode = function(s) return s:gsub("&","&amp;"):gsub("<","&lt;"):gsub(">","&gt;") end }
local function parseStyle(styleStr)
    local out = {}
    if not styleStr or styleStr == "" then return out end
    for k, v in string.gmatch(styleStr, "([%w%-]+)%s*:%s*([^;]+)") do
        out[string.lower(k)] = string.gsub(string.lower(v), "^%s*(.-)%s*$", "%1")
    end
    return out
end
local function parseAlign(attrs)
    if not attrs then return nil end
    local a = attrs["align"]
    local st = parseStyle(attrs["style"])
    if st["text-align"] then a = st["text-align"] end
    if not a then return nil end
    a = string.lower(a)
    if a == "center" or a == "right" or a == "left" then return a end
    return nil
end
local function isDisplayNone(attrs)
    if not attrs then return false end
    if attrs["hidden"] ~= nil then return true end
    if attrs["popover"] ~= nil then return true end
    local st = parseStyle(attrs["style"])
    local d = st["display"]
    if d and string.match(d, "none") then return true end
    local v = st["visibility"]
    if v and string.match(v, "hidden") then return true end
    return false
end
local function isInvertedStyle(attrs)
    if not attrs then return false end
    local st = parseStyle(attrs["style"])
    local c = st["color"]
    local bg = st["background-color"] or st["background"]
    if c and (string.match(c, "white") or string.match(c, "%#fff") or string.match(c, "%#FFFF%w*")) then
        return true
    end
    if bg and (string.match(bg, "black") or string.match(bg, "%#000") or string.match(bg, "%#000000")) then
        return true
    end
    return false
end
local function numv(v)
    if not v then return nil end
    v = string.gsub(v, "%%", "")
    local n = tonumber(v)
    if not n then return nil end
    return math.floor(n / 2)
end
local function parseBoxSpacing(attrs)
    local out = { top = 0, bottom = 0, left = 0, right = 0 }
    if not attrs then return out end
    local st = parseStyle(attrs["style"])
    local function num(v)
        if not v then return nil end
        v = string.gsub(v, "%%", "")
        local n = tonumber(v)
        if not n then return nil end
        return math.floor(n / 2)
    end
    local m = st["margin"]
    if m then
        local t, r, b, l
        local parts = {}
        for p in string.gmatch(m, "[^%s,]+") do parts[#parts + 1] = p end
        if #parts == 1 then t, r, b, l = num(parts[1]), num(parts[1]), num(parts[1]), num(parts[1])
        elseif #parts == 2 then t, r, b, l = num(parts[1]), num(parts[2]), num(parts[1]), num(parts[2])
        elseif #parts == 3 then t, r, b, l = num(parts[1]), num(parts[2]), num(parts[3]), num(parts[2])
        elseif #parts == 4 then t, r, b, l = num(parts[1]), num(parts[2]), num(parts[3]), num(parts[4]) end
        if t then out.top = t end
        if b then out.bottom = b end
        if l then out.left = l end
        if r then out.right = r end
    else
        if st["margin-top"] then out.top = num(st["margin-top"]) or 0 end
        if st["margin-bottom"] then out.bottom = num(st["margin-bottom"]) or 0 end
        if st["margin-left"] then out.left = num(st["margin-left"]) or 0 end
        if st["margin-right"] then out.right = num(st["margin-right"]) or 0 end
    end
    local p = st["padding"]
    if p then
        local parts = {}
        for part in string.gmatch(p, "[^%s,]+") do parts[#parts + 1] = part end
        if #parts == 1 then
            out.left = out.left + (num(parts[1]) or 0)
        elseif #parts == 2 then
            out.left = out.left + (num(parts[2]) or 0)
        elseif #parts == 4 then
            out.left = out.left + (num(parts[4]) or 0)
        end
    else
        if st["padding-left"] then out.left = out.left + (num(st["padding-left"]) or 0) end
    end
    return out
end
local function boxstr(b)
    return string.format("t%d b%d l%d r%d", b.top, b.bottom, b.left, b.right)
end
local function printfmt(name, f, attrs)
    local st = parseStyle(attrs and attrs["style"] or nil)
    local keys = {}
    for k in pairs(st) do keys[#keys+1] = k end
    table.sort(keys)
    local sstr = {}
    for _, k in ipairs(keys) do sstr[#sstr+1] = k .. "=" .. st[k] end
    print(string.format("%s|style=[%s]|align=%s|dnone=%s|inv=%s|box=%s",
      name, table.concat(sstr, ","),
      tostring(parseAlign(attrs)), tostring(isDisplayNone(attrs)),
      tostring(isInvertedStyle(attrs)), boxstr(parseBoxSpacing(attrs))))
end
printfmt("tc1a", nil, { style = "margin: 10px; padding: 5px" })
printfmt("tc1b", nil, { style = "margin: 10px 20px" })
printfmt("tc1c", nil, { style = "margin: 10px 20px 30px" })
printfmt("tc1d", nil, { style = "margin: 10px 20px 30px 40px; padding: 1px 2px" })
printfmt("tc1e", nil, { style = "MARGIN-Top: 33%" })
printfmt("tc1f", nil, { style = "margin-left:7px ; color : RED" })
printfmt("tc1g", nil, nil)
printfmt("tc2a", nil, { align = "CENTER" })
printfmt("tc2b", nil, { align = "left", style = "text-align: right" })
printfmt("tc2c", nil, { style = "text-align: justify" })
printfmt("tc2d", nil, { align = "justify" })
printfmt("tc3a", nil, { hidden = "" })
printfmt("tc3b", nil, { popover = "" })
printfmt("tc3c", nil, { style = "display: none" })
printfmt("tc3d", nil, { style = "visibility:hidden" })
printfmt("tc3e", nil, { style = "display:none; color: white" })
printfmt("tc3f", nil, { style = "display: block" })
printfmt("tc3g", nil, { align = "left" })
printfmt("tc4a", nil, { style = "color: white" })
printfmt("tc4b", nil, { style = "color: #fff" })
printfmt("tc4c", nil, { style = "color:#FFFF80" })
printfmt("tc4d", nil, { style = "background: black" })
printfmt("tc4e", nil, { style = "background-color:#000000" })
printfmt("tc4f", nil, { style = "color:#000" })
printfmt("tc4g", nil, { style = "background: #FFF" })
printfmt("tc4h", nil, { style = "color: #ff00ff" })
printfmt("tc5a", nil, { style = "padding: 8px" })
printfmt("tc5b", nil, { style = "padding: 8px 16px; margin: 4%" })
printfmt("tc5c", nil, { style = "margin-top: -3px; margin-bottom: 7.9px" })
printfmt("tc5d", nil, { style = "padding-left: 10px; margin: 2px 4px 6px 8px" })
