# PlutoBrowser — Master TODO (C Port of CometBrowser)

**Status: PLANNING COMPLETE — PHASES P01–P09 DONE (verified in simulator). NEXT: P10.**
This document is the single source of truth for the port. Every execution session performs
EXACTLY ONE phase, then updates this file and STOPS.

**Phase progress:** P01 ✅ P02 ✅ P03 ✅ P04 ✅ P05 ✅ P06 ✅ P07 ✅ P08 ✅ P09 ✅ | P10–P36 ⬜

---

## 0. Mission

Re-implement **CometBrowser** (`/Users/bwandrych/Desktop/CometBrowser`, pure Lua,
~14,711 lines across 38 `.lua` files) as a **100% C** Playdate application named
**PlutoBrowser** at `/Users/bwandrych/Desktop/PlutoBrowser`.

- Source of truth: the CometBrowser Lua code. Nothing may be removed, simplified,
  stubbed, approximated, downgraded, or omitted. Behavior parity is mandatory.
- Internal algorithms, memory layout, rendering strategy, and networking internals MAY be
  redesigned/optimized for Playdate hardware as long as observable behavior matches.
- Final app must contain **no Lua runtime, no Lua bridge, no Lua fallback, no hybrid code**.
- Every Lua file gets at least one corresponding C file (see mapping in §3). Extra C/H files
  are allowed and expected where helpful.

## 1. Hard Rules (apply to every phase)

1. **One phase per execution.** Never auto-continue into the next phase.
2. Every phase MUST end with:
   - Clean build with the Playdate SDK (`make` from `/Users/bwandrych/Desktop/PlutoBrowser`).
   - Fix all compiler/linker errors before proceeding.
   - Launch the Playdate Simulator and actually test any executable functionality.
   - Diagnostic logging written to a text log file (see §4.9). Logs are NEVER removed until
     the user explicitly authorizes removal in the final cleanup phase.
   - Copy/append the phase's log to `logs/phase-XX.log` inside PlutoBrowser (text files kept
     in repo).
   - Update this MASTER_TODO: mark only work that was actually verified complete; list any
     bugs/blockers under the phase entry.
   - Report exact stop point, then STOP completely.
3. If a timeout is imminent: stop at a safe checkpoint, preserve all work, update this TODO,
   report the exact stop point.
4. Edit source files only. Never edit generated `.pdx` bundles.
5. Web/rendering/networking phases benchmark against
   `https://wiesmann.codiferes.net/share/bitmaps/` (temporary benchmark target; the normal
   home page is restored only in final cleanup after explicit authorization).
6. The on-screen keyboard MUST be the C implementation from
   `https://github.com/Raphcal/some-corelibs-port/tree/main/keyboard` (no Lua keyboard, no bridge).
7. "Compiles" ≠ "complete". A phase is done only when its verification checklist passes in
   the simulator with logs proving it.

## 2. CometBrowser Source Inventory (verified)

LOC via `wc -l`; public API via function scan. All paths relative to `Source/`.

| Lua file | LOC | Public API (exported functions) |
|---|---|---|
| main.lua | 1035 | playdate.update; locals: pushHistory, goBack, goForward, openKeyboardForInput, submitForm, activateFormBlock, getActiveTotalHeight, updateFrame; system menu handlers |
| core/constants.lua | ~90 | Constants.* (geometry, states, modes, engines, image modes, defaults) |
| core/logger.lua | 55 | Logger.init/log/error |
| core/url.lua | 245 | URL.encode/decode/isSearchQuery/parse/unwrapRedirect/resolve/buildSearchUrl |
| core/storage.lua | 131 | Storage.init/save/addHistory/addBookmark/removeBookmark/isBookmarked |
| core/http_client.lua | 538 | HttpClient.get/update/cancel/isLoading; locals closeTcp/reset/buildRequest/decodeChunked/parseHeaders/doGet |
| core/tasks.lua | ~95 | Tasks.yieldCheck/reportProgress/getProgress/run/isRunning/cancelAll/scheduleGC/update |
| core/cookie_jar.lua | 268 | CookieJar.parseSetCookie/store/processSetCookies/getHeader/prune/clear/count |
| core/encoding.lua | 184 | Encoding.toUtf8 |
| html/tokenizer.lua | 210 | Tokenizer.tokenize |
| html/entities.lua | 240 | Entities.decode/encode |
| html/dom.lua | 252 | DOM.build |
| html/document.lua | 1488 | Document.parse(htmlString, baseUrl, mode, opts) |
| html/readability.lua | 535 | Readability.distill(tokens, rawTitle, baseUrl) |
| render/style.lua | ~90 | Style.init/getTextWidth/getHeadingFont/getBodyFont/getInlineFont |
| render/layout.lua | 1472 | Layout.build/draw/evictOffscreen/evictHoveredImage/showOnDemandOverlay/clearOnDemandOverlay/drawOnDemandOverlay/handleOnDemandInput; locals normalizeAlign/toRoman/toAlpha/orderedMarker/tabAdvance/expandTabColumns/breakLines/emitFlow |
| render/cloud_layout.lua | 152 | CloudLayout.parse/build/draw |
| render/link_manager.lua | 181 | LinkManager.clear/clearSelection/addLinkRect/getCount/getSelectedLink/selectNext/selectPrev/drawSelectedHighlight/isHighlighted/addLink/getHoveredLink |
| render/image_decoder.lua | 308 | ImageDecoder.clearCache/update/enqueue/evict/isCached/isDecoded/getImage/draw/_testDecode; local decodeRawImageData/processNextImage |
| render/decoders/dither.lua | ~40 | Dither.rgbToGray/toImage |
| render/decoders/scale.lua | ~120 | Scale.boxSizes/newAccum (+addRow/finish) |
| render/decoders/inflate.lua | 427 | Inflate.decompress/createStream |
| render/decoders/bmp.lua | ~150 | BMPDecoder.decode |
| render/decoders/gif.lua | 293 | GIFDecoder.decode |
| render/decoders/png.lua | 270 | PNGDecoder.decode |
| render/decoders/jpeg.lua | 670 | JPEGDecoder.decode |
| render/decoders/webp.lua | 3052 | WebPDecoder.decode/decodeAnimation/_testDecodeRaw/_testBuildTable |
| render/decoders/webp_vp8_data.lua | 348 | VP8Tables (kDcTable, kAcTable, kZigzag, kCat3456, kBands, kScan, kYModesIntra4, kBModesProba, CoeffsProba0, CoeffsUpdateProba) |
| render/decoders/ico.lua | 185 | ICODecoder.decode |
| render/decoders/svg.lua | 451 | SVGDecoder.decode |
| ui/chrome.lua | 103 | Chrome.draw |
| ui/address_bar.lua | 148 | AddressBar.open/launchKeyboard/cancel/drawOverlay |
| ui/home_page.lua | 278 | HomePage.reset/handleInput/draw |
| ui/hud.lua | 76 | Hud.draw/drawHoverStatus |
| ui/error_page.lua | 94 | ErrorPage.show/handleInput/draw |
| ui/bookmarks_page.lua | 101 | BookmarksPage.open/handleInput/draw |
| ui/history_page.lua | 101 | HistoryPage.open/handleInput/draw |
| ui/settings_page.lua | 278 | SettingsPage.open/close/saveAndClose/cancelAndClose/handleInput/draw |

Assets to copy verbatim: `fonts/*.fnt` (8 Roobert fonts), `images/home_banner.png`,
`icon.png`, `assets/launcher/*`. `replace.py` is empty → ignore.

## 3. Target C Project Layout & File Mapping

```
PlutoBrowser/
├── MASTER_TODO.md            # this file
├── Makefile                  # Playdate SDK C build (arm-gcc + pdx), sim target
├── logs/                     # per-phase exported text logs (never delete)
├── vendor/keyboard/          # Raphcal some-corelibs-port keyboard (C) — vendored
└── src/
    ├── main.c/h              # ← main.lua (state machine, input, menu, forms, loop)
    ├── pdxinfo               # name=PlutoBrowser, bundleID=com.bryanwandrych.plutobrowser
    ├── core/
    │   ├── constants.c/h     # ← core/constants.lua
    │   ├── logger.c/h        # ← core/logger.lua  (pluto.log, append-per-line)
    │   ├── url.c/h           # ← core/url.lua
    │   ├── storage.c/h       # ← core/storage.lua (datastore parity)
    │   ├── http_client.c/h   # ← core/http_client.lua (playdate->network->tcp)
    │   ├── tasks.c/h         # ← core/tasks.lua (cooperative scheduler)
    │   ├── cookie_jar.c/h    # ← core/cookie_jar.lua
    │   └── encoding.c/h      # ← core/encoding.lua
    ├── util/
    │   ├── strbuf.c/h        # growable byte strings (string concat replacement)
    │   ├── strmap.c/h        # string→ptr hash map (Lua table keyed by string)
    │   ├── dynarray.c/h      # typed dynamic arrays (Lua sequences)
    │   ├── luapattern.c/h    # Lua-pattern subset matcher (gmatch/find/sub semantics)
    │   └── json.c/h          # minimal JSON parser for CloudLayout + storage if needed
    ├── html/
    │   ├── tokenizer.c/h     # ← html/tokenizer.lua
    │   ├── entities.c/h      # ← html/entities.lua
    │   ├── dom.c/h           # ← html/dom.lua
    │   ├── document.c/h      # ← html/document.lua
    │   └── readability.c/h   # ← html/readability.lua
    ├── render/
    │   ├── style.c/h         # ← render/style.lua
    │   ├── layout.c/h        # ← render/layout.lua
    │   ├── cloud_layout.c/h  # ← render/cloud_layout.lua
    │   ├── link_manager.c/h  # ← render/link_manager.lua
    │   ├── image_decoder.c/h # ← render/image_decoder.lua
    │   └── decoders/
    │       ├── dither.c/h    ├── scale.c/h      ├── inflate.c/h
    │       ├── bmp.c/h       ├── gif.c/h        ├── png.c/h
    │       ├── jpeg.c/h      ├── webp.c/h       ├── webp_vp8_data.c/h
    │       ├── ico.c/h       └── svg.c/h
    └── ui/
        ├── chrome.c/h        ├── address_bar.c/h ├── home_page.c/h
        ├── hud.c/h           ├── error_page.c/h  ├── bookmarks_page.c/h
        ├── history_page.c/h  └── settings_page.c/h
```

## 4. Cross-Cutting Engineering Notes (read before every phase)

### 4.1 Lua→C semantic traps
- Lua strings are byte strings with NUL-safe lengths → use `const char*` + `size_t`
  everywhere; never assume C-string termination for network/HTML data.
- 1-based indexing in tables → keep indices consistent per module; document each choice.
- `math.floor` on negatives, integer division truncation toward zero in C vs floor in Lua:
  audit every division/modulo during port.
- Lua 5.4 bitwise ops on 64-bit ints; webp/gif/png rely on masks like `& 0xFFFFFFFF` →
  use `uint32_t`/`int32_t` explicitly; replicate sign-extension behavior of `>>` on
  negatives via arithmetic shift helpers (`sar()` exists in webp.lua).
- Lua patterns (`%a`, `%d`, `%s`, `%w`, `-` lazy, sets `[...]`, captures) are NOT POSIX
  regex → implement the small subset actually used in `util/luapattern.c` OR hand-write
  equivalent parsers per call site. Semantics must match exactly (e.g., URL.parse uses
  `string.match(url, "^(%a+)://...")` style patterns).
- `tonumber`, `tostring`, `string.format("%02d:%02d")`, `string.sub` with negative indices:
  provide tiny helpers replicating behavior.
- pcall error containment → every decoder entry point gets an internal error path returning
  NULL + reason logged; a decode failure must never crash the app (parity with pcall).

### 4.2 Coroutines → cooperative state machines
`Tasks.run` wraps coroutines; decoders call `Tasks.yieldCheck()` mid-loop. In C:
- Each long-running job becomes a context struct with a `step()` function returning
  `TASK_YIELD` / `TASK_DONE` / `TASK_ERROR`, driven from `Tasks_update()` within the same
  frame budget rules (FRAME_BUDGET_MS=500, CHECK_EVERY=64 iteration counter).
- Yield points must occur at the SAME logical places as the Lua `yieldCheck()` calls
  (e.g., JPEG between MCU rows, PNG between rows, GIF between LZW chunks, WebP between
  macroblock rows / between scaled output rows).
- Progress reporting (`Tasks.reportProgress`) preserved identically (drives chrome progress bar).

### 4.3 Memory strategy
- Preallocated pools/arenas per page load for DOM/blocks/items (caps already exist:
  MAX_NODES=6000, MAX_BLOCKS=1200, MAX_INLINES=900, MAX_HTML_SIZE=256KB, MAX_RESPONSE_SIZE=2MB,
  MAX_COOKIES=300, MAX_HISTORY=30, image cache caps). Enforce identical caps.
- Image cache eviction semantics must match ImageDecoder.evict + Layout.evictOffscreen
  (200px buffer) + hover/on-demand modes.

### 4.4 Playdate C API mapping (gfx)
fillRect/drawRect/fillRoundRect/drawRoundRect/drawLine/drawCircleAtPoint/fillCircleAtPoint/
drawEllipse/setLineWidth/setColor/setImageDrawMode(kDrawModeCopy|FillWhite)/setClipRect/
clearClipRect/pushContext/popContext(→ bitmap targets)/drawText via LCDFonts loaded from the
copied .fnt files/font:getHeight/getTextWidth equivalents (`playdate->graphics->getTextWidth`).
Images: `LCDBitmap*` created via `newBitmap`, drawn with `drawBitmap`; 1-bit pixel setting via
`getBitmapData` + direct framebuffer writes for Dither.toImage performance.

### 4.5 Datastore parity
Storage uses `playdate.datastore` (Lua table serialized to `comet_browser_data`).
C port: serialize the same logical structure (bookmarks/history/cookies/settings) using
SDK JSON (`playdate->json`) to a data file of the same name so user data survives/migrates;
preserve default values (DEFAULT_BOOKMARKS list, settings defaults searchEngine=1,
mode=reader, autoReader, fontSize, imageMode=all, invertCrank=false).

### 4.6 Networking
Use `playdate->network->tcp` (open/openConnected/read/write/close + callbacks). Port the raw
HTTP/1.1 client exactly: manual request build, header parse, chunked decoding, redirect
chain ≤5 deferred one tick, 60s timeout, 2MB cap, requestId staleness guard, Set-Cookie
harvest, Content-Type charset propagation, internal error pages. NOTE: SDK native
`playdate->network->http` is NOT used (crashes on 3xx) — keep raw TCP approach.

### 4.7 Keyboard (vendored C)
Fetch `https://github.com/Raphcal/some-corelibs-port/tree/main/keyboard` into
`vendor/keyboard/`, adapt includes/build, wire callbacks to AddressBar semantics:
show(initialText)/hide(), textChangedCallback, keyboardWillHideCallback(submitted),
keyboardDidHideCallback. B-release launch gating and skipInputFrames=2 behavior preserved.

### 4.8 JSON
CloudLayout needs `json.decode`; use SDK json or `util/json.c`. Storage may reuse it.

### 4.9 Logging (permanent until authorized)
`core/logger.c`: mirrors Logger.init/log/error — opens `pluto.log` append per line with
timestamp; Logger.error adds location info (file:line instead of Lua where()). Add
PLUTO_LOG(level, fmt, ...) used liberally in every module (network bytes/state transitions,
decode stages/timings, layout item counts, input events, state changes). Never strip these
calls; final cleanup removes them ONLY with explicit user authorization.

### 4.10 Benchmark protocol
For P07 and every decoder phase and integration: fetch test assets from
`https://wiesmann.codiferes.net/share/bitmaps/`, log: URL, byte size, download ms, decode ms,
output dimensions, success/failure. Store results in `logs/phase-XX.log`.

---

## 5. Phase Plan

Legend: [x] done+verified, [~] partial (note stop point), [ ] not started.
Each phase ends with: clean `make`, simulator run, logs exported, TODO updated, STOP.

### PHASE P01 — Scaffolding, build system, logger, app skeleton  ✅ DONE (verified in simulator)
- [x] Create PlutoBrowser tree per §3; copy fonts/, images/, icon.png, assets/launcher/.
      NOTE: build layout follows the SDK convention — C sources in `src/`, pdc input
      (pdxinfo + assets + built pdex.elf/pdex.dylib) in `Source/` (SDK common.mk hardcodes
      `Source` as the pdc input dir). Same split as the official "Hello World" C example.
- [x] `Source/pdxinfo`: name=PlutoBrowser, author=Bryan Wandrych,
      bundleID=com.bryanwandrych.plutobrowser, version=1.0.0, buildNumber=1,
      imagePath=assets/launcher.
- [x] Makefile: HEAP_SIZE=8388208 STACK_SIZE=61800, SRC list + VPATH (src:src/core:...),
      includes $(SDK)/C_API/buildsupport/common.mk (device arm-gcc pdex.elf + simulator
      clang pdex.dylib + pdc packaging); extra `sim` target opens Playdate Simulator.app.
      SDK fallback /Users/bwandrych/Developer/PlaydateSDK honored.
- [x] `src/main.c`: eventHandler (kEventInit/kEventTerminate), setUpdateCallback loop,
      placeholder screen ("PlutoBrowser" + frame counter) using Roobert-11-Medium.
      C-API notes learned: no setTextColor/setColor for text — black text via default
      kDrawModeCopy; white text later via setDrawMode(kDrawModeFillWhite); shape calls
      take LCDColor as a parameter (fillRect(x,y,w,h,color) etc.).
- [x] `src/core/constants.c/h`: full port of constants.lua (geometry macros PLUTO_*,
      PlutoState/PlutoMode enums + exact-string name functions, SEARCH_ENGINES ×4 verbatim,
      DEFAULT_BOOKMARKS ×9 verbatim, USER_AGENT verbatim, image-mode enums + NAMES order +
      LABELS map).
- [x] `src/core/logger.c/h`: pluto.log, truncate-on-init banner + gamePath line
      ("unknown": C API has no getPath; matches Lua pcall fallback), per-line open/append/
      close "[HH:MM:SS #seq] msg", ERROR prefix + file:line stack equivalent via
      PLUTO_ERROR macro. Timestamps via getSecondsSinceEpoch + convertEpochToDateTime.
- [x] Verify: `make clean && make` → zero errors/warnings (device elf + sim dylib + pdc).
      Simulator launched (`make sim`): game boots, font loads, update loop runs at ~30fps
      (heartbeat every 300 frames ≈ 10s), pluto.log written to
      SDK/Disk/Data/com.bryanwandrych.plutobrowser/pluto.log. Log exported to
      logs/phase-P01.log.

### PHASE P02 — util layer (strbuf, dynarray, strmap, luapattern, json) ✅ DONE
- [x] Implement §3/util modules with unit smoke tests runnable in-sim via a temporary debug
      draw screen (log PASS/FAIL lines to pluto.log).
      Delivered: mem.c/h (allocator wrapper via pd->system->realloc + libc fallback),
      strbuf.c/h (growable byte strings, embedded-NUL safe), dynarray.c/h,
      strmap.c/h (FNV-1a chained hash, key copies), luapattern.c/h, json.c/h
      (parser+writer+builders). Self-tests: 128 assertions run at boot
      (selftest_util.c), summary drawn on placeholder screen.
- [x] luapattern: support exactly the constructs used across the codebase (character classes
      %a %d %s %w %p, sets, anchors ^ $, quantifiers * + - ?, captures (), lazy `-`);
      APIs: lmatch, lgmatch(iterate), lsub-style helpers. Validate against tricky cases found
      in url.lua/tokenizer/svg (e.g. `'([%w%:-]+)%s*=%s*"([^"]*)"'`,
      `"^%s*(.-)%s*$"`, `"([a-zA-Z])%s*([^a-zA-Z]*)"`).
      Delivered: full lstrlib port incl %b/%f/position-captures; gsub with %0-%9 templates
      AND function callbacks; find/find_plain/match/gmatch/gsub; NUL-safe explicit lengths.
      Semantics verified against host Lua 5.5 ground truth — including the quirk that a
      quantifier after ')' is an optional literal char (readability.lua:36 domain pattern
      only matches strings ending in '?'), css-prop keys keep trailing space, and gmatch
      skips '</div>'-style closers for `<([%w%:-]+)([^>]*)>`.
      Engine bugs found & fixed during verification: bracket-range comparison offsets,
      %b bounds off-by-one, lp_find/lp_match whole-match start offset, %N template
      capture index off-by-one; JSON sign-dropped-by-parser bug.
- [x] Verify: pattern self-tests all PASS in log; make clean build; STOP.
      Verified: make clean && make = 0 errors / 0 warnings; simulator run:
      "128 passed, 0 failed" (logs/phase-P02.log).

### PHASE P03 — core/url.c (full URL.lua port)
- [x] encode/decode (%XX, '+'→space), isSearchQuery heuristics (exact thresholds/order),
      parse → {scheme,host,port,path,query,hash,isSsl,normalized} incl. default ports,
      lowercase host, empty-path→"/", unwrapRedirect (DDG /l/ uddg= only — mirrors source),
      resolve(baseUrl, rel) handling scheme-relative, root-relative, query-only, hash-only,
      ../ normalization; buildSearchUrl(engineUrlTemplate, query) encoding.
- [x] Self-test suite: 70 assertions with ground truth captured from the REAL url.lua
      under host Lua (parity oracle): 14 parse fixtures (about:, userinfo colon quirk,
      ":0"/":" ports, case folding), 8 encode / 6 decode / roundtrip, 16 isSearchQuery
      ("   "→query, "HTTP://X.COM"→URL via dot rule), 3 unwrapRedirect, 19 resolve
      (dotdot clamping at root, mailto: NOT special, uppercase absolute passthrough).
- [x] Verify: tests pass in log; STOP.
      Verified: make clean && make = 0 errors / 0 warnings; simulator run:
      "[P03] url selftests done: 70 passed, 0 failed" (logs/phase-P03.log);
      fixes en route: space→'+' post-pass in url_encode; locale-free ascii_isalnum
      (sim ctype accepted byte 0xC3 as alnum); Lua decode(encode(x)) not identity
      for literal '+' (documented + test adjusted).

### PHASE P04 — core/storage.c + cookie_jar.c  ✅ DONE (verified in simulator)
- [x] Storage: load-or-default datastore file `comet_browser_data.json` (JSON via util/json);
      addHistory (skip NULL/""/^about:, dedupe-by-url move-to-front scanning from END,
      title=title-or-url, insert front, cap 50 dropping TAIL, time "%02d:%02d" via getTime
      with pcall-style fallback "Recent"), addBookmark (dup updates TITLE ONLY keeping desc),
      removeBookmark (1-based bounds), isBookmarked, save() (full JSON round-trip incl.
      cookies + settings; empty bookmarks array on load → defaults restored, Lua parity).
      Delivered storage_data.h (PlutoSavedBookmark/PlutoHistoryItem/PlutoCookie/PlutoSettings).
- [x] CookieJar: trim, makeTimestamp (Howard Hinnant days_from_civil; month/day/year clamps,
      fractional y/m/d floored but hour NOT floored — Lua quirk preserved), parseDate
      (IMF-fixdate/RFC850 yy<70→+2000/asctime single-space form; real asctime double-space →
      nil QUIRK preserved), nowSeconds injectable clock (defaults getSecondsSinceEpoch),
      domainMatches (dot-strip once, requires "." or localhost), pathMatches (boundary rule),
      cookieKey (domain|path|name|hostOnly), parseSetCookie (Path noslash→"/", Max-Age≤0
      deletes / junk ignored, Expires only if expiry unset, value rejects quote/comma/
      control chars, name must be RFC2616 token), store (silent no-op re-store, replace
      moves to END, cap 300 evicts FRONT, save-on-change), processSetCookies, getHeader
      (sorted-by-insertion Cookie: assembly), prune (expired vs injected clock), clear,
      count.
- [x] Self-tests: 119 assertions from host-Lua oracle fixtures (makeTimestamp ×10, parseDate
      ×11, Set-Cookie ×33 incl. both attr orders, store/getHeader/cap/expiry ×~40, storage
      logic + disk round-trip ×~25) run at boot after url selftests.
      New util: util/luanum.c/h (lua_tonumber_strict); url.c refactored onto it.
- [x] Verify; STOP.
      Verified: make clean && make = 0 errors / 0 warnings; simulator run:
      "[P04] storage+cookiejar selftests done: 119 passed, 0 failed"
      (logs/phase-P04.log); bugs found & fixed en route:
      (1) lp_find called with NULL out-params segfaults (P02 API contract: always pass
      buffers) — fixed call site;
      (2) leading-slash "/Data/<bundle>/file" paths break open(kFileRead) on sim — datastore
      parity requires bare relative filename "comet_browser_data.json" (+ kFileReadData flag);
      (3) PlutoBookmark name collision with constants.h — persisted record renamed
      PlutoSavedBookmark, defaults now sourced from PLUTO_DEFAULT_BOOKMARKS (P01 table).

### PHASE P05 — core/tasks.c (cooperative scheduler)  ✅ DONE (verified in simulator)
- [x] Job registry, run(step, ctx, ctxFree, onComplete, onError, ud), yieldCheck honoring
      FRAME_BUDGET_MS=500 + CHECK_EVERY=64 shared counter, monotonic clamped progress
      get/set, cancelAll, scheduleGC (log-only note; C has no GC), update() pump wired into
      the main loop. C has no coroutines → jobs are resumable state machines:
      step() returns DONE/YIELD/ERROR; tasks_yield_check() returns "must unwind" at the
      same loop positions Lua calls Tasks.yieldCheck(); tasks_set_error() carries the
      message to onError ("Parse Error: ..." style strings stay caller-built).
- [x] Ground truth via host-Lua oracle on the REAL tasks.lua (p05_truth.txt): run resets
      progress immediately + starts next frame; head-only FIFO one resume per update;
      done forces progress=1 before pcall(onComplete); error path KEEPS task-reported
      progress; cancelAll empty-queue = full no-op / mid-flight drops with NO callbacks +
      frees ctx; GC drain clears within one update regardless (pcall quirk); yieldCheck
      outside a job never yields; frozen clock (elapsed==frameStart) never yields;
      +20ms/iter × 1000 iterations = 16 frames, first gate at iteration 64.
      Self-tests: 33 assertions (fake injected clock), all encoded from oracle output.
- [x] Verify; STOP.
      Verified: make clean && make = 0 errors / 0 warnings; simulator run:
      "[P05] tasks selftests done: 33 passed, 0 failed" (logs/phase-P05.log);
      fixes en route: missing da_init in tasks_init (queue pushes silently no-op'd);
      da_remove_at promoted storage.c-private helper → util/dynarray (shared API);
      budget-gate test must run first (shared counter parity) or first-yield lands at 44
      not 64.

### PHASE P06 — core/encoding.c + html/entities.c  ✅ DONE (verified in simulator)
- [x] Encoding.toUtf8: BOM detect UTF-8/UTF-16LE/BE; transport charset param;
      scanMetaCharset first 1024 bytes; charsetFromHeader; normalizeCharset aliases;
      windows-1252 table (exact CP1252 0x80–0x9F mappings); utf16→utf8 converter;
      single-byte→utf8; utf8Encode codepoint encoder.
- [x] Entities: full NAMED_ENTITIES table verbatim (Lua duplicate keys resolved
      last-wins), numeric &#NN;/&#xHH; decoding incl. bounds/overflow→" ", decode()
      pipeline order (fast path → decimal → hex → named → fixed UTF-8 seqs →
      transliteration loop w/ Tasks.yieldCheck per byte), encode() inverse (& < > "
      escaped, amp first, apostrophe NOT).
- [x] Ground truth via host-Lua oracle on the REAL encoding.lua + entities.lua
      (p06_truth.txt). Quirks preserved: quoted header charset value FAILS the Lua
      pattern (meta scan then applies); bare "utf16" never dispatched; lone high
      surrogate consumes the next unit before emitting "?"; hex branch lacks the
      decimal branch's 0x201A/0x201E specials; &frac12; never matches (%a+ can't
      cross digits); meta-scan greedy [^>]* resolves duplicate charset= attrs to
      the LAST occurrence; header beats meta unless header is utf-8; emoji widens
      one space PER byte; truncated tails widen.
      Self-tests: 75 assertions, all encoded from oracle output.
- [x] Verify; STOP.
      Verified: make clean && make = 0 errors / 0 warnings; simulator run:
      "[P06] encoding selftests done: 75 passed, 0 failed" (logs/phase-P06.log);
      fixes en route: entities hex branch missed the '#' before [xX] (hex entities
      never decoded); test fixtures initially passed NULL content-type to CP1252
      probes (verbatim is correct without a dispatching charset) and used raw
      Latin-1 bytes instead of UTF-8 in the transliteration fixture.

### PHASE P26B — render/layout.c (block layout engine)  ⬜ (INSERTED)
> Gap found during P06: render/layout.lua (~1472 LOC) had no dedicated phase;
> P32/P33 assume it exists. This phase closes that gap before P27 consumes it.
- [ ] Port block layout: node tree walk, inline text wrapping vs Playdate font
      metrics (getImageTextWidth equivalent), margins/padding collapse rules as
      implemented in source, list/table/figure/image geometry, absolute offsets,
      page height computation feeding the scroll renderer.
- [ ] Oracle-driven like P05/P06; self-tests assert computed rects for fixture
      documents against host-Lua output of the REAL layout.lua.
- [ ] Depends on P08–P25 DOM/render primitives being present; schedule before P27.

### PHASE P07 — core/http_client.c (raw TCP HTTP/HTTPS) + BENCHMARK  ✅ DONE (verified in simulator)
- [x] Port doGet/get/update/cancel/isLoading over playdate->network->tcp with identical
      constants (MAX_RESPONSE_SIZE=2MB, TIMEOUT=60000ms, MAX_REDIRECTS=5, READ_CHUNK=32768).
      Files: src/core/http_client.{h,c} (~830 LOC), src/core/internal_pages.{h,c}
      (byte-exact GENERATED from the Lua INTERNAL_PAGES via p07_dump_pages.lua extraction),
      src/core/selftest_http.{h,c} (89 checks, offline fake-TCP vtable +
      hc_set_tcp_for_tests/hc_set_clock_fn injection hooks).
- [x] buildRequest (method GET, Host, User-Agent/Accept headers exactly as source;
      Lua port rule verbatim: any port != 80/443 shown incl. parse("") -> "blank:0"),
      parseHeaders (status line, case-insensitive keys, Content-Length full-string
      tonumber parity, Transfer-Encoding chunked, Location, Content-Type, Set-Cookie
      list capture), decodeChunked (full-string hex sizes, extensions/trailers,
      incomplete->raw fallback at close), redirect deferral one tick + depth cap 5,
      generation-id staleness guard via setUserdata, closeTcp/reset state machine,
      internal about:home/blank/acidtest pages served after 20ms.
- [x] Faithful quirks kept: slice-to-end beyond Content-Length; too-many-redirects
      SILENT DROP (error branch wiped by unconditional reset()); watchdog >512-byte
      partial completes instead of erroring; progress (0,0) until headers parsed;
      304-without-Location falls through; empty-string URL parses to https://blank/.
- [x] C-only additions: requestAccess() gating (kAccessAsk async wait, session grant
      cache keyed by host), ms timeouts (10000 connect/read), PDNetErr-name error
      strings for send/open failures.
- [x] Wire CookieJar.getHeader outbound + processSetCookies inbound (round-trip
      verified against the real P04 jar); charset passthrough to caller headers.
- [x] BENCHMARK (§4.10, log-only): https listing 5217 bytes / 3604 ms (TLS handshake
      dominates); http probe 404 page 996 bytes / 282 ms — full HTTP flow exercised.
- [x] Verify in simulator: **[P07] http selftests done: 89 passed, 0 failed**; P02–P06
      still green (128/70/119/33/75); clean make 0 errors / 0 warnings; logs exported
      to logs/phase-P07.log; STOP.

### PHASE P08 — html/tokenizer.c  ✅ DONE (verified in simulator)
- [x] tokenize() ported to src/html/tokenizer.{h,c} (~330 LOC) with exact source
      semantics: MAX_HTML_SIZE=262144 truncation cutting at first '>' found in the
      [MAX-128, ∞) window (single trailing text token when no '<' survives — oracle
      probed: 262202-byte cut, 262144 when window empty / exactly-MAX untouched);
      doctype/junk tags vanish silently; comments produce NO token (-->" searched
      from tagStart); script/style skipped via case-insensitive literal closers
      searched from tagEnd; <title> open tag never emitted (title text ws-collapsed,
      trimmed, entity-decoded into pageTitle, pos += 8); unterminated tag drops the
      broken remainder; "3 < 4 > 2" yields TAG "4" quirk preserved.
- [x] findTagEnd quote-aware segment scan ('>' vs quoted skips); parseAttributes
      byte-wise port with all quirks verified by oracle: key charset [w-_:]
      (dot starts bogus-key single-byte skip), space-before-'=' loses the value
      ("href =\"v\"" -> href="" + boolean v), unquoted value charset [w-_./?#],
      first-wins per lowered key, Lua-true booleans as HT_ATTR_TRUE sentinel,
      missing closing quote aborts whole attr parse, trailing "key=" -> "".
- [x] NOTE: plan wording said tokens TEXT/OPEN/CLOSE/COMMENT/DOCTYPE, but the
      SOURCE emits only {text, tag(isClosing,isSelfClosing)} — parity kept to
      source (closing tags always carry isSelfClosing=1).
- [x] Oracle p08_oracle.lua drove the real tokenizer.lua over 29 fixtures ->
      p08_truth.txt; C selftest (src/html/selftest_tokenizer.{h,c}, 68 checks)
      replays every fixture incl. dup-first-wins, title-prefix hijack
      ("<titles>" consumes following <title> region), truncation lengths.
- [x] Tasks integration per iteration: tasks_yield_check() cancel +
      tasks_report_progress(0.5*(pos+1)/workLen) (global monotonic max shared
      across phases — asserted >= own contribution).
- [x] Verify: **[P08] tokenizer selftests done: 68 passed, 0 failed**; P02–P07
      still green (128/70/119/33/75/89); clean make 0 errors / 0 warnings;
      logs exported to logs/phase-P08.log; STOP.

### PHASE P09 — html/dom.c  ✅ DONE (verified in simulator)
- [x] DOM.build ported to src/html/dom.{h,c} (~330 LOC): stack builder over the
      token stream with exact source semantics — VOID set (14 elems), SKIP_SUBTREE
      {template,head,selectedcontent} with 500-token safety valve (valve resets
      skipDepth but skippedDepth stays cumulative — oracle-probed: 500 skipped,
      remaining 350 spans built), BLOCK set closeOpenP scanning past ancestors
      ("close_through": inner <div> pops p+span), implied-end helpers
      prepareListItem/DtDd/Row/Cell/Option incl. stray-element stack truncation
      to root and stray row/cell/option DROPPED (doPush=false).
- [x] Nested anchors reopen at parent level; closing void/skip tags ignored;
      self-closing non-void tags appended but never pushed (<div/> siblings OK);
      MAX_NODES=6000 cap; faithful dead-code quirk: loop breaks before append()
      can refuse so maxNodesHit stays false even capped (oracle: maxhit=0 @6000).
- [x] Ownership convention: dom_build TRANSFERS each token's attrs map / text
      buffer into nodes and nulls the token fields, so htt_free() afterwards
      stays safe (documented in dom.h); dropped elements destroy their attrs.
- [x] Oracle p09_oracle.lua drove real tokenizer.lua -> dom.lua over 25 fixtures
      -> p09_truth.txt; C selftest (src/html/selftest_dom.{h,c}, 49 checks)
      replays every fixture through the REAL pipeline (htt_tokenize -> dom_build):
      nesting survival, list/dl/table shapes, skip valve, 6000-node cap,
      diag counters per case.
- [x] Tasks integration per token: tasks_yield_check() cancel +
      tasks_report_progress(0.5 + 0.3*(i/n)) (oracle probe first value 0.8).
- [x] Verify: **[P09] dom selftests done: 49 passed, 0 failed**; P02–P08 still
      green (128/70/119/33/75/89/68); clean make 0 errors / 0 warnings; logs
      exported to logs/phase-P09.log; STOP.

### PHASE P10 — html/document.c PART 1 (core block building)
- [x] Document.parse scaffolding: mode switch (READER -> NULL + log until P12),
      baseUrl/base href override (first nonempty href wins, then validated against
      ^[a-zA-Z][%w+%-.]*://), title from tokenizer ("Blank Page" early return for
      empty html), meta refresh detection head-scan + body-walker (body preferred;
      patterns "N; url=X" / "N"; delay via tonumber semantics; url resolved vs base).
      Form model collection DEFERRED to P11 per phase-boundary agreement.
      NOTE: base/meta TOKEN scans run BEFORE dom_build because dom_build TRANSFERS
      attrs ownership (Lua scanned the same tokens after build via shared refs).
- [x] Helpers: parseStyle (keys+values lowercased/trimmed, LAST duplicate wins,
      16-entry map), parseAlign (attr then text-align override), isDisplayNone
      (hidden/popover/display:none/visibility:hidden -> whole subtree skipped),
      isInvertedStyle (color white/#fff/#FFFF, bg black/#000 substring approx),
      parseBoxSpacing (num strips %, floor(n/2); margin shorthand 1/2/3/4-part;
      padding contributes LEFT only; unitless values only - tonumber("10px")=nil),
      concatNodeText, validHref (!empty !# !javascript: !data:), serializeSvgNode
      (recursive, entities_encode text, ["< escaped; hookup lands in P11).
- [x] Block model structs (document.h): DocInline (DIT_TEXT/DIT_BR/DIT_WBR +
      11 flags + href + anchorIndex), DocBlock (PARAGRAPH/HEADING/BLOCKQUOTE/
      LIST_ITEM/CODE_BLOCK/HR/IMAGE + align/spacingTop/spacingBottom/indent/
      invert/level/isOrdered/number/markerType(static)/depth/dtFlag/ddFlag/
      codeText+lines/img fields/imgInert), DocLink, DocDocument (+metaDelay/
      metaUrl); doc_free frees everything recursively.
- [x] Capture paths: h1-h6 (spacing 18/8), paragraph set p/div/section/article/
      header/footer/main/nav/aside/noindex/search, blockquote indent=left+12,
      center/marquee align=center, pre/xmp/listing/plaintext (raw buffer + \r?\n
      line split incl blank lines), ul/ol/menu/dir ctx stack (start/reversed/type
      whitelist/depth), li numbering + value= renumbering, dl/dt/dd (dlDepth*20
      indent, dt/dd flags), figure/figcaption caption attach to image block,
      inline runs b/i/u/s/mark/small/big/sub/sup/code families, span/font/time/
      data style-driven flags + time/data value fallback when no inline children,
      q bold-italic quotes, a links (resolve/anchorIndex/target/title->href text
      fallback/linkText accumulation across children), br/wbr/hr(6/6), img
      (src->data-src->srcset first token, alt->title->"Image", defaults 160/80,
      <=0->defaults, tracking/beacon filter pre-resolve, clamps 360/180, usemap
      # strip, inherits link href, figure deferral).
- [x] Caps: MAX_BLOCKS=1200 (truncated flag -> "(Page truncated: too many blocks)"
      bold centered notice appended past cap), MAX_INLINES=900 (silent drop);
      empty result -> "(Empty Web Page)" italic notice; aria-hidden inert scope.
- [x] Verify: **[P10] document selftests done: 69 passed, 0 failed**; P02-P09
      still green (128/70/119/33/75/89/68/49); clean make 0 errors / 0 warnings;
      host-side replay harness caught splitter + meta-resolve bugs pre-device;
      logs exported to logs/phase-P10.log; STOP.

### PHASE P11 — html/document.c PART 2 (tables, svg, misc, parity sweep)
- [x] Table parsing (rows/cells/spans as source handles), fieldset, details/summary,
      dialog, fencedframe placeholder, select options, button/input blocks, marquee-ish
      leftovers — i.e., EVERY remaining branch of Document.parse walked against source.
- [x] Inline SVG extraction → serializeSvgNode → SVGDecoder hook point (decoder arrives P24;
      store serialized XML now).
- [x] Line-by-line parity audit of document.lua vs document.c (checklist appended to phase
      log listing every source branch → C location).
- [x] Verify; STOP.

      DONE (2026-08-21). Blocks DB_TABLE / DB_INPUT_FIELD / DB_CHECKBOX_FIELD /
      DB_INPUT_SUBMIT / DB_SELECT_FIELD / DB_BOX_OPEN / DB_BOX_CLOSE / DB_PLACEHOLDER /
      DB_METER / DB_MATH implemented; doc.maps + doc.datalists stored. inputType made an
      owned char* (fixes use-after-free); per-element formaction/formmethod overrides added
      (d_form_overrides). Faithful quirks kept and selftest-pinned: implicit <tr> nesting
      (row loss), <option> dropped without <select> ancestor (datalists empty), stray
      tr/td text leaks as paragraph, viewBox lookup dead in both impls (tokenizer
      lowercases attr keys) → svg defaults 120x40, progress/meter swallow children,
      mroot "^(1/" + ")" before last child text, empty-page italic notice,
      dialog-open block ordering. StrBuf is not NUL-terminated until detach — math
      join now terminates via sb_reserve before d_collapse_trim (ASAN caught overflow).
      Selftests: 132 passed, 0 failed (host harness clean under ASAN; simulator log this
      phase). Build gate 0/0. main.c display label P10→P11. Phase log exported with full
      parity-audit checklist.

### PHASE P12 — html/readability.c
- [x] distill(tokens, rawTitle, baseUrl): STRIP_TAGS set, wordCount, isBareUrlText,
      mergeParagraphFragments, scoring/selection identical, reader block emission,
      title fallbacks.
- [x] Fixture article → distilled block dump logged.
- [x] Verify; STOP.

      DONE (2026-08-21). readability_distill() consumes HttTokens and returns a
      reader-mode DocDocument (DB_READER_HEADER + h1 + hr + merged content);
      doc_parse_opts(PLUTO_MODE_READER) wires it (rawHtml attached, as Lua).
      New DocBlock fields readerHost/readerTitle/readingTime; DocDocument
      readerWords/readerTime; doc_free_block_fields exported for reuse.
      Faithful quirks pinned by tests: nav/header/footer/aside container branch
      is dead code in source (they are STRIP_TAGS); containers walked in
      table.sort order despite "document order" comment; running fragment
      accumulator over-merges until a sentence-ending punct appears; inline
      texts join raw without injected spaces; formMethod persists after
      </form> while formAction resets; button uses open-time action snapshot,
      input-submit uses the live one. isBareUrlText replicates Lua pattern
      backtracking via right-to-left dot scan.
      Selftests: 36 passed, 0 failed (host + ASAN clean + simulator log this
      phase). Build gate 0/0. Phase log exported with full parity map.

### PHASE P13 — render/style.c + fonts
- [ ] Load fonts: Roobert-20-Medium, Roobert-10-Bold, Roobert-11-Medium,
      Roobert-11-Mono-Condensed (+ halved variants where source uses them, e.g.
      Roobert-10-Bold-Halved in hud.lua); Style.init/getTextWidth (fallback chain),
      getHeadingFont(h1..h6 mapping), getBodyFont(isBold,isCode), getInlineFont(...).
- [ ] Log font load results + sample widths.
- [ ] Verify; STOP.

### PHASE P14 — render/link_manager.c
- [ ] Full port: addLinkRect merging consecutive rects per anchorIndex, getCount,
      findInitialSelection (viewport-distance rule), selectNext/selectPrev (skip logic +
      scroll-follow amounts), getSelectedLink, drawSelectedHighlight (inversion rect),
      isHighlighted, addLink, getHoveredLink hit-test, clear/clearSelection.
- [ ] Verify with synthetic rects logged; STOP.

### PHASE P15 — render/decoders/dither.c + scale.c
- [ ] rgbToGray ((306*r+601*g+117*b)>>10), 4×4 Bayer ordered dithering, toImage building
      LCDBitmap via framebuffer write (max 380x240 guard), run-length fillRect batching
      option preserved or improved with identical output pixels.
- [ ] Scale.boxSizes + newAccum streaming box filter (addRow/finish) bit-exact.
- [ ] Golden-pixel tests logged (small synthetic images).
- [ ] Verify; STOP.

### PHASE P16 — render/decoders/inflate.c
- [ ] createBitStream (LSB-first), canonical Huffman buildHuffmanTable, decodeSymbol,
      fixed tables, stored/fixed/dynamic blocks, Inflate.decompress + streaming
      Inflate.createStream (used by PNG row streaming).
- [ ] Test vectors (zlib-produced fixtures) round-trip logged.
- [ ] Verify; STOP.

### PHASE P17 — png.c + BENCHMARK
- [ ] PNGDecoder.decode: signature, IHDR, palette/tRNS/gAMA-ignore per source, IDAT concat
      streaming inflate, unfilter (none/sub/up/avg/paeth), alpha composite over white,
      Adam7 (first pass only per source), gray/palette/truecolor bit depths as supported by
      source, Scale downsample to maxW/maxH, Dither.toImage.
- [ ] Benchmark: PNG samples from https://wiesmann.codiferes.net/share/bitmaps/ ; log dims/ms.
- [ ] Verify visually in sim (temporary debug viewer drawing decoded image); STOP.

### PHASE P18 — bmp.c
- [ ] BMPDecoder.decode: BM header, pixelOffset/headerSize/width/rawHeight(top-down)/bpp/
      compression checks, 1/4/8-bit palette indexed, 24/32bpp, bottom-up flip, scale-to-fit
      (360x200), dither out.
- [ ] Benchmark BMP sample from bitmaps site; log.
- [ ] Verify; STOP.

### PHASE P19 — gif.c
- [ ] GIF87a/89a: header/screen descriptor, global/local palettes (grayscale shortcut),
      LZW streaming into box downscaler, first-frame-only, interlace handling per source,
      Tasks yield points, transparent index → white composite.
- [ ] Benchmark GIF sample; log.
- [ ] Verify; STOP.

### PHASE P20 — jpeg.c
- [ ] Baseline SOF0: markers, quant tables, Huffman buildHuff/decodeSymbol/extend,
      decodeDC/decodeAC, restart intervals, idct2d fixed-point (4096 basis) EXACT math,
      luma-only rendering (chroma consumed for sync), coarse DC-only box when downscaled
      hard, YCbCr→gray path as source does, progressive SOF2 DC-scan-only path
      (decodeProgressiveDC/renderProgressiveDC), reject arithmetic SOF9/10/11, yield per
      MCU row, scale+dither out.
- [ ] Benchmark JPEG samples; log dims/ms.
- [ ] Verify; STOP.

### PHASE P21 — webp.c PART 1 (VP8L lossless)
- [ ] BitReader (brPrefetch/brAdvance/brReadBits), huffman (replicateValue/getNextKey/
      nextTableBitSize/buildHuffmanTable rootBits=8+7 readSymbol/readSymbol7),
      readHuffmanCodeLengths/readHuffmanCode/readHuffmanCodes (5 metas, color cache),
      decodeImageData (lz77 window, planeCodeToDistance, transforms):
      predictorAdd/Inverse (26 modes incl. select/clampedAddSubtractFull/Half, average2/3/4),
      colorTransformDelta/transformColorInverse/colorSpaceInverse, subtract-green
      addGreenToBlueAndRed, colorIndexInverse + expandColorMap, applyInverseTransforms,
      decodeVP8LPayload (ARGB out).
- [ ] Test vector: known .webp lossless → ARGB checksum logged (compare vs dwebp reference
      computed offline once, recorded in log).
- [ ] Verify; STOP.

### PHASE P22 — webp.c PART 2 (VP8 lossy + alpha + animation) + webp_vp8_data.c
- [ ] Transcribe VP8Tables verbatim into webp_vp8_data.c (kDcTable…CoeffsUpdateProba).
- [ ] VP8: newBr/vp8GetBit/GetSigned/GetValue/SignedValue, bool decoder vp8LoadNew,
      segment/filter/quant/proba headers, tokens (getLargeValue/getCoeffs/zigzag/bands),
      parseResiduals context model, intra modes (16x16 + 4x4 B-modes w/ kBModesProba),
      transforms (WHT/one/AC3/DC/UV), predictors 16/8/4 (dc/ve/he/TM/rd/vr/ld/vl/hd/hu),
      loop filter (simple + normal, inner edges, precomputeFilterStrengths), reconstructRow
      scanline pipeline, fancy chroma upsample (upsampleLinePair/writeRgbPixel/clipYUV),
      finishRow caching, decodeAlphaPlane (ALPH unfilter + VP8L-compressed alpha),
      MAX_PIXELS guard.
- [ ] Animation: parseWebPAnimation (VP8X/ANIM/ANMF), blendPixelNonPremult, keyframe/dispose/
      blend state machine, decodeAnimation; WebPDecoder.decode picks still-vs-anim first
      frame; _testDecodeRaw/_testBuildTable hooks.
- [ ] Benchmark lossy+lossless samples from bitmaps site; log ms; visual check.
- [ ] Verify; STOP.

### PHASE P23 — ico.c
- [ ] ICONDIR/Icondir entries sort (area desc, bpp desc), PNG-signature entries → PNGDecoder,
      else embedded DIB (height doubled, AND-mask transparency composited over white,
      1/4/8/24/32bpp, top-down flag), nearest-scale sampling, Dither out.
- [ ] Verify with favicon fixture; log; STOP.

### PHASE P24 — svg.c
- [ ] SVGDecoder.decode: viewBox/width/height parse, scale cap ×2, min 20px, white canvas,
      expandUses (<use href> resolution), scanTags (comments/CDATA/doctype skips),
      mergeStyle(style attr wins), hidden/display:none subtree skip stack, hasInk rule,
      shapes rect(rx)/circle/ellipse(fallback rings)/line/polygon(close)/polyline,
      path M/L/H/V/Z/C(8 seg)/S/Q(6 seg)/T/A(straight chord) with relative support and
      smooth-control reflection, tokenizePathNumbers char-level parser (sign/dot/exponent
      edge cases), draw into offscreen bitmap, zero-drawn → nil.
- [ ] Fixture icons rendered; visual check; log shape counts.
- [ ] Verify; STOP.

### PHASE P25 — render/image_decoder.c
- [ ] Cache (url→bitmap|PENDING|FAILED), downloadQueue FIFO, processNextImage via
      HttpClient, decodeRawImageData magic dispatch (JPEG FFD8 async via Tasks; GIF87a/89a;
      PNG sig; BM; RIFF/WEBP; ICO; svg sniff '<svg'/'<?xml'), failure→cached false,
      max-dims caps, ImageDecoder.draw placeholders ([Image Off]/[Hover]/on-demand card w/
      alt + A/B hints + selected inversion), enqueue/evict/isCached/isDecoded/getImage/
      clearCache/update, _testDecode.
- [ ] End-to-end: load a page image through network in sim; log pipeline timings.
- [ ] Verify; STOP.

### PHASE P26 — render/cloud_layout.c (+json)
- [ ] CloudLayout.parse(jsonString, baseUrl): element list (type/text/x/y/font/size...),
      resolve relative URLs; build(doc) wiring; draw(scrollY) painters identical fonts/
      offsets; MODE_OPERA_DS path ready.
- [ ] Fixture cloud JSON rendered; log.
- [ ] Verify; STOP.

### PHASE P27 — ui/chrome.c + ui/hud.c
- [ ] Chrome.draw: black bar, separator, SSL lock/globe vector drawing, host display rules
      (about:, >28 chars ellipsis), [READ]/[WEB] badge, comet loading dots anim (12-phase),
      progress bar (known total vs indeterminate sweep), clock HH:MM via playdate->system->
      getTime; cometAnimFrame static.
- [ ] Hud.draw scrollbar (track/thumb math exact) + active-link bottom HUD (56-char clip);
      Hud.drawHoverStatus (halved bold font pill bottom-left, 50-char clip).
- [ ] Visual check in sim; STOP.

### PHASE P28 — ui/home_page.c
- [ ] Speed dial: logo header (comet pixel art lines + circles), banner text, address-bar
      prompt pill, selectable Settings button, 2-col bookmark grid (172×46 cards, gaps),
      selection inversion, marquee oscillating clipped text (speed 50 px/s, dwell 1.0s,
      per-key state), crank scroll ×1.5 w/ invertCrank, auto-scroll-to-selection math,
      handleInput grid nav rules (settings↔first row, odd/even column moves), A→url /
      settingsCallback, footer hint line, reset().
- [ ] Visual + input check; STOP.

### PHASE P29 — ui/address_bar.c + VENDOR KEYBOARD (Raphcal C lib)
- [ ] Vendor https://github.com/Raphcal/some-corelibs-port/tree/main/keyboard into
      vendor/keyboard; adapt to SDK C API; ensure it renders + edits text fully in C.
- [ ] AddressBar.open (prefill non-about currentUrl), launchKeyboard gating (B held check,
      shown-once), keyboardWillHide(submitted): trim whitespace, empty→cancel path,
      isSearchQuery→buildSearchUrl(selected engine) else URL.parse normalized, onSubmit
      callback, skipInputFrames=2; cancel() hides keyboard + clears callbacks;
      drawOverlay two layouts (armed pill vs keyboard-side box w/ wrapped mono text).
- [ ] Manual test: type URL, submit navigates (stub nav ok), B+Left/Right while armed works.
- [ ] Verify; STOP.

### PHASE P30 — ui/error_page.c + bookmarks_page.c + history_page.c
- [ ] ErrorPage: show/handleInput (left/up, right/down clamp 1..3, A→retry/search/home,
      draw roundrect dialog, msg/url 48/50-char clips, 3 buttons 100×28 sel inversion).
- [ ] Bookmarks/History pages: open/handleInput (A open url, B close), draw list rows
      34px, title 34/url 46-char clips, crank scroll ×2, culling, empty-state texts.
- [ ] Visual + input check; STOP.

### PHASE P31 — ui/settings_page.c
- [ ] Staged-settings overlay: open(prevState) snapshot, options Search Engine (cycle),
      Browse Mode toggle, Invert Crank, Image Mode cycle (NAMES order), Clear Cookies
      action (immediate), Save(A)/Cancel(B) semantics, onChangeCallback re-render trigger,
      ease-out-cubic 300ms open animation (box grows from center, radius lerp, content
      after t>0.4), row painters + < > arrows + "Press A" hint, footer hint.
- [ ] Verify persistence via Storage.save + reload; STOP.

### PHASE P32 — main.c state machine, navigation, forms, menu, internal pages
- [ ] States home/loading/page/error/bookmarks/history/settings; navigateTo flow
      (about:home special-case, scheme prepend/normalization identical, pushHistory cap 30,
      loading→HttpClient.get with callbacks: success→Encoding→Tokenizer→DOM→Document/
      Readability per mode→Layout.build→image enqueue per imageMode; error→ErrorPage.show;
      meta-refresh scheduled redirect; goBack/goForward stack ops).
- [ ] System menu: Home-Page, View mode option (Reader/HTML, page-only), Settings, History,
      Clear Cookies — exact labels/behavior.
- [ ] Reader-mode input: Up/Down link jumps w/ scroll follow, A follow link/activate input,
      B address bar, crank kinetic scroll physics (targetScrollY + damping constants from
      source), A+Left/Right back/forward.
- [ ] Form submission: activateFormBlock (checkbox/radio toggle, select cycle?, submit),
      openKeyboardForInput (text input via keyboard → value), submitForm builds query per
      HTML spec (hidden included, submit name/value appended, GET URL assembly exactly as
      source; POST if source does — mirror).
- [ ] Loading state: B cancel, Left cancel+back, progress UI.
- [ ] Internal pages: about:home/help/acidtest/blank content generation identical.
- [ ] updateFrame composition order: Chrome → content (Layout/CloudLayout/Home) → overlays
      (on-demand, address bar, settings) → Hud; playdate.update pumps Tasks/HttpClient/
      ImageDecoder/keyboard/crank deltas.
- [ ] Full manual pass in sim navigating real sites; logs; STOP.

### PHASE P33 — HTML mode virtual mouse cursor + image interaction modes
- [ ] Cursor: d-pad held movement (repeat rates from source), crank vertical, edge
      auto-scroll, A click (link hit via LinkManager.getHoveredLink, form activation),
      hover status bar via Hud.drawHoverStatus, cursor sprite rendering.
- [ ] Image modes end-to-end: ALL (background queue), INVIEW (viewport load + 200px evict),
      ONDEMAND (placeholder card → A view/B open-link overlay, unload on re-tap),
      HOVER (load on hover/selection proximity, evict on leave), DISABLED placeholders.
- [ ] Layout.handleOnDemandInput/drawOnDemandOverlay/showOnDemandOverlay/clearOnDemandOverlay
      + evictOffscreen/evictHoveredImage wired to main loop.
- [ ] Verify each mode via Settings cycling on a real page; logs; STOP.

### PHASE P34 — Integration, performance, memory, BENCHMARK SUITE
- [ ] Full flows: cold boot→home→search→article (reader)→toggle HTML mode→forms→images all
      modes→bookmarks/history/settings/cookies persist across reboot (datastore reload).
- [ ] Benchmark battery against https://wiesmann.codiferes.net/share/bitmaps/ : per-format
      decode table (bytes, ms, fps impact) logged; optimize hot spots (only if needed to
      reach usable interaction, preserving output parity).
- [ ] Memory soak: long browsing session, cache eviction correctness, no leaks (log
      heap stats via SDK _malloc counters if available).
- [ ] Regression checklist from README §Controls executed manually; results logged.
- [ ] STOP.

### PHASE P35 — Feature-parity audit (README + code sweep)
- [ ] Walk README "Key Features" bullet-by-bullet; verify each in sim; record PASS/FAIL.
- [ ] Grep-driven sweep: every public function in §2 inventory confirmed present + called in
      C port; every constant referenced; every Lua branch of main.lua input handling mapped.
- [ ] Diff behaviors found → fix in this phase (allowed: fixes only, no new features).
- [ ] Re-run P34 quick suite; STOP.

### PHASE P36 — FINAL CLEANUP (GATED — requires explicit user authorization)
Only after P35 passes AND user says go:
- [ ] Restore permanent home page/default bookmarks usage (remove benchmark-site defaults if
      any were introduced temporarily).
- [ ] Remove diagnostic logs ONLY if user authorized; otherwise keep.
- [ ] Clear image/data caches from data dir if user authorized.
- [ ] Delete generated .pdx artifacts from repo working tree if user authorized; clean build.
- [ ] Final `make` + sim smoke test; final report; STOP.

---

## 6. Verification Protocol (every phase)

1. `make clean && make` — zero errors/warnings introduced.
2. Launch Simulator (`make sim` or open .pdx).
3. Execute the phase's manual test list; capture pluto.log evidence.
4. `cp` relevant log excerpt → `logs/phase-XX.log`.
5. Update checkboxes here (only verified items), append notes/bugs under the phase.
6. Report: what passed, what failed/blocked, exact next step. Then STOP.

## 7. Open Decisions (defaults chosen; user may override)
- Bundle ID: com.bryanwandrych.plutobrowser (mirrors original author style).
- Datastore filename stays `comet_browser_data` for data continuity.
- Temporary benchmark home-page override allowed during P07–P34, reverted in P36.
- Host-side (macOS) unit-test harness for pure-C modules is permitted as dev tooling but is
  optional; in-sim self-tests via logs are the required minimum.
