-- Authoritative capture of the P19 element walker (document.lua lines ~248-1443).
-- Runs the REAL CometBrowser pipeline: tokenizer -> DOM -> Document.parse (MODE_RAW_HTML)
-- with Tasks/SVGDecoder stubbed (SVG decode is pcall-guarded at runtime; we stub it
-- so svg blocks exercise the xml-serialize + decode-fail path).
-- Cases come from cases.lua (shared with the C input generator).
import = function() end
Tasks = { yieldCheck = function() end, reportProgress = function() end }
Constants = { MODE_READER = "reader", MODE_RAW_HTML = "html" }
SVGDecoder = { decode = function() return nil end }

local SRC = "/Users/bwandrych/Desktop/CometBrowser/Source/"
dofile(SRC .. "html/entities.lua")
dofile(SRC .. "html/tokenizer.lua")
dofile(SRC .. "html/dom.lua")
dofile(SRC .. "core/url.lua")
dofile(SRC .. "html/document.lua")

-- Escape embedded newlines/tabs in printed VALUES so each printed record
-- stays one physical line (only affects the test dump, not the pipeline).
local function esc(s)
  s = tostring(s)
  s = s:gsub("\\", "\\\\"):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
  return s
end

local function serInline(i)
  local p = i.type
  for _, k in ipairs({ "text", "bold", "italic", "underline", "code", "small",
                       "big", "sub", "sup", "mark", "strike", "invert", "href",
                       "anchorIndex", "inert" }) do
    local v = i[k]
    if v ~= nil and v ~= false then
      if type(v) == "boolean" and v == true then p = p .. "|" .. k .. "=t"
      else p = p .. "|" .. k .. "=" .. esc(v) end
    end
  end
  return p
end

local function serBlock(b)
  local p = "B " .. b.type
  for _, k in ipairs({ "level", "align", "spacingTop", "spacingBottom", "indent",
                       "invert", "isOrdered", "number", "markerType", "depth",
                       "dt", "dd", "src", "alt", "width", "height", "usemap",
                       "href", "inert", "label", "toggleKey", "toggleOpen",
                       "tag", "value", "max", "min", "low", "high", "optimum",
                       "radio", "checked", "name", "placeholder", "fieldWidth",
                       "fieldRows", "inputType", "disabled", "readonly",
                       "required", "maxlength", "formAction", "formMethod",
                       "selectedIndex", "multiple", "text", "caption", "border" }) do
    local v = b[k]
    if v ~= nil and v ~= false then
      if type(v) == "boolean" and v == true then p = p .. "|" .. k .. "=t"
      else p = p .. "|" .. k .. "=" .. esc(v) end
    end
  end
  if b.inlines then
    for _, i in ipairs(b.inlines) do p = p .. " {" .. serInline(i) .. "}" end
  end
  if b.lines then
    for _, l in ipairs(b.lines) do p = p .. " [L:" .. esc(l) .. "]" end
  end
  if b.options then
    for j, o in ipairs(b.options) do
      p = p .. " opt" .. j .. ":"
      for _, k in ipairs({ "text", "value", "group", "selected", "disabled" }) do
        local v = o[k]
        if v ~= nil and v ~= false then
          if type(v) == "boolean" and v == true then p = p .. k .. "=t,"
          else p = p .. k .. "=" .. esc(v) .. "," end
        end
      end
    end
  end
  return p
end

local function run(case)
  local name = case.name
  local html = case.html
  local doc = Document.parse(html, case.baseUrl or "https://example.com/",
                             Constants.MODE_RAW_HTML, case.detailsOpen and { detailsOpen = case.detailsOpen } or nil)
  print("== " .. name)
  print("title=" .. esc(doc.title) .. "|base=" .. esc(doc.baseUrl) .. "|reader=" .. tostring(doc.isReaderMode))
  for _, b in ipairs(doc.blocks) do print(serBlock(b)) end
  print("--links")
  for i, l in ipairs(doc.links) do
    print("L" .. i .. "|href=" .. esc(l.href) .. "|text=" .. esc(l.text) .. "|target=" .. esc(l.target or ""))
  end
  print("--maps")
  local ks = {}
  for k in pairs(doc.maps or {}) do ks[#ks + 1] = k end
  table.sort(ks)
  for _, k in ipairs(ks) do
    local rs = doc.maps[k]
    print("M|" .. esc(k))
    for j, r in ipairs(rs) do
      print("  area" .. j .. "|shape=" .. esc(r.shape) .. "|coords=" .. table.concat(r.coords, ",") ..
            "|href=" .. esc(r.href or "") .. "|alt=" .. esc(r.alt))
    end
  end
  print("--end " .. name)
end

local cases = dofile("tests/lua_reference/p19_walker/cases.lua")
for _, c in ipairs(cases) do
  if c.expectThrow then
    -- Reference throws on bare <li> (document.lua:797 nil ctx.start). Capture
    -- as a guarded probe so the C battery can assert the parse-error flag.
    print("== " .. c.name)
    local ok, err = pcall(function()
      Document.parse(c.html, c.baseUrl or "https://example.com/", Constants.MODE_RAW_HTML)
    end)
    print(ok and "NO-ERROR (unexpected)" or ("ERROR: " .. tostring(err):gsub("%s+", " "):sub(1, 140)))
    print("--end " .. c.name)
  else
    run(c)
  end
end