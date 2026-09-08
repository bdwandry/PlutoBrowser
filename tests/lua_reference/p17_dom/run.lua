import = function() end
Tasks = { yieldCheck = function() end, reportProgress = function() end }
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/entities.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/tokenizer.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/dom.lua")

local function dump(node, depth)
  local pad = string.rep(" ", depth * 2)
  if node.kind == "text" then
    print(pad .. "[text:" .. node.text .. "]")
  else
    local extra = ""
    if node.attrs and next(node.attrs) ~= nil then
      local ks = {}
      for k, v in pairs(node.attrs) do ks[#ks+1] = k .. "=" .. tostring(v) end
      table.sort(ks)
      extra = " {" .. table.concat(ks, ",") .. "}"
    end
    print(pad .. "<" .. node.tag .. ">" .. extra)
    for _, c in ipairs(node.children or {}) do dump(c, depth + 1) end
  end
end

local function run(name, html)
  local tokens, _ = Tokenizer.tokenize(html)
  local root = DOM.build(tokens)
  print("== " .. name .. " tokens=" .. #tokens)
  dump(root, 0)
  local d = root._diag
  print(string.format("diag: text=%d elem=%d proc=%d maxHit=%s skipDepth=%d",
    d.textNodes, d.elemNodes, d.tokensProcessed, tostring(d.maxNodesHit), d.skippedDepth))
end

run("tc1-nested-list", "<ul><li>A<ul><li>B<li>C</ul><li>D</ul>")
run("tc2-table", "<table><thead><tr><th>H1</th></tr></thead><tbody><tr><td>1</td><td>2</td></tr></tbody></table>")
run("tc3-stray", "<li>stray</li><option>s</option><tr><td>x</td></tr><dl><dt>t<dd>d</dl>")
run("tc4-pclose", "<p>one<p>two<div>d</div>")
run("tc5-template", "<template><p>hidden</p></template>after<ul><li>x</li></ul>")
run("tc5b-valve", "<template><p>never closed" .. string.rep("<i>f</i>", 400) .. "tail</ul><p>visible")
run("tc6-cap", string.rep("<div>x</div>", 4000))

-- extra probes: nested anchors + self-closing + void + attr survival
run("tc7-anchors", "<a href='1'>out<a href='2'>in</a>after</a>")
run("tc8-selfclose", "<div><mspace/>after<img src='i.png'>more</div>")
