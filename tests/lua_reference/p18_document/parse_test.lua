import = function() end
Tasks = { yieldCheck = function() end, reportProgress = function() end }
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/core/constants.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/core/url.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/entities.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/tokenizer.lua")
dofile("/Users/bwandrych/Desktop/CometBrowser/Source/html/dom.lua")

-- Extract just the parse-skeleton part: empty path, base href, meta refresh
local function scanSkel(htmlString, baseUrl)
  if not htmlString or htmlString == "" then
    return "EMPTY|title=Blank Page|base=" .. tostring(baseUrl or "about:blank")
  end
  local tokens, extractedTitle = Tokenizer.tokenize(htmlString)
  local pageTitle = extractedTitle or "Web Page"
  local baseHref = nil
  for _, tok in ipairs(tokens) do
    if tok.type == "tag" and tok.name == "base" and not tok.isClosing then
      baseHref = (tok.attrs and tok.attrs["href"]) or ""
      if baseHref ~= "" then break end
    end
  end
  if baseHref and string.match(baseHref, "^[a-zA-Z][%w+%-%.]*://") then
    baseUrl = baseHref
  end
  local metaRefresh = nil
  for _, tok in ipairs(tokens) do
    if tok.type == "tag" and tok.name == "meta" and not tok.isClosing then
      local httpEquiv = tok.attrs and tok.attrs["http-equiv"]
      if httpEquiv and string.lower(httpEquiv) == "refresh" then
        local content = tok.attrs["content"] or ""
        local delayStr, urlStr = string.match(content, "^(%d+%.?%d*)%s*;%s*[Uu][Rr][Ll]=%s*(.+)$")
        if not delayStr then
          delayStr = string.match(content, "^(%d+%.?%d*)%s*$")
        end
        if delayStr then
          local refreshUrl = urlStr and string.match(urlStr, "^%s*(.-)%s*$") or nil
          if refreshUrl and refreshUrl ~= "" then
            refreshUrl = URL.resolve(baseUrl, refreshUrl)
          else
            refreshUrl = nil
          end
          metaRefresh = {
            delay = tonumber(delayStr) or 0,
            url = refreshUrl,
          }
          break
        end
      end
    end
  end
  local mr = "nil"
  if metaRefresh then
    mr = string.format("{delay=%s,url=%s}", tostring(metaRefresh.delay), tostring(metaRefresh.url))
  end
  return string.format("OK|title=%s|base=%s|meta=%s", pageTitle, tostring(baseUrl), mr)
end
print(scanSkel(nil, "https://x.com/"))
print(scanSkel("", "https://x.com/"))
print(scanSkel("<html><title>My Page</title><body>hi</body></html>", "https://x.com/"))
print(scanSkel("<base href='/docs/'>", "https://x.com/"))
print(scanSkel("<base href='/docs/'>", "https://x.com/page.html"))
print(scanSkel("<base href='https://cdn.example.com/abs/'>", "https://x.com/page.html"))
print(scanSkel("<meta http-equiv='refresh' content='5; url=/next.html'>", "https://x.com/start"))
print(scanSkel("<meta HTTP-EQUIV='REFRESH' content='0; URL=https://a.b/c?d=1'>", "https://x.com/"))
print(scanSkel("<meta http-equiv='refresh' content='10'>", "https://x.com/"))
print(scanSkel("<meta http-equiv='refresh' content='2.5;url=rel/page'>", "https://x.com/dir/"))
print(scanSkel("<meta http-equiv='refresh' content='junk'>", "https://x.com/"))
print(scanSkel("<base href='https://b.com/'><meta http-equiv='refresh' content='1;url=x'>", "https://x.com/"))
print(scanSkel("<meta http-equiv='refresh' content='3; url=a'><meta http-equiv='refresh' content='9; url=b'>", "https://x.com/"))
