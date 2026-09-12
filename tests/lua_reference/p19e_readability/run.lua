-- Authoritative capture of Readability.distill (readability.lua, verbatim).
-- Usage: lua run.lua <dummy> <outfile>
import = function() end
Tasks = { yieldCheck = function() end, reportProgress = function() end }

local SRC = "/Users/bwandrych/Desktop/CometBrowser/Source/"
dofile(SRC .. "html/entities.lua")
dofile(SRC .. "html/tokenizer.lua")
dofile(SRC .. "core/url.lua")
dofile(SRC .. "html/readability.lua")

local cases = dofile("/Users/bwandrych/Desktop/PlutoBrowser/tests/lua_reference/p19e_readability/cases.lua")

local function esc(s)
  s = tostring(s)
  s = s:gsub("\\", "\\\\"):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
  return s
end

local function serInline(i)
  local p = i.type
  for _, k in ipairs({ "text", "bold", "italic", "underline", "code", "href" }) do
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
  for _, k in ipairs({ "level", "isOrdered", "number", "src", "alt", "width",
                       "height", "href", "label", "inputType", "name", "value",
                       "placeholder", "formAction", "formMethod", "host",
                       "title", "readingTime", "text" }) do
    local v = b[k]
    if v ~= nil and v ~= false then
      p = p .. "|" .. k .. "=" .. esc(v)
    end
  end
  if b.inlines then
    for _, i in ipairs(b.inlines) do p = p .. " {" .. serInline(i) .. "}" end
  end
  return p
end

local out = {}
local function emit(s) out[#out + 1] = s end

for _, case in ipairs(cases) do
  local tokens = Tokenizer.tokenize(case.html)
  local doc = Readability.distill(tokens, "Web Page", case.baseUrl)
  emit("== " .. case.name)
  emit("title=" .. esc(doc.title) .. "|base=" .. esc(doc.baseUrl) ..
       "|reader=" .. tostring(doc.isReaderMode) ..
       "|words=" .. tostring(doc.wordCount) .. "|time=" .. esc(doc.readingTime))
  for _, b in ipairs(doc.blocks) do emit(serBlock(b)) end
end

local f = io.open(arg[2], "w")
f:write(table.concat(out, "\n") .. "\n")
f:close()
print("wrote " .. arg[2])
