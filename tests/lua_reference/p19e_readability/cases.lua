-- Shared readability test cases (Lua harness + C host battery).
return {
  { name = "tc1-strips-containers", baseUrl = "https://example.com/a/b",
    html = [[<html><head><style>p{color:red}</style><script>var x=1;</script></head><body>
<nav><a href="/home">Home</a> <a href="/about">About</a></nav>
<article><h1>Real Headline</h1><p>First paragraph of content.</p>
<svg><circle/></svg><iframe src="x"></iframe><noscript>no js</noscript>
<p>Second paragraph.</p></article>
<footer><p>Copyright junk</p></footer></body></html>]] },
  { name = "tc2-inline-state", baseUrl = "https://ex.com/x",
    html = [[<p>plain <b>bold</b> and <strong>strong</strong> <i>ital</i> <em>em</em> <code>mono</code> <kbd>kbd</kbd> end.</p>
<p>Link to <a href="https://en.wikipedia.org/wiki/Mesklin">https://en.wikipedia.org/wiki/Mesklin</a> hidden.</p>
<p>Link to <a href="/wiki/Mesklin">Mesklin page</a> shown.</p>
<pre>line1
  line2   kept</pre>
<textarea name="ta">ta content</textarea>
<button type="submit">ClickMe</button>]] },
  { name = "tc3-boundaries", baseUrl = "https://ex.com/",
    html = [[<h2>H2 title</h2><p>para one</p><div><p>in div</p></div><p>line<br/>break</p><hr/>
<ul><li>one</li><li>two</li></ul><ol><li>first</li><li>second</li><li>third</li></ol>
<blockquote><p>quoted text</p></blockquote><h6>deep heading</h6>]] },
  { name = "tc4-forms", baseUrl = "https://ex.com/search",
    html = [[<form action="/do" method="POST"><input type="search" name="q" placeholder="Find..." value="hi"/>
<input type="submit" value="Go!"/><input type="text" name="extra"/>
<input type="checkbox" name="c"/><input type="hidden" name="h" value="v"/>
<textarea name="msg"></textarea><button>Btn Label</button></form>]] },
  { name = "tc5-images", baseUrl = "https://ex.com/p/1",
    html = [[<img src="/a.png" width="800" height="400" alt="Big">
<img data-src="/b.png">
<img srcset="/c.png 1x, /c2.png 2x" title="Titled">
<img src="tracking-pixel.gif"><img src="/beacon.png">
<img src="/ok.png" alt="">]] },
  { name = "tc6-scoring", baseUrl = "https://ex.com/s",
    html = [[<nav><a href="/1">one</a><a href="/2">two</a><a href="/3">three</a><a href="/4">four</a><a href="/5">five</a></nav>
<article><p>This is the main content paragraph with many words of text and an image below.</p><img src="/main.png" alt="m"></article>
<p>Loose root paragraph too.</p>]] },
  { name = "tc7-merge-readtime", baseUrl = "https://wikipedia.org/wiki/x",
    html = [[<article><p>Welcome to Wikipedia,</p><p>the free encyclopedia that anyone can edit.</p>
<p>The end. And more sentences follow here to push reading time up slightly for the words count check.</p></article>]] },
  { name = "tc8-empty-fallback", baseUrl = "",
    html = [[<div>plain text only</div>]] },
}
