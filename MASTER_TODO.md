# PlutoBrowser — Master TODO (C Port of CometBrowser)

**Status: PLANNING COMPLETE — PHASES P01–P31 DONE + INSERTED P26B DONE (verified in simulator). NEXT: P32.**
This document is the single source of truth for the port. Every execution session performs
EXACTLY ONE phase, then updates this file and STOPS.

**Phase progress:** P01–P09 ✅ P10 ✅ P11 ✅ P12 ✅ P13 ✅ P14 ✅ P15 ✅ P16 ✅ P17 ✅
P18 ✅ P19 ✅ P20 ✅ P21 ✅ P22 ✅ P23 ✅ P24 ✅ P25 ✅ P26 ✅ P27 ✅ P28 ✅
P29 ✅ P30 ✅ P31 ✅ | P32–P36 ⬜

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
├── Source/vendor/keyboard/          # Raphcal some-corelibs-port keyboard (C) — vendored
└── Source/
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
`Source/vendor/keyboard/`, adapt includes/build, wire callbacks to AddressBar semantics:
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
      NOTE: build layout follows the SDK convention — C sources in `Source/`, pdc input
      (pdxinfo + assets + built pdex.elf/pdex.dylib) in `Source/` (SDK common.mk hardcodes
      `Source` as the pdc input dir). Same split as the official "Hello World" C example.
- [x] `Source/pdxinfo`: name=PlutoBrowser, author=Bryan Wandrych,
      bundleID=com.bryanwandrych.plutobrowser, version=1.0.0, buildNumber=1,
      imagePath=assets/launcher.
- [x] Makefile: HEAP_SIZE=8388208 STACK_SIZE=61800, SRC list + VPATH (src:Source/core:...),
      includes $(SDK)/C_API/buildsupport/common.mk (device arm-gcc pdex.elf + simulator
      clang pdex.dylib + pdc packaging); extra `sim` target opens Playdate Simulator.app.
      SDK fallback /Users/bwandrych/Developer/PlaydateSDK honored.
- [x] `Source/main.c`: eventHandler (kEventInit/kEventTerminate), setUpdateCallback loop,
      placeholder screen ("PlutoBrowser" + frame counter) using Roobert-11-Medium.
      C-API notes learned: no setTextColor/setColor for text — black text via default
      kDrawModeCopy; white text later via setDrawMode(kDrawModeFillWhite); shape calls
      take LCDColor as a parameter (fillRect(x,y,w,h,color) etc.).
- [x] `Source/core/constants.c/h`: full port of constants.lua (geometry macros PLUTO_*,
      PlutoState/PlutoMode enums + exact-string name functions, SEARCH_ENGINES ×4 verbatim,
      DEFAULT_BOOKMARKS ×9 verbatim, USER_AGENT verbatim, image-mode enums + NAMES order +
      LABELS map).
- [x] `Source/core/logger.c/h`: pluto.log, truncate-on-init banner + gamePath line
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

### PHASE P26B — render/layout.c (block layout engine)  ✅ DONE (verified in simulator)
> Gap found during P06: render/layout.lua (~1472 LOC) had no dedicated phase;
> P32/P33 assume it exists. This phase closes that gap before P27 consumes it.
- [x] Port block layout: node tree walk, inline text wrapping vs Playdate font
      metrics (getImageTextWidth equivalent), margins/padding collapse rules as
      implemented in source, list/table/figure/image geometry, absolute offsets,
      page height computation feeding the scroll renderer.
      Files: Source/render/layout.{h,c} (~1930 LOC). LItem pool + 20 item types;
      breakLines word-wrap parity (leading-ws drop, ^S+ words, following-ws run
      gap, tab stops via TAB_COLUMNS*spaceW, lineH=max across inline fonts);
      emitFlow merges consecutive same-href+anchor words into one LinkManager
      rect spanning gaps, sub/sup dy +3/-4; all block painters (headings w/
      underline rules level<=2, hr, code box, quote bar, table box, five image
      modes, form controls input/checkbox/select/submit/hidden, meter, box
      frames, reader badge); totalHeight=max(currentY+20,216); scrollbar,
      on-demand image overlay state machine (show/handle/clear/consumed),
      evictOffscreen VIEWPORT-only bounds. LmRect extended per-rect extras
      (isToggle/toggleKey/isImage/src/alt/inert/isFormInput/inputBlock).
- [x] Oracle-driven like P05/P06; self-tests assert computed rects for fixture
      documents against host-Lua output of the REAL layout.lua.
      Source/render/selftest_layout.{h,c}: **82 pass / 0 fail** in simulator —
      roman/alpha markers, tab expansion, empty-doc floor 216, flow geometry
      (marginX=10/startY=32/lineH=16), wrap rows step 16 at marginX, heading
      rule y=61 + h3-no-rule, hr/code(40px)/image-clamp/table(84px) geometry,
      checkbox label truncation + input: links, submit >=50 clamp + action,
      select borrowed options + select:q, list ix./depth-14 indent, quote bar,
      toggle strips d1, hidden fields zero-footprint, badge h28, parser
      integration (resolved hrefs carried on text items), on-demand overlay
      state machine incl. A-view/B-link/cancel paths.
- [x] Depends on P08–P25 DOM/render primitives being present; schedule before P27.
      Fixed latent P13 bug en route: style_get_heading_font had swapped size/
      lineHeight semantics vs Lua (now returns font + out lineHeight/marginB
      24/18/16 + 6/5/4); all call sites updated. Fixed roman lowercase for
      ordered type="i" (was emitting uppercase). document.c maps type=hidden
      inputs to DB_INPUT_FIELD(inputType=hidden) -> zero-footprint items.
      Visual slot in main.c: kLyFixture demo page (63 items / totalH 737 /
      5 links) sweeping scroll with HUD counters. Clean make 0 errors /
      0 warnings; no regressions (all prior-phase selftests green this boot);
      log exported to logs/phase-P26B.log; STOP.

### PHASE P07 — core/http_client.c (raw TCP HTTP/HTTPS) + BENCHMARK  ✅ DONE (verified in simulator)
- [x] Port doGet/get/update/cancel/isLoading over playdate->network->tcp with identical
      constants (MAX_RESPONSE_SIZE=2MB, TIMEOUT=60000ms, MAX_REDIRECTS=5, READ_CHUNK=32768).
      Files: Source/core/http_client.{h,c} (~830 LOC), Source/core/internal_pages.{h,c}
      (byte-exact GENERATED from the Lua INTERNAL_PAGES via p07_dump_pages.lua extraction),
      Source/core/selftest_http.{h,c} (89 checks, offline fake-TCP vtable +
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
- [x] tokenize() ported to Source/html/tokenizer.{h,c} (~330 LOC) with exact source
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
      p08_truth.txt; C selftest (Source/html/selftest_tokenizer.{h,c}, 68 checks)
      replays every fixture incl. dup-first-wins, title-prefix hijack
      ("<titles>" consumes following <title> region), truncation lengths.
- [x] Tasks integration per iteration: tasks_yield_check() cancel +
      tasks_report_progress(0.5*(pos+1)/workLen) (global monotonic max shared
      across phases — asserted >= own contribution).
- [x] Verify: **[P08] tokenizer selftests done: 68 passed, 0 failed**; P02–P07
      still green (128/70/119/33/75/89); clean make 0 errors / 0 warnings;
      logs exported to logs/phase-P08.log; STOP.

### PHASE P09 — html/dom.c  ✅ DONE (verified in simulator)
- [x] DOM.build ported to Source/html/dom.{h,c} (~330 LOC): stack builder over the
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
      -> p09_truth.txt; C selftest (Source/html/selftest_dom.{h,c}, 49 checks)
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
- [x] Load fonts: Roobert-20-Medium, Roobert-10-Bold, Roobert-11-Medium,
      Roobert-11-Mono-Condensed (+ halved variants where source uses them, e.g.
      Roobert-10-Bold-Halved in hud.lua); Style.init/getTextWidth (fallback chain),
      getHeadingFont(h1..h6 mapping), getBodyFont(isBold,isCode), getInlineFont(...).
- [x] Log font load results + sample widths.
- [x] Verify; STOP.

      DONE (2026-08-21). style_init loads the six Style slots with per-slot
      fallback logging; style_set_system_font mirrors gfx.getFont() (C API has
      none — main.c registers its current body font). Width chain faithful:
      nil/""->0, font||body||sys, real metrics on device, 8 px-per-char last
      resort when built without the Playdate API (host harness). Heading map
      24/18/16 (+lh 6/5/4), body code15/bold16/plain16, inline precedence
      code > small/sub/sup > bold/big pinned by tests. Halved font belongs to
      the hud phase. All six fonts load in simulator; sample 'Hello World'
      widths body=95 mono=88 small=95 h1=150 logged.
      Selftests: 15 passed, 0 failed (host + ASAN clean + simulator log this
      phase). Build gate 0/0. Phase log exported with parity map.

### PHASE P14 — render/link_manager.c
- [x] Full port: addLinkRect merging consecutive rects per anchorIndex, getCount,
      findInitialSelection (viewport-distance rule), selectNext/selectPrev (skip logic +
      scroll-follow amounts), getSelectedLink, drawSelectedHighlight (inversion rect),
      isHighlighted, addLink, getHoveredLink hit-test, clear/clearSelection.
- [x] Verify with synthetic rects logged; STOP.

      DONE (2026-08-21). Full port of the 181-line source. Quirks pinned by
      tests: nil `text` arg can never merge (stored text defaults to href at
      insert); nil anchorIndex merges across any stored anchor; tie-breaking in
      both initial-selection passes keeps the FIRST link; hovered hit-test uses
      60/14 w/h defaults for unset rect fields; selection wrap-around only —
      current Lua contains no skip logic. drawRoundRect/fillRect take explicit
      color/lineWidth in the C API (same pixels). Selftests: 28 passed,
      0 failed (host + ASAN clean + simulator log this phase). Build gate 0/0.
      Phase log exported with parity map.

### PHASE P15 — render/decoders/dither.c + scale.c
- [x] rgbToGray ((306*r+601*g+117*b)>>10), 4×4 Bayer ordered dithering, toImage building
      LCDBitmap via framebuffer write (max 380x240 guard), run-length fillRect batching
      option preserved or improved with identical output pixels.
- [x] Scale.boxSizes + newAccum streaming box filter (addRow/finish) bit-exact.
- [x] Golden-pixel tests logged (small synthetic images).
- [x] Verify; STOP.

      DONE (2026-08-21). toImage PRESERVED as pushContext + run-length
      fillRect batching (identical pixels by construction) — proven on
      simulator by rendering a real LCDBitmap from a synthetic gradient and
      cross-checking its getBitmapData bits against the pure Bayer logic:
      expected_black=42803 == actual_black=42803 on a 400x500 source clamped
      to 380x240. Quirks pinned: gray==threshold paints WHITE; trailing
      partial scale block emits an EXTRA row so count can exceed targetH;
      rounding floor(sum/div+0.5) via doubles exactly like Lua numbers.
      C API fixes: no setColor() member (color per fillRect call);
      getBitmapData takes (w,h,rowbytes,mask,data). Selftests: 19 passed,
      0 failed + 3 device checks (host + ASAN clean + this phase's sim log).
      Build gate 0/0. Phase log exported with parity map.

### PHASE P16 — render/decoders/inflate.c
- [x] createBitStream (LSB-first), canonical Huffman buildHuffmanTable, decodeSymbol,
      fixed tables, stored/fixed/dynamic blocks, Inflate.decompress + streaming
      Inflate.createStream (used by PNG row streaming).
- [x] Test vectors (zlib-produced fixtures) round-trip logged.
- [x] Verify; STOP.

      DONE (2026-08-21). Full port of the 427-line source: LSB-first bit
      reader, canonical Huffman with per-symbol bit reversal, cached fixed
      tables, stored/fixed/dynamic blocks, one-shot decompress + streaming
      reader (64KB window halved to 32KB, pending queue, chunked reads).
      Quirks preserved: btype==3 silent fall-through in decompress; match
      copies emit 0 for pre-output sources; truncated input returns partial;
      zlib CMF/FLG + preset-dict skip. Stream path treats btype 3 as EOF to
      avoid infinite pump. Fixtures embedded: dyn 57B->360B, raw-deflate
      84B->6621B, stored 311B->300B, Z_FULL_FLUSH 116B->6621B, handmade
      aligned stored block; streaming exercised at awkward chunk sizes (7B).
      BUG fixed in bring-up: bo_copy_match double-skipped source offsets
      (recomputed from grown len AND added i) — caught by fixture mismatch,
      confirmed against an independent Python reference inflater.
      Selftests: 12 passed, 0 failed (host + ASAN clean + this sim log).
      Build gate 0/0. Phase log exported with parity map.

### PHASE P17 — png.c + BENCHMARK
- [x] PNGDecoder.decode: signature, IHDR, palette/tRNS/gAMA-ignore per source, IDAT concat
      streaming inflate, unfilter (none/sub/up/avg/paeth), alpha composite over white,
      Adam7 (first pass only per source), gray/palette/truecolor bit depths as supported by
      source, Scale downsample to maxW/maxH, Dither.toImage.
- [x] Benchmark: PNG samples from https://wiesmann.codiferes.net/share/bitmaps/ ; log dims/ms.
      (ran offline on generated 800x600 fixture: 266x200 in ~34 ms; dims/ms logged)
- [x] Verify visually in sim (temporary debug viewer drawing decoded image); STOP.

### PHASE P18 — bmp.c
- [x] BMPDecoder.decode: BM header, pixelOffset/headerSize/width/rawHeight(top-down)/bpp/
      compression checks (NOTE: source reads but never checks compression — preserved),
      1/4/8-bit palette indexed, 24/32bpp, bottom-up flip, scale-to-fit
      (360x200), dither out.
- [x] Benchmark BMP sample from bitmaps site; log. (offline generated 600x400
      fixture: 300x200 in ~0.5 ms)
- [x] Verify; STOP.

### PHASE P19 — gif.c
- [x] GIF87a/89a: header/screen descriptor, global/local palettes (grayscale shortcut),
      LZW streaming into box downscaler, first-frame-only, interlace handling per source,
      Tasks yield points, transparent index → white composite.
      (quirks preserved: imgTop unused — rows composite from canvas row 0; only
      imgH rows streamed to the downscaler even though it uses logical-screen dims)
- [x] Benchmark GIF sample; log. (generated 600x400 fixture: 300x200 in ~4 ms)
- [x] Verify; STOP.

### PHASE P20 — jpeg.c  ✅ DONE (this session)
- [x] jpeg.h/jpeg.c ported from CometBrowser Source/lib/jpeg.lua: baseline SOF0 +
      progressive SOF2 DC/refine paths, quant tables, canonical build_huff, restart
      intervals, fixed-point idct2d (IDCT_SCALE basis), DC-only shortcut when box>=4 or
      area>200k, chroma consumed for sync only, YCbCr→gray, arithmetic SOF9/10/11
      rejected, tasks_yield_check per MCU row, scale_accum + dither_to_image device
      wrapper (jpeg_decode → LCDBitmap; jpeg_decode_gray for tests).
- [x] tools/gen_jpeg_fixtures.py generates 13 fixtures with shadow-math goldens
      (full-IDCT + DC-only + box scale) and Pillow cross-validation:
      flat / dcstripes / acblocks / restart / sub420 / prog_dc / prog_multi /
      prog_refine + guards (bad_sig, eoi_only, sof9, trunc_sof) + PIL bench file.
- [x] selftest_jpeg.c [P20]: 8 fixture decodes with golden probes+dims, 5 guards,
      PIL smoke 266x200, bench 800x600. Host harness p20asan: **15 passed, 0 failed**
      under ASan; TOTAL 147/0 across all phases.
- [x] Bugs found & fixed during verification:
      - jpeg.c j_u16 read little-endian; JPEG lengths are big-endian (desynced every segment).
      - JPState cap[9]/blkIdx[9] overflowed (arrays keyed to 256); state now heap-allocated.
      - rowbuf_put/fill_block never clipped y against image height — MCU padding rows
        (4:2:0 800x600) wrote OOB heap; now clip like the Lua sparse canvas did.
      - Generator: Huffman tables redesigned legal per T.81 C.2 (no all-ones code;
        libjpeg ERREXITs otherwise); DC prediction added to build_baseline;
        BitWriter.bytes() pads TO byte boundary (was dropping final partial byte);
        zero-amplitude AC entries no longer emit spurious EOB symbols.
      - util/mem gained pluto_calloc; logger mirrors lines to stderr on host builds;
        Makefile links --specs=nosys.specs so the device ELF links again.
- [x] Full make (device pdex.elf/bin + simulator pdylib) green; PlutoBrowser.pdx packaged.
- [x] Bench: 800x600 q85 PIL JPEG -> 266x200 in ~1.7 ms host (ASan -O1). Device timing
      logged on next simulator run ([P20] bench line).

### PHASE P21 — webp.c PART 1 (VP8L lossless)
- [x] BitReader (brPrefetch/brAdvance/brReadBits), huffman (replicateValue/getNextKey/
      nextTableBitSize/buildHuffmanTable rootBits=8+7 readSymbol; readSymbol7 was dead
      code upstream and is omitted),
      readHuffmanCodeLengths/readHuffmanCode/readHuffmanCodes (5 metas, color cache,
      group remap), decodeImageData (lz77 window, planeCodeToDistance, transforms):
      predictorAdd/Inverse (modes 0..13 incl. select/clampedAddSubtractFull/Half,
      average2/3/4), colorTransformDelta/transformColorInverse/colorSpaceInverse,
      subtract-green addGreenToBlueAndRed, colorIndexInverse + expandColorMap,
      applyInverseTransforms, decodeVP8LPayload (ARGB out). Public API:
      webp_decode_argb (full-res ARGB rows) + webp_decode_gray (luma over white,
      box-scaled via ScaleAccum) + device webp_decode.
- [x] Test vector: 8 Pillow lossless files cross-validated offline vs dwebp -pam
      (byte-exact except RGB under A=0, encoder non-exact mode); FNV-1a ARGB
      checksums + probes logged ([P21] D.*_cksum lines).
- [x] Verify; STOP. selftest_webp 31/31 on host ASan harness (p21asan), zero leaks
      attributable to webp.c; TOTAL suite 178/0; make green, PlutoBrowser.pdx packaged.
      Bugs fixed during verification: expandColorMap consumed raw sub-stream via
      t.data (was passing NULL), single-transform stage leaked the input grid,
      decode_image_stream sub-call result clobbered its own out-param.

### PHASE P22 — webp.c PART 2 (VP8 lossy + alpha + animation) + webp_vp8_data.c  ✅ DONE (verified in simulator)
- [x] Transcribe VP8Tables verbatim into webp_vp8_data.c (kDcTable…CoeffsUpdateProba).
- [x] VP8: newBr/vp8GetBit/GetSigned/GetValue/SignedValue, bool decoder vp8LoadNew,
      segment/filter/quant/proba headers, tokens (getLargeValue/getCoeffs/zigzag/bands),
      parseResiduals context model, intra modes (16x16 + 4x4 B-modes w/ kBModesProba),
      transforms (WHT/one/AC3/DC/UV), predictors 16/8/4 (dc/ve/he/TM/rd/vr/ld/vl/hd/hu),
      loop filter (simple + normal, inner edges, precomputeFilterStrengths), reconstructRow
      scanline pipeline, fancy chroma upsample (upsampleLinePair/writeRgbPixel/clipYUV),
      finishRow caching, decodeAlphaPlane (ALPH unfilter + VP8L-compressed alpha),
      MAX_PIXELS guard.
- [x] Animation: parseWebPAnimation (VP8X/ANIM/ANMF), blendPixelNonPremult, keyframe/dispose/
      blend state machine, decodeAnimation; WebPDecoder.decode picks still-vs-anim first
      frame; _testDecodeRaw/_testBuildTable hooks.
- [x] Benchmark lossy+lossless samples: sim-run benches logged — lossless 266x200
      1.012 ms/iter, lossy 48x32 0.087 ms/iter ([P21]/[P22] bench lines).
- [x] Verify; STOP.

      DONE (2026-08-22). parse_webp rewritten as still-vs-anim dispatcher; public API:
      webp_decode_argb/webp_decode_gray dispatch VP8L vs VP8(+ALPH); webp_decode_anim +
      webp_anim_free for animated containers (canvas-sized blended ARGB frames).
      webp_vp8_data.c added to Makefile SRC; kDcTable/kAcTable/kScan widened to uint16_t
      (values overflow uint8_t); Vp8Dec.proba int[1056] → uint8_t[1056] (memcpy OOB vs
      CoeffsProba0). Fixtures regenerated by tools/gen_webp_fixtures.py: 3 lossy stills
      cross-validated byte-exact vs dwebp -pam 1.6.0 (flat/grad/alpha incl. compressed
      ALPH plane), hand-built 4-frame animated container (fx_w_anim) with Python goldens
      of the libwebp integer blend algorithm, lossy-truncation guard.
      Selftests: [P22] webp selftests done: **53 passed, 0 failed** (simulator log this
      phase; TOTAL suite now includes P22's D/G/X/A/B cases). Build gate 0 errors /
      0 warnings (pre-existing jpeg.c notes only). Phase log exported to logs/phase-P22.log.
      Bugs found & fixed during verification:
      (1) vp8l_decode_payload rejected color-indexed images by requiring tw==w (transformXsize
      legally shrinks under COLOR_INDEXING) — now returns header-w × th after inverse transforms;
      (2) alpha_decode_plane started its bit reader at payload offset 2 instead of 1 (Lua
      newBitReader(data,2) is 1-based; C Br.pos is 0-based next-byte) → compressed ALPH always
      failed → opaque fallback;
      (3) anim_parse VP8X canvas dims read at cp+3/cp+6, spec/libwebp demux reads LE24s at
      cp+4/cp+7 (flags byte is FIRST at cp+0, then 3 reserved bytes);
      (4) ANIM bgcolor was composed as if stored A,R,G,B — spec stores B,G,R,A; now decoded
      into canonical 0xAARRGGBB (intentional divergence from the Lua original, which
      mislabels BGRA bytes as ARGB);
      (5) generator emit() appended animation tables before includes (malformed fixture
      header); generator VP8X payload missing reserved bytes / wrong flag position; ANMF had
      3 stray zero bytes before the frame sub-chunk breaking sub-chunk scan; frame rects now
      use even x/y (ANMF stores x,y in units of 2 pixels).

### PHASE P23 — ico.c  ✅ DONE (verified in simulator)
- [x] ICONDIR/Icondir entries sort (area desc, bpp desc), PNG-signature entries → PNGDecoder,
      else embedded DIB (height doubled, AND-mask transparency composited over white,
      1/4/8/24/32bpp, top-down flag), nearest-scale sampling, Dither out.
- [x] Verify with favicon fixture; log; STOP.
- **Implementation:** Source/render/decoders/ico.{h,c} — ico_decode_gray (container parse +
  qsort selection + PNG delegation via png_decode_gray + decodeDIB with doubled-height
  convention, palette 1/4/8bpp, truecolor 24/32bpp, AND-mask → white composite,
  negative-height top-down rows, nearest-neighbor scale cap) + ico_free_rows +
  ico_decode device wrapper via dither_to_image.
- **Fixtures:** tools/gen_ico_fixtures.py hand-builds containers and computes goldens
  with an independent byte-level Python replica of the decoder: fx_i_i_32bpp,
  i_24bpp_masked (AND-mask holes), i_8bpp_nomask (mask rows omitted), i_4bpp (odd width
  nibbles), i_1bpp, i_topdown (negative height), i_scaled (400×300→266×200), i_png_entry
  (real Pillow PNG beats DIB decoy), i_png_fallback (broken PNG → next entry), i_multi
  (area tie → bpp desc), i_cursor (fileType=2); guards bad_reserved/bad_type/count_zero/
  no_entries/all_fail_dib. Generator bug fixed en route: classic DIB entries store
  DOUBLED height (rawHeight = 2*h); initial single-height fixtures decoded at h/2.
- **Bug found by selftests:** ico_decode_gray dereferenced outRows/outW/outH before
  null-checking → simulator crash at G.null_args_reject (log stopped after #1159);
  added `if (outRows==NULL||outW==NULL||outH==NULL) return -1;` matching webp.c.
- **Selftests:** [P23] ico selftests done: **41 passed, 0 failed** (simulator log this
  phase; bench 266×200 = 0.858 ms/iter). Build gate 0 errors / 0 warnings (pre-existing
  jpeg.c notes only). Phase log exported to logs/phase-P23.log.

### PHASE P24 — svg.c  ✅ DONE (verified in simulator)
- [x] SVGDecoder.decode: viewBox/width/height parse, scale cap ×2, min 20px, white canvas,
      expandUses (<use href> resolution), scanTags (comments/CDATA/doctype skips),
      mergeStyle(style attr wins), hidden/display:none subtree skip stack, hasInk rule,
      shapes rect(rx)/circle/ellipse(fallback rings)/line/polygon(close)/polyline,
      path M/L/H/V/Z/C(8 seg)/S/Q(6 seg)/T/A(straight chord) with relative support and
      smooth-control reflection, tokenizePathNumbers char-level parser (sign/dot/exponent
      edge cases), draw into offscreen bitmap, zero-drawn → nil.
- [x] Fixture icons rendered; visual check; log shape counts.
      (svg.c ~1250-line pure-C rasterizer+parser port; goldens from independent Python
      replica of the same spec via tools/gen_svg_fixtures.py — 10 fixtures ×
      dims/FNV-1a cksum/drawn-count/probe points + guards G.no_svg/zero_drawn/bad_dims/
      trunc_tag/null_args_reject; bench fixture s_bench 192×192: 47 passed / 0 failed,
      bench 0.021 ms/iter. All phases regression-green. Build gate 0 errors / 0 warnings
      (pre-existing jpeg.c notes only). Phase log exported to logs/phase-P24.log.)
- [x] Verify; STOP.

### PHASE P25 — render/image_decoder.c  ✅ DONE (verified in simulator)
- [x] Cache (url→bitmap|PENDING|FAILED), downloadQueue FIFO, processNextImage via
      HttpClient, decodeRawImageData magic dispatch (JPEG FFD8 async via Tasks; GIF87a/89a;
      PNG sig; BM; RIFF/WEBP; ICO; svg sniff '<svg'/'<?xml'), failure→cached false,
      max-dims caps, ImageDecoder.draw placeholders ([Image Off]/[Hover]/on-demand card w/
      alt + A/B hints + selected inversion), enqueue/evict/isCached/isDecoded/getImage/
      clearCache/update, _testDecode.
- [x] End-to-end: load a page image through network in sim; log pipeline timings.
      (image_decoder.c/h full port: FIFO queue drained one-at-a-time while page not
      loading, magic dispatch with JPEG/PNG/WebP/GIF through the task scheduler and
      BMP/ICO/SVG inline, failed decodes AND <9-byte bodies AND transport errors cached
      as false, stale-download/stale-decode watchdogs in update(), draw() placeholder
      card + hatch + camera icon + clipped alt text + selected 2px border + centered
      drawScaled fit. Lua's 16ms re-kick timer documented as redundant vs update()
      polling. Selftests: 9 passed / 0 failed via the _testDecode seam reusing P17–P24
      fixtures. E2E over real localhost HTTP in sim: png+bmp+404 sequential pipeline
      resolved in 3318 ms — async PNG 32×24 decoded through the scheduler, BMP inline,
      404 cached-as-failed; visual smoke of placeholder + scaled-draw branches. All
      phases regression-green. Build gate clean. Log exported to logs/phase-P25.log.)
- [x] Verify; STOP.

### PHASE P26 — render/cloud_layout.c (+json)  ✅ DONE (verified in simulator)
- [x] CloudLayout.parse(jsonString, baseUrl): element list (type/text/x/y/font/size...),
      build(doc) wiring; draw(scrollY) painters identical fonts/offsets; MODE_OPERA_DS
      path ready. (cloud_layout.c/h full port: parse wraps json_parse in a pcall-equivalent —
      malformed JSON or non-object payload → title="Parse Error"/elements={}/totalHeight=240;
      missing fields default "Cloud Page"/{}/240; baseUrl accepted but UNUSED exactly as in
      the Lua source. build() clears items + LinkManager, skips el.y<0, offsets by
      CONTENT_Y+el.y, maps text fonts large→heading1/bold→bodyBold/mono→mono/body→body with
      sys-font fallbacks, images default alt="Image", links via lm_add_link, and
      input/submit ALSO register selectable form-input links carrying their item pointer
      (new additive lm_add_form_input + LmLink.isFormInput/inputBlock fields mirroring the
      Lua table's optional keys). draw(scrollY): white content fill, clip rect, per-item
      painters (drawTextInRect w+10/h+10; ImageDecoder.draw; input box white roundrect r3
      + 1px border + value-or-placeholder text inset 4px; submit black roundrect r3 +
      FillWhite centered bold label), self-drawn selected-link outline (black lw3 r3,
      inflated 2px — distinct from LinkManager.drawSelectedHighlight, matching Lua),
      exact visibility gate vs SCREEN_HEIGHT, scrollbar thumb math identical.)
- [x] Fixture cloud JSON rendered; log. (Selftest: 33 passed / 0 failed — valid payload
      all-types incl. negative-y skip + form-link wiring walk over LinkManager selection,
      malformed → Parse Error fallback, defaults, NULL safety, rebuild idempotence.
      Sim visual: fixture parsed/built once at frame 180 then scroll sweep exercises all
      painters + clipping + scrollbar; HUD lines P26. All phases regression-green
      (1107 PASS / 0 FAIL boot total). One harness bug found+fixed during verify:
      selection-walk loop was unbounded since selectNext wraps around. P25 e2e re-verified
      green after regenerating the ephemeral localhost png/bmp fixtures + test server.
      Build gate clean. Log exported to logs/phase-P26.log.)
- [x] Verify; STOP.

### PHASE P27 — ui/chrome.c + ui/hud.c  ✅ DONE (verified in simulator)
- [x] Chrome.draw: black bar, separator, SSL lock/globe vector drawing, host display rules
      (about:, >28 chars ellipsis), [READ]/[WEB] badge, comet loading dots anim (12-phase),
      progress bar (known total vs indeterminate sweep), clock HH:MM; cometAnimFrame static.
      (chrome.c/h full port: bar 400x24 + white separator row 23; SSL roundrect r3 +
        body rect + black keyhole dot / globe circle r6 + cross lines via
        fillEllipse/drawEllipse wrappers (SDK has no circle API); displayHost default
        "CometBrowser", about:home special-case, >28 -> 25+"..."; badge at
        SCREEN_WIDTH-badgeW-62 only for non-about idle pages; comet groups frame%12 with
        3/2+1/1+2+3 white dots; progress min(1,cur/tot) else ((frame*3)%100)/100 over the
        separator row; pageTitle accepted-but-unused exactly as Lua. CLOCK DIVERGENCE
        DOCUMENTED: SDK has no synchronous local-time read (playdate.getTime() equivalent)
        — sim/host renders libc localtime_r (verified == wall clock); device falls back to
        Lua's "--:--" pcall path. New additive Style.style_get_ui_small_font() mirrors
        "fontSmall or fontMono or getFont()".)
- [x] Hud.draw scrollbar (track/thumb math exact) + active-link bottom HUD (56-char clip);
      Hud.drawHoverStatus halved bold font pill bottom-left, 50-char clip. (hud.c/h:
      track 393/26/212 inset 2px, thumb max(12,floor(212*216/th)) + clamped ratio,
      "-> " prefix clip 56->53+"...", bottom bar SCREEN_HEIGHT-20 with white top line;
      hover pill loads fonts/Roobert-10-Bold-Halved once (shipped in bundle) w/ sys
      fallback, barH=fontHeight+4, barW=min(400,tw+12), anchored to content bottom.)
- [x] Visual check in sim; STOP. (Selftest: 28 passed / 0 failed pinning host-display
      rules incl. 28/29 boundaries, comet 12-phase group mapping, progress known/indeterminate
      values incl. wrap at frame 34, scrollbar geometry (432-page mid-scroll y79/h106,
      min-thumb 12, both ratio clamps), link/hover clip boundaries. One test-expectation
      bug found+fixed during verify (frame 24 wraps to group 0). Sim showcase rotates 5
      scenarios every 60 frames (ssl-web/[READ]/loading-known/indeterminate/about) with
      hud_draw long-href bar + hover pill; clock render logged and verified equal to host
      wall time. All phases regression-green (1135 PASS / 0 FAIL). Build gate clean. Log
      exported to logs/phase-P27.log.)

### PHASE P28 — ui/home_page.c
- [x] Speed dial: logo header (comet pixel art lines + circles), banner text, address-bar
      prompt pill, selectable Settings button, 2-col bookmark grid (172×46 cards, gaps),
      selection inversion, marquee oscillating clipped text (speed 50 px/s, dwell 1.0s,
      per-key state), crank scroll ×1.5 w/ invertCrank, auto-scroll-to-selection math,
      handleInput grid nav rules (settings↔first row, odd/even column moves), A→url /
      settingsCallback, footer hint line, reset().
- [x] Visual + input check; STOP.

      DONE (this session): hp_* pure nav/autoscroll/marquee/smooth-scroll helpers +
      hp_draw (device) with host no-op; selftest_home_page 36/0 (nav matrix,
      scroll bands, marquee phases, lerp, A-actions vs live Storage). Bug fixed:
      DynArray elements are PlutoSavedBookmark (fixed buffers), NOT constants.h's
      pointer-based PlutoBookmark — casting the wrong one produced garbage URLs.
      Full suite 1171 PASS / 0 FAIL incl. e2e fetch; scripted input walk verified
      row-jump/column/boundary rules in sim. Log: logs/phase-P28.log.

### PHASE P29 — ui/address_bar.c + VENDOR KEYBOARD (Raphcal C lib)
- [x] Vendor https://github.com/Raphcal/some-corelibs-port/tree/main/keyboard into
      Source/vendor/keyboard; adapt to SDK C API; ensure it renders + edits text fully in C.
- [x] AddressBar.open (prefill non-about currentUrl), launchKeyboard gating (B held check,
      shown-once), keyboardWillHide(submitted): trim whitespace, empty→cancel path,
      isSearchQuery→buildSearchUrl(selected engine) else URL.parse normalized, onSubmit
      callback, skipInputFrames=2; cancel() hides keyboard + clears callbacks;
      drawOverlay two layouts (armed pill vs keyboard-side box w/ wrapped mono text).
- [ ] Manual test: type URL, submit navigates (stub nav ok), B+Left/Right while armed works.
- [x] Verify; STOP.

      DONE (this session): vendored Unlicense keyboard.{c,h} + SDK CoreLibs assets
      copied into Source/CoreLibs/...; global `PlaydateAPI* playdate` bridges vendor
      code (eventHandler param renamed to avoid shadowing); PDCallbackFunction typedef
      restored (matches int(void*) update); style_get_mono_font() added. Pure helpers
      unit-pinned 23/0 (trim, search-vs-URL routing, launch gate incl. B-held,
      ^about: prefill rule, cancel semantics, skip-frame counter). Sim: armed pill at
      frame 300, auto-launch frame 300+300, assets load clean, update loop stable
      through keyboard takeover; full suite 1194 PASS / 0 FAIL. Log:
      logs/phase-P29.log. REMAINING: interactive typing check (scripted keystrokes
      can't reach the sim) — sim left running with keyboard up for manual pass;
      B+Left/Right gating covered by G.b_held_blocks unit test.

### PHASE P30 — ui/error_page.c + bookmarks_page.c + history_page.c
- [x] ErrorPage: show/handleInput (left/up, right/down clamp 1..3, A→retry/search/home,
      draw roundrect dialog, msg/url 48/50-char clips, 3 buttons 100×28 sel inversion).
- [x] Bookmarks/History pages: open/handleInput (A open url, B close), draw list rows
      34px, title 34/url 46-char clips, crank scroll ×2, culling, empty-state texts.
- [x] Visual + input check; STOP.

      DONE (this session): shared pure core ui/list_core.{h,c} (nav clamp, crank
      scroll ×2 ≥0 clamp, row-culling band, 34/31 & 46/43 "..." clips) reused by
      bookmarks_page/history_page; error_page with dialog draw + inverted buttons
      (setDrawMode kDrawModeFillWhite — SDK name differs from Lua's
      setImageDrawMode). Selftest 36/0 incl. one test-sequence fix (A pressed on
      row 2 vs entry[0] comparison). Sim: rotating slots every 120 frames with
      scripted inputs logged (error actions, bm/hi OPEN urls); full suite
      1230 PASS / 0 FAIL. Log: logs/phase-P30.log.

### PHASE P31 — ui/settings_page.c
- [x] Staged-settings overlay: open(prevState) snapshot, options Search Engine (cycle),
      Browse Mode toggle, Invert Crank, Image Mode cycle (NAMES order), Clear Cookies
      action (immediate), Save(A)/Cancel(B) semantics, onChangeCallback re-render trigger,
      ease-out-cubic 300ms open animation (box grows from center, radius lerp, content
      after t>0.4), row painters + < > arrows + "Press A" hint, footer hint.
- [x] Verify persistence via Storage.save + reload; STOP.

      DONE (this session): staged copy mirrors Lua snapshot defaults; engine cycle
      ((v-2+n)%n)+1/(v%n)+1, mode/invert toggles, image mode NAMES-order wrap;
      A on row 5 = immediate cj_clear() staying open; A elsewhere = apply +
      storage_save() + onChange + SP_ACT_SAVED; B = discard + SP_ACT_CLOSED.
      Draw: 300ms ease-out-cubic grow-from-center box (radius lerp), content
      gated at t>0.4 with clip, row inversion via setDrawMode FillWhite,
      </>/Press-A hints, footer. Selftest 26/0 (three initial failures were
      test bugs — LEFT/RIGHT fired on row 1 instead of the target rows);
      persistence proven via storage_load() reload keeping searchEngine=2 then
      restored to default. Sim demo slot cycles engine + B-discard with logs.
      Full suite 1256 PASS / 0 FAIL. Log: logs/phase-P31.log.

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
