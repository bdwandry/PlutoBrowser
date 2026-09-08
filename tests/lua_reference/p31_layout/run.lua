-- P31 reference harness: captures the verbatim local helpers from layout.lua
-- plus breakLines/emitFlow outputs on constructed vectors, using stub
-- Style/LinkManager/Constants/Tasks/gfx so the REAL file loads untouched.
-- Usage: lua run.lua <vector> <out.txt>   vector: helpers|break|emit
local W = (os.getenv("P31V") or "helpers")
local out = {}

-- ── stubs the reference's imports need ─────────────────────────────────────
Tasks = { run = function() end, yieldCheck = function() end, reportProgress = function() end }
Constants = {
  CONTENT_Y = 24, CONTENT_HEIGHT = 216, CONTENT_WIDTH = 400, SCREEN_WIDTH = 400,
  SCREEN_HEIGHT = 240, CONTENT_MARGIN = 8, CONTENT_TEXT_WIDTH = 384,
  SCROLLBAR_WIDTH = 5, IMAGE_MODE_ALL = "all", IMAGE_MODE_VIEWPORT = "viewport",
}
-- deterministic metrics: width = (#text * 10) for any font, " " = 4
local function mw(font, text) if not text or text == "" then return 0 end return #text * 10 end
Style = {
  fontBody = "BODY", fontBodyBold = "BOLD", fontMono = "MONO", fontSmall = "SMALL",
  getTextWidth = function(font, text) return mw(font, text) end,
  getInlineFont = function(b, c, s, sub, sup, big)
    if c then return "MONO", 15
    elseif s or sub or sup then return "SMALL", 14
    elseif b or big then return "BOLD", 16
    else return "BODY", 16 end
  end,
  getHeadingFont = function(level)
    if level == 1 then return "H1", 24, 6
    elseif level == 2 then return "H2", 18, 5
    elseif level == 3 then return "H3", 17, 4
    elseif level == 4 then return "H4", 16, 4
    elseif level == 5 then return "H5", 16, 4
    else return "H6", 16, 4 end
  end,
}
LinkManager = { links = {}, addLinkRect = function(h, t, r, ai)
  LinkManager.links[#LinkManager.links + 1] = { href = h, text = t, rect = r, anchor = ai }
end, clear = function() LinkManager.links = {} end,
  getSelectedLink = function() return nil end,
  drawSelectedHighlight = function() end,
  isHighlighted = function() return false end }
ImageDecoder = { isDecoded = function() return false end, getImage = function() return nil end,
  enqueue = function() end, draw = function() end, evict = function() end }
Storage = { settings = {} }
gfx = setmetatable({}, { __index = function(t, k)
  if k == "kColorWhite" or k == "kColorBlack" then return k end
  return function() end
end })
playdate = { graphics = gfx, timer = { performAfterDelay = function() end } }
Logger = { error = function() end }

-- font stubs need getTextWidth method for Style.getTextWidth(font, ...) path?
-- Reference layout calls Style.getTextWidth(font, text) directly — covered.

-- load the REAL layout.lua with our globals (import is a Playdate no-op);
-- the local helpers are captured by string-extracting their definitions from
-- the verbatim file (they are file-locals, not reachable through dofile).
function import() end
local src = io.open("/Users/bwandrych/Desktop/CometBrowser/Source/render/layout.lua"):read("*a")
-- extract each local function block by name and load it in our env
local function grab(name)
  local a, b = src:find("local function " .. name .. "%b()")
  assert(a, "fn not found: " .. name)
  -- find the extent: scan to column-0 "end" after body start
  local e = src:find("\nend\n", b, true)
  assert(e, "end not found for " .. name)
  return src:sub(a, e + 4)
end
TAB_COLUMNS = 8 -- verbatim constant from layout.lua
for _, name in ipairs({ "normalizeAlign", "toRoman", "toAlpha", "orderedMarker",
                        "tabAdvance", "expandTabColumns" }) do
  local code = grab(name)
  -- expose as global for test access
  code = code:gsub("^local function " .. name, "function " .. name)
  assert(load(code, name, "t", _G))()
end

-- breakLines/emitFlow: slice between their definition markers.
local function slice(fromPat, toPat)
  local a, b = src:find(fromPat)
  assert(a, fromPat)
  local e = src:find(toPat, b, true)
  assert(e, toPat)
  return src:sub(a, e - 1)
end
local lines = {}
local li2 = 0
for line in src:gmatch("([^\n]*)\n") do
  li2 = li2 + 1
  lines[li2] = line
end
local function sliceLines(a, b)  -- inclusive 1-based line numbers
  local t = {}
  for i = a, b do t[#t+1] = lines[i] end
  return table.concat(t, "\n")
end
local blCode = sliceLines(99, 180)   -- breakLines (comment 98 not needed)
blCode = blCode:gsub("^local function breakLines", "function breakLines")
assert(load(blCode, "breakLines", "t", _G))()
local efCode = sliceLines(183, 258)  -- emitFlow body after comment lines
efCode = efCode:gsub("^local function emitFlow", "function emitFlow")
assert(load(efCode, "emitFlow", "t", _G))()
-- expose TAB_COLUMNS for the tabAdvance closure? tabAdvance references
-- Style/gfx globals — already stubbed above.
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/render/layout.lua")

if W == "helpers" then
  -- normalizeAlign via build is indirect; test toRoman/toAlpha/orderedMarker
  -- and expandTabColumns only (normalizeAlign is exercised via emit vectors).
  for _, n in ipairs({1, 2, 3, 4, 5, 9, 10, 14, 38, 40, 49, 50, 51, 90, 99, 100,
                      444, 999, 1000, 1994, 3999, 4000, 0, -5}) do
    out[#out+1] = string.format("roman %d = %s", n, tostring(toRoman(n)))
  end
  for _, n in ipairs({1, 2, 25, 26, 27, 52, 53, 702, 703, 0, -1}) do
    out[#out+1] = string.format("alpha %d = %s", n, tostring(toAlpha(n)))
  end
  local types = {"1", "a", "A", "i", "I", nil, "x"}
  for _, mt in ipairs(types) do
    for _, n in ipairs({1, 5, 27, 444}) do
      out[#out+1] = string.format("marker %s %d = %s", tostring(mt), n,
                                  tostring(orderedMarker(n, mt)))
    end
  end
  for _, l in ipairs({"a\tb", "\tx", "abc", "ab\t\tcd", "a\nb"}) do
    out[#out+1] = string.format("expand %q = %q", l, expandTabColumns(l))
  end
elseif W == "break" then
  local function dump(lines, lineH, tag)
    out[#out+1] = string.format("%s lineH=%d n=%d", tag, lineH, #lines)
    for li, line in ipairs(lines) do
      local ws = {}
      for wi, wd in ipairs(line.words) do
        ws[#ws+1] = string.format("%s/%s/w%d/a%d", wd.text, tostring(wd.font), wd.w, wd.advance)
      end
      out[#out+1] = string.format("  L%d width=%d words=[%s]", li, line.width, table.concat(ws, ","))
    end
  end
  -- vector 1: simple wrap (word width = len*10, space = 4)
  local inlines = { { type = "text", text = "the quick brown fox jumps over the lazy dog" } }
  local lines, lh = breakLines(inlines, 120, {})
  dump(lines, lh, "v1")
  -- vector 2: bold inline font selection + mixed sizes raise lineH
  local inlines2 = {
    { type = "text", text = "small ", small = true },
    { type = "text", text = "boldcode ", bold = true, code = true },
    { type = "text", text = "plain words here" },
  }
  local lines2, lh2 = breakLines(inlines2, 90, {})
  dump(lines2, lh2, "v2")
  -- vector 3: br handling + leading/trailing whitespace collapse
  local inlines3 = {
    { type = "text", text = "  spaced   out  " },
    { type = "br" },
    { type = "text", text = "after" },
  }
  local lines3, lh3 = breakLines(inlines3, 200, {})
  dump(lines3, lh3, "v3")
  -- vector 4: tab gap advance (tab in ws run → tabAdvance from cur.width+ww)
  local inlines4 = { { type = "text", text = "ab\tcd ef" } }
  local lines4, lh4 = breakLines(inlines4, 400, {})
  dump(lines4, lh4, "v4")
  -- vector 5: first-word overflow stays on line
  local inlines5 = { { type = "text", text = "superlongword" } }
  local lines5, lh5 = breakLines(inlines5, 50, {})
  dump(lines5, lh5, "v5")
  -- vector 6: opts.font fixed font
  local lines6, lh6 = breakLines(inlines, 130, { font = "MONO", lineH = 15 })
  dump(lines6, lh6, "v6")
  -- vector 7: opts.bold
  local lines7, lh7 = breakLines(inlines, 130, { bold = true })
  dump(lines7, lh7, "v7")
elseif W == "emit" then
  -- Build lines via the real breakLines, then run emitFlow with alignments.
  local inlines = {
    { type = "text", text = "hello linked world", href = nil },
  }
  -- give the middle words a link
  local inlines2 = {
    { type = "text", text = "go " },
    { type = "text", text = "to ", href = "http://x", anchorIndex = 0 },
    { type = "text", text = "the ", href = "http://x", anchorIndex = 0 },
    { type = "text", text = "store", href = "http://x", anchorIndex = 0 },
    { type = "text", text = " now" },
  }
  local aligns = { "SENTINEL-NIL", "left", "center", "right" }
  for _, align0 in ipairs(aligns) do
    local align = (align0 == "SENTINEL-NIL") and nil or align0
    LinkManager.links = {}
    Layout.renderItems = {}
    local lines, lh = breakLines(inlines2, 400, {})
    local items, endY = emitFlow(lines, lh, 10, 200, align, 33, {})
    out[#out+1] = string.format("align=%s lh=%d endY=%d items=%d",
                                tostring(align), lh, endY, #items)
    for _, it in ipairs(items) do
      out[#out+1] = string.format("  text=%q font=%s x=%d y=%d w=%d h=%d href=%s bold=%s",
        it.text, tostring(it.font), it.x, it.y, it.w, it.h,
        tostring(it.href), tostring(it.bold))
    end
    for _, l in ipairs(LinkManager.links) do
      out[#out+1] = string.format("  link href=%s text=%s rect={%d,%d,%d,%d} anchor=%s",
        tostring(l.href), tostring(l.text), l.rect.x, l.rect.y, l.rect.w, l.rect.h,
        tostring(l.anchor))
    end
  end
  -- sub/sup dy
  Layout.renderItems = {}
  local inlines3 = {
    { type = "text", text = "x", sub = true },
    { type = "text", text = "y", sup = true },
  }
  local lines3, lh3 = breakLines(inlines3, 400, {})
  local items3 = emitFlow(lines3, lh3, 0, 200, nil, 10, {})
  for _, it in ipairs(items3) do
    out[#out+1] = string.format("dy text=%s y=%d", it.text, it.y)
  end
elseif W == "build" then
  local function T(t)
    return { type = "text", text = t }
  end
  local blocks = {}
  blocks[#blocks+1] = { type = "reader_header", host = "example.com", readingTime = "2 min" }
  blocks[#blocks+1] = { type = "heading", level = 1, align = "center", inlines = { T("Title One") } }
  blocks[#blocks+1] = { type = "paragraph", align = "right",
    inlines = { T("Plain "), T("words "), T("here.") } }
  blocks[#blocks+1] = { type = "blockquote", inlines = { T("quoted words") } }
  blocks[#blocks+1] = { type = "list_item", isOrdered = true, number = 3, markerType = "a",
    inlines = { T("first item") } }
  blocks[#blocks+1] = { type = "list_item", depth = 2, inlines = { T("nested") } }
  blocks[#blocks+1] = { type = "list_item", dt = true, inlines = { T("term") } }
  blocks[#blocks+1] = { type = "list_item", dd = true, inlines = { T("definition") } }
  blocks[#blocks+1] = { type = "code_block", text = "line one\nline two\n\nline four" }
  blocks[#blocks+1] = { type = "hr" }
  blocks[#blocks+1] = { type = "image", width = 100, height = 50, src = "i.png", alt = "pic",
    align = "center", href = "http://i", usemap = "mymap" }
  blocks[#blocks+1] = { type = "table", caption = "tbl cap", border = true, width = "50%",
    rows = { {}, {} }, width = "300" }
  blocks[#blocks+1] = { type = "hidden_field", name = "hf", value = "v1", formAction = "http://f" }
  blocks[#blocks+1] = { type = "input_field", inputType = "text", name = "user", value = "me",
    placeholder = "type", fieldWidth = 10, formAction = "http://f/submit" }
  blocks[#blocks+1] = { type = "input_submit", label = "Go!", name = "s", value = "1",
    formAction = "http://f/submit" }
  blocks[#blocks+1] = { type = "checkbox_field", name = "cb", label = "Check me", checked = true }
  blocks[#blocks+1] = { type = "select_field", name = "sel", selectedIndex = 2,
    options = { { text = "one", value = "1" }, { text = "two", value = "2" } },
    formAction = "http://f/submit" }
  blocks[#blocks+1] = { type = "placeholder", width = 200, height = 60, label = "Video",
    href = "http://e" }
  blocks[#blocks+1] = { type = "meter", value = 40, min = 0, max = 100, label = "m" }
  blocks[#blocks+1] = { type = "box_open", label = "Details", toggleKey = "d1", toggleOpen = true }
  blocks[#blocks+1] = { type = "paragraph", inlines = { T("inner") } }
  blocks[#blocks+1] = { type = "box_close" }
  local doc = { blocks = blocks,
    maps = { mymap = { { shape = "rect", coords = { 10, 20, 110, 70 }, href = "http://m", alt = "mapa" } } } }
  Layout.build(doc)
  out[#out+1] = string.format("items=%d links=%d totalHeight=%d",
    #Layout.renderItems, #LinkManager.links, Layout.totalHeight)
  for _, it in ipairs(Layout.renderItems) do
    local ty = it.type
    if ty == "text" then
      out[#out+1] = string.format("  text=%q font=%s x=%d y=%d w=%d h=%d href=%s bold=%s italic=%s",
        it.text, tostring(it.font), it.x, it.y, it.w, it.h, tostring(it.href),
        (it.bold and "true" or "nil"), tostring(it.italic))
    elseif ty == "reader_badge" then
      out[#out+1] = string.format("  badge x=%d y=%d w=%d h=%d host=%s rt=%s",
        it.x, it.y, it.w, it.h, it.host, it.readingTime)
    elseif ty == "line" then
      out[#out+1] = string.format("  line x1=%d y1=%d x2=%d y2=%d", it.x1, it.y1, it.x2, it.y2)
    elseif ty == "quote_bar" then
      out[#out+1] = string.format("  quote_bar x=%d y1=%d y2=%d", it.x, it.y1, it.y2)
    elseif ty == "code_box" then
      out[#out+1] = string.format("  code_box x=%d y=%d w=%d h=%d nlines=%d line1=%q",
        it.x, it.y, it.w, it.h, #it.lines, it.lines[1])
    elseif ty == "image" then
      out[#out+1] = string.format("  image x=%d y=%d w=%d h=%d src=%s alt=%s href=%s",
        it.x, it.y, it.w, it.h, tostring(it.src), tostring(it.alt), tostring(it.href))
    elseif ty == "table_box" then
      out[#out+1] = string.format("  table_box x=%d y=%d w=%d h=%d cap=%s border=%s",
        it.x, it.y, it.w, it.h, tostring(it.caption), tostring(it.border))
    elseif ty == "hidden_field" then
      out[#out+1] = string.format("  hidden name=%s value=%s fa=%s disabled=%s",
        tostring(it.name), tostring(it.value), tostring(it.formAction), tostring(it.disabled))
    elseif ty == "input_field" then
      out[#out+1] = string.format("  input x=%d y=%d w=%d h=%d type=%s name=%s value=%s ph=%s maxlength=%s",
        it.x, it.y, it.w, it.h, tostring(it.inputType), tostring(it.name), tostring(it.value),
        tostring(it.placeholder), tostring(it.maxlength))
    elseif ty == "input_submit" then
      out[#out+1] = string.format("  submit x=%d y=%d w=%d h=%d label=%s",
        it.x, it.y, it.w, it.h, tostring(it.label))
    elseif ty == "checkbox_field" then
      out[#out+1] = string.format("  checkbox x=%d y=%d w=%d h=%d checked=%s label=%s",
        it.x, it.y, it.w, it.h, tostring(it.checked), tostring(it.label))
    elseif ty == "select_field" then
      out[#out+1] = string.format("  select x=%d y=%d w=%d h=%d name=%s sel=%d nopts=%d",
        it.x, it.y, it.w, it.h, tostring(it.name), it.selectedIndex, #it.options)
    elseif ty == "placeholder" then
      out[#out+1] = string.format("  placeholder x=%d y=%d w=%d h=%d label=%s",
        it.x, it.y, it.w, it.h, tostring(it.label))
    elseif ty == "meter" then
      out[#out+1] = string.format("  meter x=%d y=%d w=%d h=%d value=%s min=%s max=%s",
        it.x, it.y, it.w, it.h, tostring(it.value), tostring(it.min), tostring(it.max))
    elseif ty == "box_frame" then
      out[#out+1] = string.format("  box_frame x=%d y=%d w=%d y2=%d label=%s toggle=%s open=%s",
        it.x, it.y, it.w, it.y2, tostring(it.label), tostring(it.toggleKey), tostring(it.toggleOpen))
    else
      out[#out+1] = string.format("  UNHANDLED type=%s", tostring(ty))
    end
  end
  for _, l in ipairs(LinkManager.links) do
    out[#out+1] = string.format(
      "  link href=%s text=%s rect={%d,%d,%d,%d} anchor=%s isImage=%s isFormInput=%s isToggle=%s",
      tostring(l.href), tostring(l.text), l.rect.x, l.rect.y, l.rect.w, l.rect.h,
      tostring(l.anchor), tostring(l.rect.isImage), tostring(l.rect.isFormInput),
      tostring(l.rect.isToggle))
  end
  -- build2: percent-width table — the reference's table branch raises here
  -- (tonumber(gsub(width,"%","")) passes gsub's count as tonumber's base).
  -- Callers pcall Layout.build → "Layout Error" page. Capture as flag.
  local blocks2 = {
    { type = "paragraph", inlines = { T("before table") } },
    { type = "table", caption = "pct", border = true, width = "50%", rows = { {}, {} } },
    { type = "paragraph", inlines = { T("after table") } },
  }
  local ok2, err2 = pcall(function() Layout.build({ blocks = blocks2 }) end)
  out[#out+1] = string.format("build2 error=%s (%s)", tostring(not ok2),
                              tostring(err2):match("base out of range") and "percent-width" or "other")
else
  error("unknown vector " .. W)
end

local f = io.open(arg[2], "w")
f:write(table.concat(out, "\n") .. "\n")
f:close()
print("written " .. arg[2])
