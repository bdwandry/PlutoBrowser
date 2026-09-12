import = function() end
Tasks = { yieldCheck = function() end, reportProgress = function() end }
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/core/url.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/entities.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/tokenizer.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/dom.lua")

-- Verbatim from document.lua
local function concatNodeText(node)
    local parts = {}
    local function rec(n)
        if n.kind == "text" then
            table.insert(parts, n.text or "")
        elseif n.kind == "element" then
            for _, c in ipairs(n.children) do rec(c) end
        end
    end
    rec(node)
    return table.concat(parts)
end
local function validHref(raw)
    if not raw or raw == "" then return false end
    if string.match(raw, "^#") then return false end
    if string.match(raw, "^[Jj][Aa][Vv][Aa][Ss][Cc][Rr][Ii][Pp][Tt]:") then return false end
    if string.match(raw, "^[Dd][Aa][Tt][Aa]:") then return false end
    return true
end
local function serializeSvgNode(n)
    if n.kind == "text" then
        return Entities.encode(n.text or "")
    end
    if n.kind == "element" then
        local parts = { "<", n.tag }
        for k, v in pairs(n.attrs or {}) do
            local vv = string.gsub(v or "", '["<]', function(c)
                return (c == '"') and "&quot;" or "&lt;"
            end)
            parts[#parts + 1] = " " .. k .. '="' .. vv .. '"'
        end
        local children = n.children or {}
        if #children == 0 then
            parts[#parts + 1] = "/>"
        else
            parts[#parts + 1] = ">"
            for _, c in ipairs(children) do
                parts[#parts + 1] = serializeSvgNode(c)
            end
            parts[#parts + 1] = "</" .. n.tag .. ">"
        end
        return table.concat(parts)
    end
    return ""
end

local function findTag(node, tag)
    if node.kind == "element" and node.tag == tag then return node end
    for _, c in ipairs(node.children or {}) do
        local f = findTag(c, tag)
        if f then return f end
    end
    return nil
end

local function t(name, html, tag)
    local root = DOM.build(Tokenizer.tokenize(html))
    local n = tag and findTag(root, tag) or root
    print(name .. "|" .. concatNodeText(n))
end
t("tc6a", "<div><p>one</p><span>two<b>three</b></span></div>", "div")
t("tc6b", "<ul><li>x<li>y</ul>", "ul")
t("tc6c", "<img src='x.png'>", nil)
t("tc6d", "<p>a &amp; b &lt;c&gt;</p>", "p")
print("tc7a|" .. tostring(validHref("https://a.com/x")))
print("tc7b|" .. tostring(validHref("")))
print("tc7c|" .. tostring(validHref(nil)))
print("tc7d|" .. tostring(validHref("#top")))
print("tc7e|" .. tostring(validHref("JavaScript:void(0)")))
print("tc7f|" .. tostring(validHref("data:image/png;base64,AA")))
print("tc7g|" .. tostring(validHref("/relative/path")))
print("tc7h|" .. tostring(validHref("javascripty.com/x")))
local svg1 = DOM.build(Tokenizer.tokenize("<svg id='x' width='10'><circle r='1'/><text>A<b>B</b></text></svg>"))
print("tc8a|" .. serializeSvgNode(findTag(svg1, "svg")))
local svg2 = DOM.build(Tokenizer.tokenize("<svg><text>1 &lt; 2 &amp; &quot;q&quot;</text></svg>"))
print("tc8b|" .. serializeSvgNode(findTag(svg2, "svg")))
local svg3 = DOM.build(Tokenizer.tokenize("<svg><g><rect w='2'></rect><brk></g></svg>"))
print("tc8c|" .. serializeSvgNode(findTag(svg3, "svg")))
