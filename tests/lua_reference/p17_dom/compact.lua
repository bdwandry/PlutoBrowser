import = function() end
Tasks = { yieldCheck = function() end, reportProgress = function() end }
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/entities.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/tokenizer.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/dom.lua")

local function esc(s) return s end
local function ser(node, out)
  if node.kind == "text" then
    out[#out+1] = "[" .. esc(node.text) .. "]"
  else
    out[#out+1] = "<" .. node.tag
    local ks = {}
    if node.attrs then for k, v in pairs(node.attrs) do ks[#ks+1] = k end end
    table.sort(ks)
    if #ks > 0 then
      local parts = {}
      for _, k in ipairs(ks) do
        parts[#parts+1] = k .. "=" .. tostring(node.attrs[k])
      end
      out[#out+1] = "{" .. table.concat(parts, ",") .. "}"
    end
    out[#out+1] = ">"
    for _, c in ipairs(node.children or {}) do ser(c, out) end
  end
end
local function run(name, html)
  local tokens = Tokenizer.tokenize(html)
  local root = DOM.build(tokens)
  local out = {}
  ser(root, out)
  local s = table.concat(out)
  local d = root._diag
  print(string.format("%s|%d|%d|%d|%d|%d|%d|%s", name, #tokens, d.textNodes, d.elemNodes,
    d.tokensProcessed, d.maxNodesHit and 1 or 0, d.skippedDepth, s))
end
run("tc1", "<ul><li>A<ul><li>B<li>C</ul><li>D</ul>")
run("tc2", "<table><thead><tr><th>H1</th></tr></thead><tbody><tr><td>1</td><td>2</td></tr></tbody></table>")
run("tc3", "<li>stray</li><option>s</option><tr><td>x</td></tr><dl><dt>t<dd>d</dl>")
run("tc4", "<p>one<p>two<div>d</div>")
run("tc5", "<template><p>hidden</p></template>after<ul><li>x</li></ul>")
run("tc5b", "<template><p>never closed" .. string.rep("<i>f</i>", 400) .. "tail</ul><p>visible")
run("tc6", string.rep("<div>x</div>", 4000))
run("tc7", "<a href='1'>out<a href='2'>in</a>after</a>")
run("tc8", "<div><mspace/>after<img src='i.png'>more</div>")
