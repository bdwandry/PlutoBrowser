# PlutoBrowser Platform Scorecard

**Durable reference — "where we are."** Update in place; scores move only with
receipts (log rows, suites, deploys). Date-stamp every change at the bottom.

- Snapshot date: **2026-10-18**
- Branch snapshot: `develop_browser_fixes_2026-10-18` @ `7305057`
  (parent = main tip `1d1e82e`, includes the full AVIF decoder)
- Production device build at snapshot: `9dd105636542bc8f42c00e052ab74cbb`
- Engine: **XS (No Recursion) fork** — the ONE JavaScript interpreter for every
  site in every mode of this browser.

## Scorecard

| Area | Score | Status | Grounded by |
|---|---|---|---|
| JS language (XS NR engine) | ~95% | NOT STARTED | Real react bundles run end-to-end (TAP2 ×3, `jsErrors=0`); jsext suite 8/0; fork survives its own stack-limit engineering |
| Engine's browser API surface | 100% | COMPLETE | DOM read/write, events **(receipted end-to-end Phase 1: submit/input/change dispatch, bubbling to document listeners, preventDefault honored)**, fetch **(real + receipted Phase 2: promise shape, local+wire receipts, setRequestHeader Content-Type wire lane, currentScript asserted)**, the page's own address **(receipted Phase 3: live location getters, pushState/replaceState in-engine journal with popstate/hashchange back-stack delivery, document.title set/get, pushState never navigates — `apihist: pass=7 miss=0`)**, console **(receipted Phase 4: every level tagged into the log file — `[console.log/warn/error/info/debug]` rows with printf substitution (%s/%d/%o/%%·out-of-args-literal) + the console-shape renderer (nested objects/arrays, `[1, 2, 3, 4, …5]`, depth-2 bounded) + assert browser semantics (truthy silent / falsey error row, never throws) + the panel stays clean — `apicon: pass=8 miss=0 sent=17`, `P4COUNT 17` = 16 rows + the documented silent assert)**, ES modules **(receipted Phase 5: the loader-side module lane — both scanners flag `<script type="module">` and the router transforms import/export in C before ANY engine sees the source, so every engine gets the same lane with the vendored engines untouched: IIFE module scope, import-first graph on a per-document ledger (cycle/depth/cap receipts), namespace objects `globalThis.__p5mod[<resolved URL>]`, named/default/star import forms, `export const/let/var/function/class` materialized after the body runs; imports resolve module-RELATIVELY through url_resolve (the journal's resolver) and fetch on the page's own roads — the `about:` store and the `file:` Data-dir loader rule with the flat file-road join; probe receipts `apimod: kids=3 refuse=PASS` (modin inline runs, modabs named bindings, modstar `* as ns`, and the path-only import on the opaque about: base refusing exactly as browsers refuse it — `./p5x.js` throws its honest load error), and the `file://modpage.html` bar proves the relative chain `import './p5rel.js'` → nested `./p5rel-child.js` runs children before the importer (`modpage: pass=2 miss=0 kids=2`); documented boundaries: export default/{list}/* cut whole with receipts, dynamic import() untouched, data:/http(s) module fetches refused synchronously, template ${} contents not re-lexed)**, DOM conveniences **(receipted Phase 6: `template.content` with the browser shape — the parsed template element stays reachable while its children parse into the DETACHED `#template-content` fragment (nodeType 11, parent-NULL: inert to getElementById/render/census; `content` materializes empty for created templates), `el.closest()` through the shared SW5 selector machinery (document-order self-then-ancestors, null for unusable selectors), `el.dataset` = a fresh camelCase snapshot per access (setAttribute is the write road), the append family returning its touched node per MDN — probe receipts `apidom: pass=6 miss=0` and the P0 `template` row flipping into `apisum: pass=12 miss=2` on all gates; the phase also repaired the id road — one shared O(depth) `dom_find_by_id` replaces the adapters' push-all-children getElementById walks that aborted whole once a node held 65+ element children, silently hiding every later id on wide bodies)**, timers, localStorage/sessionStorage **(receipted Phase 7: the shared webstorage road — localStorage persists per origin across boots, sessionStorage per run; origin rule `http(s)://authority` / one `file://` bucket / per-URL `about:` / opaque `"null"` fail-store; 4KB quota throws `QuotaExceededError` and stores nothing; `storage: PASS` + `[webstorage] quota:` + `visits` counter receipts on all three engines)**, workers **(receipted Phase 8: the contained-stub lane — the constructor SHIPS so `new Worker(url)` never dies on a ReferenceError; a stub constructs silently with the shape-complete object (`postMessage`/`terminate` functions, `onerror`/`onmessage` null) and tells the page via the standard async `onerror` path at the next pump with `ev = {message}`; the worker script never compiles or runs — `worker: PASS` + `[p8w] worker onerror` + `[worker] onerror delivered #1 present=1` on all three engines, `apisum: pass=14 miss=0` on XS)**, wrapper identity + the className accessor **(receipted Phase 9: the browser identity model now on every engine — ONE wrapper object per DOM node for the page's whole life, cached per node in a bridge-scoped table with engine-held refs (mujs registry keys unref'd at close; duktape stash-hidden heapptr pins; XS NR keeps its own P1-lane state arrays) so strict `===` chains and page-written expandos survive getElementById/parentNode/appendChild/insertBefore returns — the P6 apidom identity rows `closest`/`appendret` flip MISS→PASS on mujs and duktape — and `el.className` ships on all three engines through one shared write road (the classList family's snapshot/dirty-receipt contract, store identical to `setAttribute('class')`), `apidom: pass=7 miss=0` on all gates after the 7th `classnames` row)**, async http(s) module graphs + dynamic `import()` **(receipted Phase 10 — the API surface CLOSES: transport in the router against the single-flight client (`http_is_loading()==0` head + ~20s/~40s watchdogs; `http_get`'s 0=refused/1=started contract corrected against receipts), the bridge-scoped `modFlight[6]` table (epoch-tagged, parents parked, drain = the standing `mod_finish_run`, completion cascades by key — receipts `net: enqueue remote … (depth 2)` → `parked … on 1 remote child(ren)` → `delivered … bytes status=200` → `drain` → `cascade`), dynamic `import()` via `__p5di(` rename spans at any brace depth + the ES5 page shim `globalThis.__p5dynimport` + the native `__p5modRequest(spec,base)` per engine (`k:/p:/x:` strings, dynq tickets settled at the module pump, never re-entrant), `export {local as exported}` list exports now materialize (aliases included) while `export {list} from` stays receipted-boundary, probe `apimod: kids=7 refuse=PASS` with moddyn/moddynid/modmiss/modnet rows (modnet = the pinned punycode CDN fetched over https and its 6 named exports asserted; conditional-PASS on failure), file gate `modpage: pass=3 miss=0 kids=3` on the full static file→file→https chain with `reuse` discipline; frozen engines feature-detect and write honest `MISS` rows (`apimod: kids=5 refuse=FAIL` on mujs/duk, `apisum pass=10 miss=4` unchanged) and the P10 body-error receipt lane exposed a standing silent mujs death — muJS lacked `globalThis`, fixed with the ES2020 identity alias, the P5 chain now alive; the file-loader tolerates a stray trailing slash on file referents, another diag-caught real bug)**. Missing: none — the API surface set is closed; the honest non-features (service workers, IndexedDB) are policy-documented rather than faked |
| HTML + CSS | ~95% | COMPLETE (residuals = documented device policies) | Phases 1–10 closed: selector language, external stylesheet fetch, `var()`/`calc()` resolution, full `@media` evaluation (MQ4/5 + link attrs + the standalone `css_media_holds` API now feeding `<source media>`), the spec-anchored positioning lane (`position`/offsets/`z-index` consumed), the overflow lane (visually-hidden recipe suppression, `text-overflow: ellipsis` + `-webkit-line-clamp` real line cuts, region refs as honest no-ops), the flex/grid container lane (`display:flex/grid` containers, gap mounts on the halved-px scale with rem/em fraction precision `.75rem` = 12px exact, `align-items/align-self: center` on real containers, the whole distribution family consumed), P7's landed generated content (`::before`/`::after` printing inline with quotes/attr()/escapes resolved, `::marker` feeding the li bullet path, `:root`/`:target`/`:disabled`/`:enabled`/`:checked`/`:placeholder-shown`/`:empty` honest matchers, the consumed-inert `::`-family + `:focus-visible`/`:focus-within` joining documented policies), P8's interactive set (spec-srcset candidate selection with the panel-width policy live on real Wikipedia thumbs, `<picture>` source ordering by media=, working reset buttons with walk-time default capture + the browser click→reset activation order, multiple-select accumulation + real name-repeated serialization, the summary-less details default label), and P9's fidelity polish (the shared `fresh_len_px` unit reader across every walk-side length consumer — rem margins land, `"50%"` margins refuse, vmin/vmax/ch/ex/pt widen the Q namespace, `@media` consumes vmin/vmax; CSS translate() = REAL paint offsets mounted per-block at the ten block-open lanes with % keyed to the element's own box and the inline-replaces-whole-value cascade, rotate/scale/skew/matrix/transform-origin = consumed census vocabulary; gradient dark-hero polarity now rides background gradients on the sheet route — dark gradient heroes invert) — probe v9 `PASS (0 fails)` (105+ rows: doc-field mounts, content/state battery, srcset picks + msel/reset model asserts, tf battery + invert/pct-margin rows, 120vmin MQ row). Gaps: generated text stays text (no per-line/per-char styling — `::first-line`/`::first-letter`/`::selection` = consumed-inert policy names), real 2-D distribution + raster transforms (rotate/scale/skew/matrix) = documented policy non-features (a 2-D compositor is the only honest home — trajectory line keeps them there), translate skips form widgets & separators by policy, focus states remain the always-match family (device truth: no persistent focus), template.content & the JS DOM surface stay out of the bridge (JS-phase item), `select size>1` = a one-line field (no multi-row widget on the flat panel), multiple-select deselection = the reset button (no pick-list UI); and P10's declarative end-event lane (`@keyframes`/`@-webkit-keyframes` registries (ident-only names, >24 chars refuse = the animation stays inert, never silently mis-fires), the `animation-name` value resolver (first registry-known token, numeric/functional/keyword-skipping), the `walk_anim_engage` companion at every block-open lane with the infinite/zero-count/paused guards and a per-(node,name) exactly-once dedupe map, `animationend`/`transitionend` delivered post-render as kind-4 listeners from an addEventListener vocabulary that exists for real in both element and document paths, `transitionend` armed by the walk-visible gate attributes (class/id flips noted through the JsBridge mutation-notes ring at nine tree-mutation sites, gated on the winner being non-`none`)) |
| Forms & interaction | ~85% | NOT STARTED | The R56g–R58 arc: tap → typing → validation → in-place submit → POST → sent card → "Send another" restore, TAP2 baseline exact ×3 + device use |
| Network | ~70% | NOT STARTED | Real HTTP client (POST/cookies/redirects/timeouts), snap cache, spill-to-disk bodies |
| Media (canvas/video/audio) | <10% | NOT STARTED | Not built; sites stub out containedly, never crash |
| Robustness under pressure | recently hardened | NOT STARTED | Wikipedia device crash chain fixed (R59 re-walk contract + `break_lines` guard); about:jsext monolith contained (R60 residency + chunked routing); failures are absence, not brittleness |

## The engine contract (how any property talks to XS NR)

- **Door 1 — engine → browser (the bridge):** everything a script cannot do
  alone goes through `jsbridge_xs_nr.c` = the complete vocabulary: document
  reads/writes, event dispatch, fetch/XHR, timers, listeners, storage.
  Tree-mutating services set `domDirty` → renderer re-walks (max one per frame).
- **Door 2 — browser → engine:** the browser only ever starts things and
  delivers events: inline + external script tags (≥64KB bodies: disk-resident →
  per-segment chunked run), tap/typing events into handlers, per-frame parked
  script slots (one slice per 30fps frame).
- **Single-threaded cooperative pump, fuel-capped** (`budgetLeft` on close).
  Script state lives *between* frames. Absent properties return `undefined`
  containedly → in-page error card, never a browser crash.

## Standing policies (do not drift)

1. **Pressure-driven building:** features are built when a real page needs
   them (Wikipedia → R59; bryanwandrych.com → R56g–R58; jsext suite → R60).
   No checklist-driven pillars.
2. **Data, not code, for site knowledge:** hosts ship endpoints/validation as
   `form_sites.txt` profiles; zero site names in C. Generic machinery first:
   native `action=` forms need no profile at all.
3. **Containment over crashes:** every failure path must land in-page
   (error card / recovery), never page-swap the browser down.
4. **The 16MB / 30fps envelope is a design constraint**, not an excuse: big
   bodies go disk-resident, scripts park per frame, budgets arm on revisit.

## Missing-property watchlist (build-when-pressure order)

| Candidate | Trigger that would build it | Status |
|---|---|---|
| Scripted navigation (history/location, SPA routing) | First real SPA site | not built |
| CSS layout richness (flexbox/grid depth, media queries) | First layout-heavy page that misrenders | partial |
| Canvas 2D | First visual/graphics page | not built |
| WebSockets / EventSource | First live-data page | not built |
| Web workers | First site that blocks on compute | **contained stub shipped, API-P8** (constructor + onerror receipt lane) |
| Video/audio playback | First media page | not built |
| Iframes as documents | First embed-heavy page | not built |

## Active roadmap TODO — HTML+CSS → 100% (usage-weighted)

Legend: `[ ]` open / `[~]` in progress / `[x]` done (receipts logged below).

**Phase 0 — instrument & baseline** *(CLOSED 2026-10-19)*
- [x] 0.1 TODO list persisted (this section)
- [x] 0.2 Skip-class counters in `css.c` — 7 classes, per-render `[css-skip]` row, reset-after-emit. **Verified exact** on the probe; zeros where real pages feed us no CSS; Wikipedia `prop=41`.
- [x] 0.2.5 Known-property vocabulary audited (~27 names: 12 via `apply_prop` — including `color: white → invert`, `font-size` bucketing, `text-decoration`, `text-transform`, `white-space` — ~15 via query sites incl. `content`). Table lives in `css_known_prop`.
- [x] 0.1b `about:cssprobe` fixture v1 (route + bookmarks + snap bypass; zero-JS; two functional canaries). Harness: `about:` URLs skip snapshot-revisit phases.
- [x] 0.4 Baseline + receipts. Two NEW receipts: **(a)** pseudo-element gap — `::before/::after` lanes wired (`css_pseudo_content`, walker calls at document.c:5641) but content never lands in rendered text (probe `>>` canary silent) → Phase 7 target. **(b)** external `<link>` stylesheets unfetched — now *receipt-level*: bryanwandrych.com (all CSS external), Hacker News (2025 redesign = one `news.css` link, ZERO inline) → engine never spins up, `compute=0`; only Wikipedia ships inline rules.
- [x] 0.4.1 **TAP2 anomaly: SEALED by experiment.** Off-instrument build (`PLUTO_CSS_SKIP_TALLY=0`, verified absent from binary) reproduces the abort byte-for-byte: same `-2576B` timer-callback stack abort, same ladder (2474→7238KB aborting seg 5–8 across runs inside the 6.8–7.6MB zone), identical close row (`ran=17 errs=1 listeners=36 timers=3 budgetLeft=1461`). Verdict: site content + host-malloc variance at the engine-wall margin; containment worked every time. Instrument is exonerated both by construction (no XS-arena cost, not in timer scopes) and by the experiment.
- [x] 0.5 First real-page usage pass (sim-only): **Wikipedia** = `prop=41`/cycle, all other classes 0, 2367 compute cycles / 1.49M rule tests / 18ms, 688 blocks green post-R59. **bryanwandrych.com + /contact** = engine absent (external CSS). **HN** = engine absent (`news.css` external). Probe = all 7 classes exact.
- [x] Regression: **tap1 ×3 GREEN** (identical `inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` — 2026-10-18 23:48 + 2026-10-19 11:30 + 11:31). TAP2 cannot complete its cycle baseline while the site home page breaches the wall (content-side; contained; forwarded as the R61 wall-margin engineering item — NOT a browser defect).
- [~] 0.3 TEMP Wikipedia-stall counters — REMOVAL PLAN (awaiting explicit yes): delete lines 27–36 (`g_cssComputeCalls`…`g_cssWinTicks12` statics) + the `[css-stats]` logger_log body in `css_log_stats` (keep the function for the `[css-skip]` emit, or drop the whole function + its document.c:5933 call + css.h:297 decl if BOTH rows leave). Zero behavior impact; probe run re-verifies exact tally afterward. One word and it's one edit.
- [x] 0.6 Phase-1 lead upgrade: extend `prop=` to record the missing NAMES (Wikipedia's 41 would become a concrete list = the Phase 1–9 priority data). **LIVE**: `[css-propnames]` row (top-5 names × hits, gate-stripped like the tally). Wikipedia's 41 now resolves to `list-style x2 clear x2 flex-shrink x1 border-color x24 counter-reset x1` (shorthands + flex/counter gaps = the P2/P4/P6 input data).

**Phases 1–10** (gates: probe section PASS + regression green + scorecard needle)
- [x] P1 Selector depth *(M)* — **CLOSED 2026-10-19.** Phase-0 audit showed child `>`, `*`, `:nth-child(An+B)`, `:first/:last/:only-child`, `:link`, `[attr]`/`[attr=value]` were ALREADY implemented. This phase BUILT: sibling combinators `+`/`~` (bounded backtracking search off the proven ancestor-walk fast path), attribute operators `^= $= *= ~= |=`, simple `:not()` (CSS3 one-simple-token shape), and the interactivity-state policy **(a) always-match** for `:hover/:focus/:active/:visited` (a 1-bit panel has no hover; refusing hid desktop-menu content forever; reversible by one parse line). Receipts: probe v2 `[css-probe] PASS (0 fails)` ×2, `[css-skip] sib=0 attrOp=0 pcls=0 medF=2 var=1 calc=1 prop=5` exact, tap1 ×3 identical green, Wikipedia re-run delta-0 (prop=41 BLOCKS 688/672, ms=18 — unchanged cost; the new selectors appear ZERO times in Wikipedia's inline sheets). Engine cost: `CssSimple` +8B (pointer into sheet text + 3 bytes) ≈ +65KB worst-case per-doc engine allocation, freed at page swap.
- [x] P1.5 External stylesheets *(M)* — **CLOSED 2026-10-19** (user: "completely finish a phase" — this was the Phase-0 usage-pass gap "external `<link>` unfetched → engine never spins on 2/3 test pages"). Render-task Stage C (pre-parse, runs BEFORE the script-prefetch lane, independent of the JS setting → JS-Off pages get their CSS): `css_ext_scan_links` (rel=stylesheet screen, media/type screens, comment-span + `<script>`-body skips, entity decode, dedupe, cap) → `url_resolve` → sequential single-flight fetches (owner-flagged collector: late strays after a teardown drop unclaimed, never misattributed) → bodies join at parse (inline `<style>` keeps the tight slots; `CSS_MAX_SHEETS` 8 total). Caps: ≤6 sheets/page, ≤96KB RAM total. Bodies doc-owned, freed in `document_free` AFTER the engine (rules point into them). Receipts: probe `PASS (0 fails)` tally + prop row byte-identical + ZERO `[css-ext]` rows (about:-guard); tap1 ×3 identical green (12:46/12:48/12:49) each with `ok …/1q690o9k3mn37.css 7618B` + `sheets joined: 1` — real CSS on the contact page, form flow untouched; Wikipedia: 2 links scanned, `load.php` trimmed at exactly 98304B (the cap), 2nd link drained-by-cap (log row added), blocks=688 links=672 `prop=41` + names gauge **unchanged with real CSS loaded** (zero render delta); HN `news.css` 7418B joined, fresh+rewalk stable (blocks=10 links=230); bryanwandrych.com home joined, 4 cycles stable (blocks=48 links=18). Note: HN's own inline script TypeError (`byClass`) pre-exists, out of scope.
- [x] P2 `var(--x)` + `calc()` *(M)* — **CLOSED 2026-10-19.** Engine: custom-property table on the engine (`--name` declarations intercept `add_prop` BEFORE rules/gauge; last-wins with importance-beats-order; 24 entries × 11-char names; values point into sheet text) + a LOOKUP-TIME resolution layer (`css_resolve_value`: var substitution with balanced-paren fallback scan, then whole-value calc evaluation) wired at the `winning_core` exit AND `apply_prop`'s consume — untainted values keep their original sheet-text pointers byte-for-byte (parity: pages without var/calc resolve NOTHING, not even a copy). Calc: recursive descent grammar expr/term/factor (paren depth per level, cap 8), milli-unit fixed point (±9999 units), px/%/unitless, canonical "Npx"/"N%" output; other units + mixed-unit +/- + composites refuse (raw flows). Refuse keeps the raw value everywhere (same visible behavior as pre-Phase-2). Scratch: 4-slot 72B ring (paired wv/sv, dv/fv lookups fit); overflow aborts resolution (no truncation). Receipts: probe v3 `PASS (0 fails)` — canaries: VAR-HIT absent (walker-side resolution hides), VAR-KEEP present (no leak), VARFALL-HIT absent (fallback none), `calc-width=64px` OK (30+34px direct engine query), `var-display=none` OK; tally exact `sib=0 attrOp=0 pcls=0 medF=2 var=3 calc=2 prop=5` (census counts var/calc TEXT; canaries assert RESOLUTION); tap1 ×3 identical green + live `7618B` fetch receipts; Wikipedia `blocks=689` (+1 vs the 688 baseline — FIRST live-page render delta from resolution; links 672/prop=41/names unchanged, no stall); HN + bryanwandrych.com home unchanged with sheets loaded. Checker note (own bug, self-caught): the first build's direct-query rows shared one inert node marker — the winning mask caches per (node, lane), so the second query was served the first's mask; one marker per query fixed it. Checker second note: `:root` is a refused pseudo-class here, so fixture `--hd` rides a plain class (v1: custom properties join the doc table in sheet order, not per-element).
- [x] P3 Media queries *(S–M)* — **CLOSED 2026-10-19.** General-purpose `@media` evaluation, parse-time (`flatten_media` decides hold/drop once; device is static). Grammar: comma-list OR (`media_holds` paren-aware), `not`/`only` prefixes, `and`-chains (multi-feature after a type verified), feature-first queries; malformed → false. **Full MQ4/5 feature vocabulary against a documented device truth table** (css.c `eval_media_feature`): viewport `400×240` exact milli-px compares (`min/max/exact` × width/height/device-width/height), px/em/rem (CSS 16px base)/vw/vh + the MQ4 unitless-zero exception; `orientation` landscape; `aspect-ratio`(+device alias, bare-number or `A/B`, cross-multiplied integers); `resolution`+`device-pixel-ratio`+`-webkit-*` (dppx=1 so 2x-asset branches stay false — small files serve, the right envelope call; dppx/dpi/dpcm/x units); `prefers-contrast: more` TRUE (the panel IS max contrast); `prefers-reduced-motion/transparency` `reduce` TRUE; `forced-colors: active` TRUE; `hover: none` + `pointer: coarse` (phone-parity input, so touch-first layouts apply); `color-index/grid` 0-truth, `scan progressive`, `scripting enabled`, `display-mode standalone`, `inverted-colors none`, `update fast`, `dynamic-range standard`, `overflow-* scroll`, `environment-blending opaque` — bare-keyword booleans per spec. Unknown names still census (medF) + evaluate false (containment unchanged). **Link `media=` attribute** now runs the SAME evaluator (`media_holds` fwd-decl from `css_ext_scan_links`) — `media="print"`, `media="screen and (max-width: 700px)"`, comma lists and negations all fetch-decide honestly (replacing the all/screen substring screen; `media=""` stays every-medium). Receipts: probe v4 `PASS (0 fails)` — 9 new canaries (MQ-HIT min-width held→hidden, MQ-KEEP 9000px dropped, MQEM-HIT `22.5em`=360px em-lane, MQAND-HIT unitless-0+landscape chain, MQNOT-KEEP negation, MQOR-HIT comma-OR, MQCOLOR-KEEP light, MQCONTRAST-HIT more TRUE, MQUNKNOWN-KEEP containment), tally exact `sib=0 attrOp=0 pcls=0 medF=1 var=3 calc=2 prop=5` (medF 2→1: prefers-contrast now known), names gauge unchanged, all Phase-1/2 canaries byte-stable; tap1 ×3 identical green; Wikipedia receipts PIN-stable (689/672, prop=41, medF=0 — desktop skin's min-width breakpoints correctly false at 400px; negative-direction containment proven live); HN + bryanwandrych.com unchanged incl. rewalks (their sheets' real `@media` rows now evaluate instead of blanket-dropping; no wall perturbation). Plain production restored (strings clean, boot green, 39KB idle).
- [x] P4 Positioning & z-index *(L)* — **CLOSED 2026-10-19.** Replaced the css.h "all static" policy note with a **spec-anchored positioning lane** (general-purpose, real-site-receipt-validated before implementation): the engine consumes `position` (apply_prop: fixed/absolute → `CSS_F_OOF`), offsets (`top/left/right/bottom`, any non-auto value → `CSS_F_OFF` — 0px IS an offset, auto/inherit/initial/unset carry none) and `z-index` (numeric value → `CSS_F_OZ`; invalid declarations carry no signal, CSS-correct). Policy at the walker's skip gate: **out-of-flow WITH an overlay signal = page chrome → dropped** (it would render ON TOP of the document in a compositor; the flat 1-bit flow has no overlay layer — Firefox/Safari reader-mode precedent, reversible one-liner); **absolute/fixed with a clean static position (no offset, no z-index) flows exactly where written — that IS the CSS static-position rule**; relative/sticky keep flowing (sticky chrome scrolls away instead of eating the 240px viewport — the honest 1-bit mapping); **z-index on a static element is inert** (CSS-correct: no effect without a positioned box). Real-site receipts gathered BEFORE implementation (curl of both gate-site sheets): HN zero positioning (lane provably inert there); bryanwandrych.com — 6 absolute + 1 sticky + 3 z-index: the `.skip` accessibility link and off-screen `.hp` helper (left:-9999px chrome) suppress = engine fidelity UP (they rendered as stray blocks before), the `::before` pseudo-rule untouched (correct lane), the mobile nav overlay (already display:none before toggle) no net change, sticky nav + relative wrappers flow. Names gauge: position/top/left/right/bottom/z-index all CONSUMED (leave `prop=`; probe tally prop 5→4 exact). 6 new probe canaries: FIXED-HIT/ABS-HIT/ABSOFF-HIT dropped, ABSSTATIC-KEEP (the static-position rule), ZKEEP (z alone inert), REL-KEEP — probe `PASS (0 fails)` full battery (39 rows). Gates: tap1 ×3 identical green; **Wikipedia pin-stable 689/672** (the vector skin's positioning declarations largely live inside already-evaluated media blocks — the Phase-3 lane prunes them before census; cross-phase synergy); HN blocks=10 with links 230→229 = upstream front-page drift (curl-proven: zero positioning surfaces → the lane cannot alter that page); bryanwandrych.com home 48/18 stable incl. rewalks. Plain production restored (0 errors, strings 1/0, boot green, 39KB idle). Out of P4 scope, documented: floats stay census-only (P6 flex/grid supersede them on modern sites; 2-D compositor semantics do not exist in the flat-flow renderer).

- [x] P5 Overflow regions *(M)* — **CLOSED 2026-10-19.** The flat page IS the only scroll region — nested scroll regions cannot exist — so the overflow family is consumed honestly: `overflow: auto/scroll/visible` = inspected no-ops (content always fully visible; the reader stance — a containment wrapper like the BW sheet's `.role { overflow: hidden }` keeps every word, canary-proved). Three author-intent behaviors went live: ①**the W3C "visually-hidden" recipe drops** — out-of-flow (fixed/absolute) + `overflow: hidden` + a concealing clip shape (`clip-path: inset(N...)` with any nonzero draw, legacy `clip: rect(...)`, new `CSS_F_CLIPN`) or a 1px-ish box (the recipe's other standard completion: width & height both ≤2px via the established `walk_box_px` hint query, `tiny_hidden_box`) is INVISIBLE on the real web, so the walker drops it (author-intent; BW's own sheet carries the recipe verbatim in `.card-hit>span`: `clip-path:inset(50%);white-space:nowrap;width:1px;height:1px;position:absolute;overflow:hidden` — previously rendered as duplicate text by our engine); partial patterns (absolute+hidden WITHOUT clip/tiny: BW's `.srb` boundary canary) keep flowing — normal-size `overflow: hidden` is NOT a hiding signal. ②**`text-overflow: ellipsis` + `white-space: nowrap` cuts the overflowing single line** with a "..." tail (new `CSS_F_ELLIP` ≡ `DOC_INF_ELLIP` ABI bit, non-inherited, scoped to the declaring element's subtree by the element-open carry + exit-restore; the breaker appends after popping overflow words). ③**`-webkit-line-clamp: N` caps a text block at N wrapped lines** with the same "..." tail (the count rides the established hint transport: `DocBoxSpacing.clamp` filled by walk_box_spacing's winning_value query, assigned at p/div/h1-h6/blockquote/li opens, consumed as `BreakOpts.maxLines`; the cut lands after the budget-filling push with the tail on the LAST kept line — a first attempt landed the tail on a phantom +1 line and was fixed to spec). Probe v5 `PASS (0 fails)`: 6 new canaries — SROFF-HIT (clip recipe drops), SRTINY-HIT (1px recipe drops), SRKEEP (boundary: no clip/tiny → visible), OVKEEP (containment no-op keeps words), ELLKEEP (ellipsis keeps the prefix; the CUT is asserted at the LAYOUT level: the doc objects keep full text by design, so `ell-clip lines=1 width=371` and `clamp-lines lines=3 want 3` run the block's own inlines through the REAL breaker via the new battery probe `layout_test_probe_maxlines` — TAILSUFF/CLAMPCUT doc-scan rows replaced by these structure proofs); tally exact `medF=1 var=3 calc=2 prop=4`. Gates: tap1 ×3 identical; Wikipedia pin-stable 689/672 with the census receipt `prop=41→40` (one overflow-family name left the gauge); HN blocks=10 (230 links = upstream drift confirmed again); Bryan 48/18 stable with byte-identical census (prop=0, same as the P4 receipt). Scorecard 84→86. Plain production restored (0 errors, strings 1/0, boot green, 39KB idle); no new warnings in touched files; device untouched; git user-locked. Residuals (documented): wrapper-clamp ambiguity (a clamp on a DIV wrapper whose inner P flattens to its own block honors the wrapper's clamp only when the wrapper IS the text block) treated by assigning at the division-open sites; `text-overflow: clip` stays census (a second ABI bit for silent-cut buys little); size-only sr variants without clip/tiny keep flowing.
- [x] P6 Flexbox & Grid core *(L, both P6a+P6b)* — **CLOSED 2026-10-19.** Policy paragraph written into `css.h`: the flat 1-bit page IS the column axis — a flex/grid container is a BLOCK box whose vertical rhythm the block flow already is — so the honest lane consumes what the stack can honor and leaves the rest as census vocabulary. **Consumed (real behavior):** `display: flex|<anything containing flex>/grid` → container lane bit `CSS_F_FLEXCT` (walker-side flag, not a doc-space inherit — the container box stays a block); `align-items: center` → `CSS_F_AIC`; `align-self: center` → `CSS_F_ASC` (both ride the element-open carry, AIC gated on the FLEXCT+AIC pair — align-items on a block box is inert, CSS-correct). **Gap transport (the phase's engineering centerpiece):** the gap AFTER each child block mounts into the NEW DocBlock field `gapApplied` (memset-0; no first-item-skip flag, no accumulator — both were designed and then deleted: the final derivation needs neither, a cleanly dropped own-block dies with its mount); at the container's CSS exit a tail rule closes the LAST child's slot to the OUTER scope's gap (LIFO verified — the container's own WX_FLUSH precedes its WX_CSS_END, so its block is in the doc at swap time; an empty container's dropped block correctly leaves the outside block alone). A flex/grid container ALWAYS takes over the gap scope — even `gap: 0` closes the inherited outer gap between its items. `flex_gap_px`: winning `row-gap` first, then `gap` shorthand (longhand wins per cascade practice); declared-but-unresolvable → 0. `gap_len_px`: px/bare through the established `walk_box_px`, then fresh-parser rem/em (×16 root) with FRACTION precision kept on an r/p fixed point (`.75rem` = 12px exactly — the BW sheet's own vocabulary, `gap: 1rem/.75rem` × 17 decls), vw (×400/100), pt (×4/3); % → refused (a % gap needs the resolved container height — not decidable at walk time), negatives refused, px cap 480→240 post-halving. **Alignment transport:** `walk_flex_block` mounts gap + AIC/ASC centering at the ten block-open sites (heading, p/div, blockquote, center, marquee, li, dt/dd, dsp-mode2, both image sites incl. the figure-deferred one) only when the element's own `align` attr is NULL (HTML owns the win); ExitCtx grew `gapSaved`/`gapSet`, restored at the WX_CSS_END swap. **Layout consumption:** `+ block->gapApplied` on all six branch advances (heading, paragraph/blockquote, list item, code block, image, table) — gap mounts visualize as real vertical rhythm. **Census receipts (BW's real sheet, curl):** display ×26 (flex-heavy), flex-direction column ×2, gap ×17, align-items ×7 (center ×4), justify-content ×5 (space-between), flex-wrap ×2, grid-template-columns ×3 — the whole family + the gap trio joined `known_prop` (lengths python-verified) and left the unknown gauge. **Probe v6 `PASS (0 fails)`:** 13 new canaries assert the mounts DIRECTLY off the DocBlock objects (walk-side data a doc scan CAN read — unlike P5's paint cuts): FIXGAPA `gap=8` (16px→halved 8) with FIXGAPB `gap=0` (the tail slot closed at container exit — the LAST-item rule exact), FGAPC `gap=4` (`.5rem`→4, fraction precision), FXROWA `gap=6` (`row-gap: 12px` beats the shorthand), FAICC/FAICD `align=center` (AIC carry across children), FXSB `align=center` (element-scoped align-self), FXNA align=unset (gapless flex carries nothing), AINC align=unset + FGNPX `gap=0` (align-items and gap INERT on a plain block box); fixture tally exact `sib=0 attrOp=0 pcls=0 medF=1 var=3 calc=2 prop=2` (prop 4→2: gap/grid-template-columns + the flex/grid family left the gauge, `backdrop-filt x1 border-radius x1` remains) and all P1–P5 canaries byte-stable. **Gates:** tap1 RESULT `inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` (the P5 ×3 baseline); HN blocks=10 links=229 stable fresh+rewalk (230→229 = the documented upstream drift, pre-existing inline `byClass` TypeError only); Bryan 48/18 stable incl. rewalks with byte-identical census `prop=0` — the sheet's real `gap: 1rem/.75rem` decls mount INSIDE existing spacing, zero render perturbation; **Wikipedia gate caught an UPSTREAM page change (curl-proven, not a regression):** `wiki/Playdate` now serves a 67KB play-date disambiguation-stub page (132/85 blocks/links both renders; the console article moved to `/wiki/Playdate_(console)`, 296KB — future gates use it), and the collapse prop 40→3 is the EXPECTED gauge receipt: the vector-2022 sheet's flex/grid vocabulary is now consumed, `[css-propnames] backdrop-filt x1 border-radius x1` exactly. (The `[nr-autotest] revisit` tail row FAILs on URL-mode runs by its own nav-start cache-off purge contract — fired identically on the harness's own Bryan page with byte-stable renders; navigation-level behavior untouched by this phase.) Scorecard 86→88. Plain production restored (0 errors; warnings = the documented 3× `baseUrl` + third-party XS/QuickJS set; boot green, 40KB idle = 39KB + 1KB of grown fixture text; sims killed after every gate, count verified 0); device untouched; git user-locked. Residuals (documented policy + discovered): real 2-D distribution (direction/justify/wrap/place/order/grid templates/areas) = consumed vocabulary without behavior (policy non-feature); gap on inline-only children (nav anchors) = no horizontal gap (the inline items ride the container's own block); stray direct text after the last child = un-mounted; the prev-side slot BETWEEN a nested flex child and its anonymous inner box = unmountable (documented miss); gap declared via inline style = not transported (sheet-only lane this phase); rem margins remain the P9 residual (gap rem rides the fresh parser path only).
- [x] P7 Pseudo-elements & focus states *(M)* — **CLOSED 2026-10-19.** The generated-content cascade now LANDS in rendered text. **Engine:** `strip_pseudo`'s table grew from 4 to 15 entries — `::marker` becomes pseudo-lane 4 (content-carried), `::first-line`/`::first-letter`/::selection/`::placeholder`/`::backdrop`/`::view-transition` (+ legacy single-colon spellings) = lane 3 **consumed-inert** (rules parse+keep with spec "never matches elements" semantics — the compound-drop class from real sheets gone; an unknown `::`-vendor tail now even counts in the census instead of vanishing silently); `css_pseudo_content` passes the lane through to the rule-pseudo equality (1/2/4), and the `:target` matcher input rides a walk-filled engine slot (`css_set_target` from the `#` tail of `doc->baseUrl` at every walk incl. re-walks; case-sensitive like HTML ids; no fragment = nothing matches). **The old silent-lane cause, found and fixed:** `walk_pseudo_emit`'s punctuation-only guard was written for stray star-only SIBLING BLOCKS but killed BOTH lanes — the P0 `>>` canary class died by it; refined to drop ONLY the block-lane sibling (stray-line guard kept, verified: `.arw::before {content:">> "}` renders nothing while the div stays) and allow the inline lane (span `::before`+`::after` = `PSBEF-SPMID-PSAFT` in one block, quotes + CSS escapes + U+00A0/U+00B7 transliteration ride the existing resolver, `attr(data-url)` prints bracketed via a probe-local beacon canary — the acidtest `[href]` canary stays on its own fixture, cross-page markers never asserted). AND a latent ABI bug caught by the lane work: `css.h` declared 6 params while css.c defined 8 (C tolerated it at link) — header corrected in lockstep; the ::after call site still passed legacy `0` (now meaning "element rules") — caught by the first probe run, fixed to `2`. **::marker:** `li` opens query lane 4 next to `list-style-type`; the resolved text rides the new `DocBlock.markerText` (arena, NULL = default glyph, `""` = author removed the bullet, `none`/`normal` keep the default); layout draws it as the bold bullet item. **Truth pseudo-classes (honest matchers, NOT always-match):** `:disabled` (attr), `:enabled` (disableable control tag without `disabled`), `:checked` (checkbox/radio `checked` or `option selected`), `:placeholder-shown` (input/textarea + non-empty `placeholder`), `:root` (html), `:target` (id == walk fragment), `:empty` (spec-exact childless — comment-only counts empty via the new `CssDom.childless` accessor, DOM-backed); `:focus-visible`/`:focus-within` join the Phase-1 always-match interactivity family (policy (a) — same reader stance). Probe v7 **`PASS (0 fails)`, 95 rows**: `:root` vars hide via var() (ROOTVARHIT), `:checked`/`:disabled`/`:enabled`/`:placeholder-shown`/`:empty` hide their sibling texts through REAL matcher hits with the un-disabled/un-checked boundaries staying visible, `:focus-within` hides (always-match), `:target` keeps (fragment-less probe URL — the honest negative), `::marker` asserted DIRECTLY off the doc fields (`marker 'MK7'` exact + `marker ''` = bullet removed) — button SELF-state is unassertable by text scan (form labels never render as block inlines; sibling-text canaries carry the proof) and the two button rows were removed on that disclosed ground; tally exact `sib=0 attrOp=0 pcls=0 medF=1 var=4 calc=2 prop=2` (pcls stays 0 — the truth family + consumed `::`-family never reach it; var 3→4 counts the `:root` var; the `sib`/`attrOp` columns are the phase-retired always-zero refusals-only counters — the fixture header now says so) and all P1–P5 canaries byte-stable. **Real-site receipts (curl, pre-implementation):** vector-2022 sheet `::after ×54 ::before ×10 :root ×2 :empty ×4 :checked ×2 :focus-visible ×2` — every one of them a compound-drop or silent drop before this phase, all consumed/matched now (Wikipedia census: `pcls=0` on the real sheet); Bryan's Next chunk carries `::marker ×1 :before ×1 :disabled ×2 :focus ×2 :root ×1`; HN's news.css carries neither family (7KB, all-zero census stands). **Gates:** tap1 RESULT `inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` byte-identical; HN blocks=11 links=226 fresh==rewalk stable (229→226 = upstream front-page drift, pre-existing `byClass` TypeError only, 0 ERROR rows); Bryan 48/18 stable incl. rewalks with byte-identical all-zero census; **Wikipedia re-pinned to the NEW gate URL `/wiki/Playdate_(console)`** (the per-P6-closure upstream article move): blocks=324 links=310 fresh==rewalk stable, first-walk census `pcls=0 medF=0 var=5 calc=0 prop=17`, 0 ERROR rows. Scorecard 88→90. Plain production restored (0 errors; warnings = the documented 3× `baseUrl` + bundler/xs third-party set; boot green, 41KB idle = 40KB + fixture/field growth, stable heartbeats); sims killed after every gate (count verified 0); device untouched; git user-locked. Residuals (documented): `::first-line`/`::first-letter`/`::selection`/`::placeholder`/`::backdrop` = consumed-inert policy names (per-line/per-char typography is not a 1-bit-flow behavior — drop-caps/selection styling stay out by the trajectory line); `content: counters()/url()/quote keywords` still drop the whole value (P0 policy); block-lane pure punctuation stays guarded (stray sibling lines); `:target` updates ride full navigations (no scroll-spy / JS-driven fragment state); focus/hover/active/visited remain always-match (device truth, reversible by one parse line).
- [x] P8 HTML interactive set: details/dialog/template/srcset/select depth *(M)* — **CLOSED 2026-10-19.** The recon found four of the five named features already deep (R56g-era + Phase-1 machinery: details toggle + open-attr + summary extraction with closed children skipped, dialog's open-attr gate (closed = not rendered at all), template inert, select with option/optgroup collection + the model fields + receipt-tested round-robin tap cycling) — the phase built the genuine gaps. **1. srcset/sizes (the named centerpiece):** the old "first URL wins verbatim, only as a src-fallback" shortcut is gone — `img_pick_srcset` parses the spec candidate grammar (comma-separated `url [descriptor]`; width `480w` / density `1.5x`): width descriptors pick the SMALLEST that still covers the panel (CSS_VIEWPORT_W = 400 — the leafy asset for a 1-bit screen), all-narrower lists take the LARGEST; density prefers exactly 1x, else the smallest ≥1, else the closest-to-1 sub-1; descriptor-less lists take the first (author order); mixed families layer w→x→bare; unusable parse → the plain src stands (the spec fallback). A non-empty srcset REPLACES src in selection even when both exist (HTML: src is the fallback only when srcset is unusable). Bolted into BOTH image sites — and `<picture>` dropped its "last <source> wins" inversion: the FIRST source whose `media=` holds now supplies the asset, evaluated through the newly exported `css_media_holds` (the @media evaluator stood up as a public API; `type=` needs no gate — the decoder dispatches by magic bytes). `sizes`/`loading`/`decoding` = consumed-inert vocabulary, documented in the helper comment (the flat page computes its own layout size and fetches eagerly — no deferred pipeline exists to schedule, and srcset width hints do not change the drawn box). **2. Reset buttons (real interaction):** `<button type="reset">` produced NO block at all (neither arm matched) — arm fixed with the default label "Reset"; the new `DocBlock.buttonType` carries the element type to the tap dispatch, whose flow now follows the browser activation order: click dispatch first (a JS handler may preventDefault; SUPPRESSED → the handler re-walks), then the HOST reset and never a submit; no-JS pages run the host reset directly; formless resets do nothing (spec). The reset basis = four captured defaults: `DocBlock.defaultValue` (input/textarea/hidden/submit arms), `defaultChecked` (checkbox/radio), `defaultSelIndex` (select), `DocOption.defaultSelected`; `host_form_reset(item)` walks the same-form items (the submit serializer's own formAction test), restores all four (disabled controls reset too — spec: reset is not a submit filter), re-walks only when something changed. **3. Multiple-select depth:** the tap cycle now ACCUMULATES on `multiple` selects (each arrival marks the option selected — the user's taps join the author's `selected=` truth), and submit serialization sends EVERY selected option as its own name=value pair, bypassing the first-seen name dedupe (the same name is SUPPOSED to repeat — the real urlencoded shape); single selects stay byte-identical on their selectedIndex path. **4. Summary-less details:** a details with no `<summary>` now prints the user-agent default "> Details" toggle line (was an empty invisible fold the tap could not even show); asserted via label (box_open_block stores it as `label`, not inline — off the k[] text battery by design). **Probe v8 `PASS (0 fails)`:** 6 text rows (TMPLJUNK absent/TMPLKEEP present, DLGCLOSED absent/DLGOPEN present, DETBODY stays hidden behind the closed fold) + 4 srcset field asserts off the DocBlock objects (PLAINIMG baseline keeps plain.png; SRCSETW pick = three.png — 480w = the smallest of {200,900,480} covering 400; SRCSETD pick = van@1x.png — the exact-1x preference over 1.5x/3x; SRCSETF unusable `srcset=""` keeps src) + the msel model (3 opts, multiple=1, selected [1,0,1]) + the button battery (RNRESET type=reset; FMBTN = the bare-`<button>` default-type receipt: submit) + the details-default label row; tally EXACT and byte-stable `sib=0 attrOp=0 pcls=0 medF=1 var=4 calc=2 prop=2` (the canaries are deliberately HTML-only — zero CSS declarations added, so the CSS counters stay untouched); and all P1–P5 canaries byte-stable. **Real-content receipt:** the Wikipedia gate log shows live `[img] srcset pick:` rows — the article's infobox thumbs carry 250px srcs with 500px srcset candidates, and the panel policy upgrades them (smallest ≥ 400) — the lane's first real-page activations. **Gates (all five):** probe v8 PASS; tap1 `inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` byte-identical; HN blocks=11 links=225 fresh==rewalk stable (the documented upstream front-page drift — 30 titlelines curl-counted on the live page; pre-existing inline `byClass` TypeError only; census all-zero; 0 ERROR rows); Bryan 48/18 stable incl. rewalks with byte-identical all-zero census, 0 ERRORs; Wikipedia 324/310 EXACT baseline with census EXACT `pcls=0 medF=0 var=5 calc=0 prop=17`, fresh==rewalk, 0 ERROR rows. Scorecard 90→92. Plain production restored (0 errors; warnings = the documented 3× `baseUrl` + bundler/xs third-party set; boot green, 41KB idle stable across heartbeats, refusals=0); sims killed after every gate (count verified 0); device untouched; git user-locked. Residuals (documented): `template.content` stays out of the JS bridge (render-side inertness is the spec-render-correct surface; SPA template-cloning is a bridge-API item for a JS phase); `select size>1` renders a one-line field (a flat 1-bit panel has no multi-row widget to offer — size consumed as vocabulary); multiple-select pickup has no deselection UI (the reset button or a JS `option.selected` rewrite is the way back, and a full re-walk rebuilds options from the DOM, so accumulated picks ride the same option-object lifecycle as every tap-mutated state); dialog remains an open/no-open render gate (no showModal/`::backdrop` layer — backdrop rides the P7 consumed-inert lane).
- [x] P9 Fidelity polish: transforms/gradients/units *(M)* — **CLOSED 2026-10-19.** Three lanes, one shared engineering piece. **The shared unit reader (`fresh_len_px`, document.c):** every walk-side length consumer now answers lengths from one grammar — px/bare ×1, rem/em ×16 (browser root), vw ×400/100, vh ×240/100, **vmin (`CSS_VMINDIM` 240)/vmax (`CSS_VMAXDIM` 400)**, ch/ex ×8 (documented half-advance/x-height approximations — the engine ships no font metrics), pt ×4/3; % answers only in per-mille and only when the caller opts in; sign semantics per lane (negative gaps still refused — invalid CSS); fractions keep the P6 r/p fixed point (p ≤ 1000); **INT_MIN = not-a-length** (the -1 sentinel collided with a legitimately negative `-1px`; all consumers test < 0, so lanes are unchanged, and `parse_translate` now routes `pxv != INT_MIN` into the length lane). **Consumers rewired:** `walk_box_px` gains the fresh-reader fallback — **the P6 "rem margins" residual lands** (`margin-left:-.5rem`, `text-indent:1.25rem` and every other suffixed box length mounts); `gap_len_px` re-bodies through the reader keeping its 480→240 cap + no-neg + first-token-only; `box_num` rewired — **the legacy `"50%"`→25px wrong turn is repaired** (% refuses for margins, honest indent 0); inline margin longhands ride `box_num`; the @media Milli parser consumes vmin/vmax (120vmin = 288px holds at the 400×240 panel — fixture-proved). **Transform lane = real paint offsets:** `parse_translate` composes translate/translateX/translateY (px AND % on both axes; other fns skipped; unbalanced fns stop the read), `walk_tf_fill` mounts halved px + percent-of-own-box on the new `DocBlock.tfXpx/tfYpx/tfXpct/tfYpct` fields (memset-0 = absent) with the CSS cascade inline-beats-sheet (the INLINE value replaces the WHOLE transform property — a sheet translateY contributes nothing under an inline translateX); layout consumes through `tf_apply_items` across **ten branch lanes** (badge, heading (the h2 underline is created after the apply — separators are not transform targets), math, paragraph/quote with the rail riding via its line fields, list item, code box, image, table, meter) shifting the freshly emitted items only — completed branch-locally so later blocks never inherit a fake offset; **translate mounts as a real paint offset; rotate/scale/skew/matrix + transform-origin stay consumed census vocabulary** (a 2-D compositor is the only honest home — policy non-feature per the trajectory line); form widgets (input/select/textarea/hidden) and hr/li-rule separators skip by policy (tap targets, not layout boxes). Probe-found bug receipts (each fixed by a probe run): ① the fresh parser never advanced past unit letters (trailing-junk check voided EVERY token — rem gaps/margins were zeros), ② per-mille scale was ×1000 (100× over: `-50%` read as -50000‰; correct = ×10, `-50%` → -500‰), ③ translateY (axis 2) had NO write path (the sole argument never landed), ④ translate()'s first argument also wrote y (the second arg + a double-add read `translate(10%, -50%)` as y = +10% -50% = -40%). **Gradient polarity (the fix):** `css_gradient_luma` averages the resolved stop colors on the established 0..255 luminance scale (--130 of 255 = dark); `doc_is_inverted_style`, `walk_inverted`, AND the sheet route through apply_prop's `background-color` arm (the `background:` shorthand's first-stop color token fans out — `linear-gradient(#050505,…)` inverts the text like any dark background) — **dark gradient heroes no longer escape inverted detection by riding `background` instead of an attribute color**. **Probe v9 `PASS (0 fails)`, 11 new rows:** tf battery ×5 doc-field exact asserts (`xpx=6` 12px → half-scale; `ypx=-6` -0.75rem → -12 → -6; `xpct=5/ypct=-25` the % pair; `TFSHIFTR xpx=2` rotate skipped + 4px composed; `TFSHIFTWIN xpx=3, ypct=0` the whole-value inline win) + VMINHIT absent (the 120vmin MQ consumed) + GRADHERO present with `invert=1` + MRG2REM `top=16` (2rem) / MRG50PCT `indent=0` (the % refusal); tally byte-stable `sib=0 attrOp=0 pcls=0 medF=1 var=4 calc=2 prop=2` (every new declaration rides KNOWN vocabulary on purpose — transform/transform-origin joined `known_prop`); all P1–P8 canaries byte-stable (FGAPC rem-gap `gap=4` EXACT after the reader fix). **Gates (all four):** tap1 `RESULT inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` byte-identical; HN blocks=11 links=225 fresh==rewalk stable, census all-zero, 0 ERROR rows; Bryan blocks=48 links=18 stable incl. rewalks with byte-identical all-zero census; **Wikipedia blocks=324 links=310 EXACT baseline, census EXACT `pcls=0 medF=0 var=5 calc=0 prop=17`** (the vector sheet's counted vocabulary untouched by the new known rows — transform/origin do not appear in the surviving census; live srcset 250→500px picks standing), fresh==rewalk, 0 ERROR rows. Scorecard 92→94. Plain production restored (0 errors; warnings = the documented 3× `baseUrl` + bundler/xs third-party set; boot green, **41KB idle stable across heartbeats**, refusals=0; sims killed after every run, count verified 0); device untouched; git user-locked. Residuals (documented): translate skips form widgets + separators by policy; `transform` on INLINE-only children (spans in flow) mounts only where a block opens; gradient polarity rides the registered color token (a gradient whose FIRST stop misrepresents the average — e.g. light-to-dark keyed white-first — leans on the inline route's full-value scan; the sheet shorthand route keys the first stop); vh/`handheld`-style exotic units (Q, dvw-family) refuse as unknown suffixes by design.
- [x] P10 transitionend/animationend immediate-fire *(S)* — **CLOSED 2026-10-19.** The declarative animation/transition vocabulary now DELIVERS its end events, honoring Mdn.js-safe engine boundaries: **`@keyframes` registry** — engine-side `keyframes[24][25]` + count, collected at `css_parse_sheets`' `@`-skip branch via `css_keyframes_collect` (ident-only names, both `@keyframes` and `@-webkit-keyframes`; >24-char names refuse = the animation stays inert, never mis-fires); **value resolver** — `css_anim_registered_name` picks the first registry-known token, skipping numerics, `(`-functional notation, and the animation keyword set (`none/infinite/linear/ease…/forwards/backwards/both/running/paused`); **walker engage** — `walk_anim_engage` rides every block-open call site (companion after each of the ten `walk_tf_fill` sites): full cascade (inline style attr beats the engine winner) on `animation-name`/`animation`, guards = infinite/zero-iteration (`animation-iteration-count` first numeric token) and `paused` (`animation-play-state`), exactly-once dedupe via the DomResult `animFired[24]` (node,name) map, enqueue into `pendEnd[16]`, `DocBlock.animName` receipts; **the walker-instance bug the fixture caught:** fresh-parse walks wrote the dedupe map into the STACK DomResult while later renders map against the HEAP twin (double `animationend` on jsext) — Walker now carries `walkDom` (fresh = the promoted heap pointer, rewalk = its own) — fixed + re-proven; **transition lane** — the JsBridge mutation-notes ring (`mutNotes[24]`, noted at nine tree-mutation sites incl. the classList helpers which now escalate `domDirty` R58-parity) arms `transitionend` only when `document_node_transition_hot` reads a non-`none` `transition`(+`-property`) vehicle against the class/id attrs of the noted node; **deliver channel** — `p10_drain_end_events` runs in `render_done` after `log_render_result`: the animation lane dispatches per pendEnd entry then zeroes, the transition lane takes the mut notes, checks hot nodes, dispatches, and logs `[p10] animationend lane` / `[p10] transitionend lane: %d mutation(s), %d armed & dispatched` (both rows GATED — silent when nothing to drain, verified by Wikipedia's zero rows); **listener kind 4** — `animationend`/`transitionend` map to a stored listener class in BOTH addEventListener paths (element + document) with the matcher filtering by kind (NEVER kind 0 — `xs_dispatch_click` matches kind 0 without a type filter and would mis-deliver on every tap) — `PL10ANIMEND/PL10TRANS` fixtures render through stored listeners. **Receipts:** probe v10 `PASS (0 fails)` — ANIMLIFT engages (`anim pf`), ANFALLDOWN/ANIMPAUSE/ANIMINF stay field-absent (the guard battery), `keyframes pf known + nokey absent`, direct `transition=color .3s` resolution, `animationend lane: 1 run(s) dispatched`; about:jsext suite 8/0 with the mounted-animation receipt: `PL10ANIMMOUNT1` renders exactly-once (ext-script-created element + pre-append addEventListener + inline-victim static completion lost pre-listener = the documented browser-faithful race; the dedupe map blocks any re-dispatch), `PL10TRANS1` renders exactly-once via the classList-toggle timer; census/blocks/links byte-stable on every real page: tap1 `RESULT inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` byte-identical (6 quiet `0 armed` lane rows), HN 11/225 fresh==rewalk + census all-zero + 0 ERROR rows, Bryan 48/18 byte-stable census + `[p10] transitionend lane: N mutation(s), 0 armed & dispatched` rows on the React-MDN surface, **Wikipedia 324/310 EXACT fresh==rewalk==snap + census byte-identical to P7/P9 (`pcls=0 medF=0 var=5 calc=0 prop=17`, propnames row identical)** — the earlier "prop 17→13" prediction was WRONG: the trimmed sheet's animation/transition decls sit inside streams that never count (dropped reduce media / past the engine cap), so the receipt = ZERO census perturbation, the strongest form; the one live-run links wiggle (309 vs 310, same binary) was closed by experiment: article unedited since 2026-10-13, back-to-back curls byte-identical, and `PLUTO_NR_AUTOTEST_FILE_URL=file://wikia.html` TWIN runs on the byte-fixed upstream snapshot render **324/310 byte-identical twice** (engine deterministic on fixed input; the wiggle was served-side chrome — `render-error(discarded)` = the pre-existing jsext-prefetch abort path inside the documented revisit-noise contract). Scorecard 94→95. Sims killed after every run (count verified 0); device untouched; git user-locked. Residuals (documented): static inline victims complete on the pre-script fresh walk (R54 pump = browser-faithful race; mounted scripted animations prove delivery), keyframes bodies stay skipped (registry = names only — no per-frame interpolation on a 1-bit flat panel), transitions need a walk-VISIBLE attr gate (JS-set styles house-registered separately), partially-filled classes score as full-name matches by the resolver's registry-first policy.
- Target trajectory: 75 → ~80 → ~88 → ~94 → ~97 (last 3 = documented policy non-features)

## Active roadmap — Engine's browser API surface → close (the API program, SIM-ONLY)

**CLOSED 2026-10-20 with API-P10 — the program finished end-to-end: the Missing list is empty,
the Status column is COMPLETE, and every gate re-verifies standing.** Approved 2026-10-19. Order
of operations (usage-weighted), sizes, and the bar each phase must clear before its row goes
`[x]`. The Status column opened at **IN PROGRESS**; flipped to COMPLETE only when the API
**Missing list emptied** (the last item — async http(s) module graphs — landed in API-P10).
**Simulator only for every phase** (device locked). Every change is written generally —
no site-specific behavior anywhere. Standing receipt set each phase: `about:apiprobe` probe rows
(as each phase lands, its row becomes an assertion) + jsext suite 8/0 + the four real-page gates
byte-identical (tap1 `RESULT inPlace=yes jsErrors=0 blocks=23 errCard=1`; HN 11/225 all-zero census;
Bryan 48/18 all-zero census; Wikipedia 324/310 with the `prop=17` census identical). The score moves
only with receipts, at phase close.

- [x] **API-P0 Baseline + `about:apiprobe` probe farm** *(S)* — **CLOSED 2026-10-19.** The capability battery page registered on Home Test Cases (appended last so the existing home-test harness assertions stay true) + in the README at creation. Baseline run on engine 4: **`apisum: pass=11 miss=3`** byte-stable across the rewalk, `ran=1 errs=0`. PASS rows: `domwrite` (functional creation check — NEW FACT: the bridge's `appendChild` returns a falsy value instead of the appended node per MDN shape; recorded for API-P6), `title`, `timers`, `xhr`, `listeners`, `promise`, **`fetch` (presence — and the log answers why: `[js] fetch shim: injecting (before script 1)` + `fetch shim: defined` — a shim ALREADY injects into page scripts on this engine; API-P2's job is vetting it into a real receipt, rehabilitating the session-plan's stale "no fetch" fact)**, `console` (presence — console.log exists on NR today, past-profile wrong), `location`, `history`, `currentscript`. MISS rows (planned/recorded): `storage` (setItem/getItem roundtrip fails on NR silently — String() tolerance tried, zero error rows — recorded as the new **API-P7** below; the old "storage live" fact does not survive today's measurement), `worker` (the planned contained-stub policy lane), `template` (API-P6 lane). Probe-bug receipts from v1 (each caught by its own run, fixed, re-run): domwrite v1 trusted appendChild's return value (labeled PASS-as-MISS while the page itself rendered) and storage v1 used strict `'1'` equality — both replaced with functional checks in v2. Real-page deltas: none (fixture + registration-only diff); the jsext standing gate re-verified 8/0 on the P0 build.
- [x] **API-P1 Events that behave** *(M)* — `submit`/`input`/`change` fire through real handlers,
  events bubble up the tree with correct `target`/`currentTarget`, `preventDefault` honored by the
  pipeline. The non-JS fallback (tap → submit_form, zero dispatch) stays byte-identical — pages with
  no listeners keep working exactly as today. **CLOSED 2026-10-19.** The phase's measurement finding:
  the event machinery was ALREADY built (the TAP2/R arcs): `form_key_write` sets the value BEFORE it
  dispatches (input, then change on commit — react's onChange reads target.value synchronously),
  and `activate_form_block` (LRI_INPUT_SUBMIT) runs the browser activation order — a dispatched
  `click` on the button first, then `submit` on the nearest `<form>`; `JSB_EVENT_SUPPRESSED`
  (= a handler called `preventDefault()`) branches to the live re-walk, `rc == NONE` short-circuits
  to the byte-identical native fallback, document-level listeners receive what bubbles. This phase
  made it RECEIPTED: **(a) the Events battery** in `about:apiprobe` — a live form fixture
  (`#evf`/`#evi`/`#evb`) with real listeners (field input/change counters, button click, a
  `preventDefault()`-calling form submit handler, and document-level submit+click rows that only
  count when the event BUBBLES past the target), painting `apisev: input=N change=N submit=N
  bub=N btn=N doc=N`; **(b) the `[ev-test]` sim driver** (`PLUTO_EV_AUTOTEST`, main.c) driving the
  REAL pipeline on the probe — one `form_key_snapshot` + `form_key_write` commit (the tap
  autotest's own typed-field recipe), then `activate_form_block` on the stamped submit button;
  verdict = counters parsed from the re-walked blocks PLUS doc-pointer identity (sameDoc = the
  re-walk; newDoc would mean a reload had rebuilt the doc and reverted the field to markup).
  **Receipts:** typing → `apisev: input=1 change=1 submit=0 bub=0 btn=0 doc=0`; press →
  `[form] submit tap: click=1 … tag=button` → `[form] submit dispatch: rc=2` (SUPPRESSED =
  preventDefault owned it) → `[ev-test] RESULT inPlace=sameDoc jsErrors=0 (before 0) blocks=27
  (was 23) found=1 input=1 change=1 submit=1 bub=1 btn=1 doc=1` + `[ev-test] PASS: input+change+
  submit all delivered, bubbling rows counted at document level, pd honored (suppress re-walk, no
  reload)`; block arithmetic exact (21→23 on typing, →27 on press); the P0 battery byte-stable in
  the same run (`apisum: pass=11 miss=3`). Gates on the P1 build: HN `blocks=11 links=225` ×2 and
  Bryan `blocks=48 links=18` ×5 census-identical; Wikipedia byte-exact via the `FILE_URL` twin
  (`served wikia.html (296562 bytes)`, census `var=5 calc=0 prop=17` + propnames row identical,
  `324/310` fresh==rewalk==snap at the SAME frame rows #46/#99/#125, snap `bytes=198815` exact);
  tap1 (TAP-only build + stack defines) `RESULT inPlace=yes jsErrors=0 (was 0) blocks=23
  errCard=1` with the identical errCard text (#818 vs the P10 baseline #820); jsext suite
  `8 passed, 0 failed`. The byte-identical pledge is STRUCTURAL here: zero engine code changed in
  P1 (driver only observes; the battery = page data), and the tap1 gate re-proved the no-listener
  fallback unchanged. Production build restored (0 errors; `ev-test`/`nr-autotest` strings back to
  0 in `pdex.dylib`), boot-verified, sims killed + counted 0 after every run. Score 50→62.
- [x] **API-P2 `fetch` for real** *(M)* — the existing HTTP client under a Promise-shaped `fetch`
  (same timeouts, refusals, spill-to-disk paths inherited); `currentScript` filled correctly.
  **CLOSED 2026-10-19.** The shim (an engine-owned script injecting before the page's first script,
  inherited from the C4 arc) now earns its "real" through a general-purpose header lane + receipts:
  **`setRequestHeader` went from missing to real** — added to the XMLHttpRequest prototype
  (`xs_xhr_set_request_header`, browser ordering: any time before send, throw after), driving a
  per-request header store (`reqCt/xhdrName/xhdrVal`: Content-Type override + a recorded-last
  receipt pair); **the wire knob** — POST bodies now ride `http_request_ct` with the site's OWN
  Content-Type (NULL keeps the historical `application/json`; every pre-existing path is
  byte-identical); **the shim applies `opts.headers`** by forwarding every pair through
  setRequestHeader (try/catch — a bad header can never kill the request). Non-content-type headers
  are RECORDed + logged (the HTTP client exposes no extra-header surface: documented boundary,
  never invented on the wire, never silently dropped). `currentScript` = A2's stamped
  per-run synthetic script element (src attr walk, NULL outside a run) — now ASSERTED rather than
  trusted: `getAttribute('src') === null` on the running inline script, plus the browser-faithful
  NULL inside a settled promise callback. **Receipts — the probe:** `apisum: pass=11 miss=3`
  byte-identical (the strengthened `currentscript` predicate still PASSes) + the new Fetch
  receipts battery `apifetch: pass=4 miss=0` byte-stable across rewarks: `fetchbody: PASS`
  (fetch about:apiprobe → ok=status 200 → text contains the page marker; the local-answer xhr
  receipts `#1 open GET about:apiprobe` + `local ok (6225 bytes)` in the same run), `fetchjsonreject`
  (r.json() rejects on HTML = the promise-rejection lane), `postct` (POST + a
  `setRequestHeader('content-type', …)` override settles ok), `csasync` (`currentScript === null`
  in the callback). **The wire receipt (the phase's crown):** the real send POST —
  `[js] xhr #1 open POST https://server.bryanwandrych.com/send_email` →
  `header content-type: application/json` (the site's OWN value through the new lane) →
  `POST ct='application/json'` on the wire → `ok (17 bytes)` → sent card, TAP2 cycles closed.
  **Gates on the Phase 2 build:** tap1 byte-identical (`RESULT inPlace=yes jsErrors=0 (was 0)
  blocks=23 errCard=1` at the SAME frame #820 as the baseline), HN `11/225` ×2, Bryan `48/18` ×5,
  Wikipedia FILE_URL twin byte-exact (324/310 fresh==rewalk==snap at identical frames #46/#99/#125,
  snap `bytes=198815`, census + propnames identical), jsext suite `8 passed, 0 failed`. Real-page
  parity intact everywhere because the site's handler already sent exactly the default content-type
  — now its own value governs instead of the default. Production build restored (0 errors, harness
  strings 0), boot-verified, sims killed + counted 0 after every run. Score 62→68.
- [x] **API-P3 The page's own address** *(M)* — `location` reads, `history.pushState/replaceState`
  recorded in an in-device journal with `popstate`/`hashchange` delivery, `document.title` set/get;
  pushState NEVER causes a network navigation. **Done when:** a scripted address change renders in
  the probe and `popstate` fires on recorded back-stacks.
  **CLOSED 2026-10-19.** The address became an ENGINE-OWNED BOOK (the bridge's `histUrl[8][260]`
  journal, entry 0 = the doc base seeded on first use) and the location surface went LIVE:
  **`location` = 9 live getters** (`href/origin/protocol/host/hostname/port/pathname/search/hash`)
  re-parsing the journal's current entry per read — the bridge-time snapshot builder is gone, so a
  scripted push moves the whole surface on the next read; **`pushState/replaceState` = real
  journal writes** (same-origin guard throws on a scheme+authority head mismatch like a browser's
  SecurityError; relative forms resolve browser-faithfully — fragment keeps path+query and replaces
  the fragment, query replaces the query and drops a fragment, paths root-resolve; `about:` bases
  get the opaque-token join since url_resolve stranded bare `#x`/`?x`; push grows (re-push
  truncates forward, capped ring), replace overwrites in place) — **and NEVER touch the wire**:
  no http_* call, no pendingNav, no app address-bar change (structural: the router layer is
  uninvolved by construction); **`back/forward/go` walk the journal** (clamped, boundary moves
  no-op) and LATCH `popstate`/`hashchange` for the next engine-pump frame (metered, per-listener
  xsTry isolation, contained aborts — the xhr-pump discipline); `popstate` fires at the WINDOW
  with the new entry's state (element-registered ones never fire — browser-faithful); `hashchange`
  fires only when the FRAGMENT field itself moved, carrying oldURL/newURL; `history.state`/`length`
  = live getters over the journal (per-entry state pins, replace re-pins). `document.title` set/get
  = now a REAL pair: the setter previously APPENDED to the title node's text ('API Probe' + 'Pluto
  J3' = 'API ProbePluto J3') — fixed to `dom_set_text` (children replaced). **The shared parser
  learned the about: opaque tails** (url_parse's about: branch was host=all/path=all with no
  query/hash compartments: the probe's own page could never read its own search/hash) — field
  replacement per the URL standard, network pages uninvolved. **Receipts — the probe's
  Address journal battery, `apihist: pass=7 miss=0`** (byte-stable across rewarks):
  `histpush` (#h11 join, state p=11 pin, length 2), `histreplace` (?a=2 query swap, fragment
  dropped, length stays 2, state replaced), `histhashtop` (second push → length 3), `docsettitle`
  (document.title set → get roundtrip), `histback` (back → the replaced entry: search '?a=2',
  state p=12, **popstate fired once carrying p=12** (`lastPopState='12'`), **hashchange fired
  once** for the `#h9`→'' hop), `histbase` (back to the seeded base: search '', state null,
  pops≥2), `histforward` (forward re-enters the entry, pops≥3) — with `apisum: pass=11 miss=3`
  byte-identical and `apifetch: pass=4 miss=0` unchanged. **Engine receipts:** `[js] history
  pushState j=1 n=2 url='about:apiprobe#h11' (no network navigation)` → replaceState `j=1 n=2
  url='about:apiprobe?a=2'` → pushState `j=2 n=3 url='about:apiprobe?a=2#h9'` → `back/forward →
  j=1 …(popstate pending)` → `delivery: popstate fires=1 j=1` + `delivery: hashchange fires=1
  'about:apiprobe?a=2#h9' → 'about:apiprobe?a=2'` → back to j=0 (popstate only — hashes equal) →
  forward to j=1. **Gates on the Phase 3 build:** jsext `8 passed, 0 failed` (0 unexpected
  errors), tap1 byte-identical (`RESULT inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` +
  errCard text), HN `11/225` ×2 at the same frames (#79/#166), Bryan `48/18` ×5, Wikipedia FILE_URL
  twin byte-exact (census `var=5 calc=0 prop=17`, propnames row, 324/310 fresh==rewalk, snap
  `bytes=198815`), ev-test PASS (`inPlace=sameDoc jsErrors=0 … found=1 input=1 change=1 submit=1
  bub=1 btn=1 doc=1` — block numbers shifted only by the new battery's rows; the verdict counts
  counters, not blocks). One sim-host observation, unrelated to the code (chronic across 2
  consecutive runs at the same frame, self-healing before any metric): a site's own timer
  aborts on the memory envelope AT the revisit reload boundary (`engine abort in timer callback`
  → engine reset → the revisit flow re-runs clean ×5) — Bryan's pending timers fire at the
  51MB-live host moment; no journal/delivery code is on that stack. Probe dump cap 24→40 in the
  sim-only seam so the deep address rows render in receipts. Production build restored
  (0 errors, harness strings 0), boot-verified (heartbeats live), sims killed + counted 0 after
  every run. Score 68→74.
- [x] **API-P4 console.log/warn/error** *(S)* — **CLOSED 2026-10-19.** The console surface became the
  real log lane on NR: every method routes into the file with its own level tag (`[console.log]` /
  `[console.info]` / `[console.debug]` / `[console.warn]` / `[console.error]` — the file is the
  receipt surface, device truth; the rendered panel NEVER changes from a console call), printf-style
  substitution rides the first string arg browser-faithfully (`%s/%d/%i/%f` to String, `%o/%O/%j`
  the console shape, `%c` consumes silently, `%%` literal, an out-of-args token prints literally
  like Chrome, extras append space-separated), objects/arrays render the console shape — top-level
  strings bare, nested strings quoted, arrays `[a, b, …n]` (4 shown), objects `{k: v, …}` (6
  own-enumerable keys via the engine's own iterator), functions `[Function]`, host objects honest
  `[Object]`, depth 2 with bounded bytes (cycle-safe by depth construction) — and `console.assert(v,
  …)` owns browser semantics: TRUTHY = silent (no row), FALSEY = an ERROR row
  ('Assertion failed: …' with the SAME substitution lane), never throws. `info/debug/dir` are real
  methods now; `count`, the group family, `time`, `trace` stay deliberately ABSENT (documented
  boundary: an unknown method stays unknown; no fake surface). Implementation notes that carry
  forward: array elements read via `xsGetIndex` and object values via `xsGetAt(keySlot)` —
  `fxNewNameC` interns plain names, so string-built "0" ids MISS the index flavor (the fixture
  caught it: `[7, 8, 9]` rendered as `[undefined, undefined, undefined]` in v1, exact after the
  fix); line budget 320 bytes with the logger's 512 row cap; `xs_console_format_into` is shared by
  the publish and assert lanes (its terminator: the helper owns the trailing NUL — an off-by-one
  memcpy'd prefix swallowed the first assert receipt, caught by the same probe run, fixed in-line).
  **Probe** (new "Console receipts" battery, `apicon`: pass=8 miss=0 **sent=17**): consolog
  (5 formatted sends: `%s`, `%d`, `%o`-with-nested-object `{a: 1, b: [7, 8, 9]}`, `%%`, bare
  5-array `[1, 2, 3, 4, …5]`), consowarn (2), consoerr (2), consoinfo (2), consodebug (1), consolong
  (268-char payload → the bounded row, no crash), consoassert (falsey fires `Assertion failed:
  P4ASSERT1 fired number=1` AT ERROR LEVEL with substitution; truthy stays SILENT — the battery
  also asserts the methods' presence: `assert` + `dir`), consoclean (the panel-clean receipt:
  `out.children.length` delta ZERO across the whole console storm). **The counts match in the log
  (the phase bar): the battery's own `P4COUNT 17` row = the JS-side sent counter; the log shows 16
  `[console.*]` rows + the documented silent truthy assert = 17 sends; `P4NOASSERT` = 0 rows in
  every run.** Receipts banked: probe ×3 runs (16 rows ×3, tags exactly log/warn/error/info/debug +
  assert-at-error), jsext 8/0 with its `[jsext-t]` markers now travelling the TAGGED console lane
  (byte-identical verdict), tap1 `RESULT inPlace=yes jsErrors=0 blocks=23 errCard=1` unchanged, HN
  `blocks=11 title='Hacker News'` ×2 (links 226 vs standing 225 = one extra link in TODAY's live
  front-page DOM — fresh and rewalk agree byte-consistently ×4, the engine shape is unchanged;
  site drift documented, the +1 is content not engine), Bryan `48/18` ×5 + the documented
  revisit-timer memory abort reproduced as chronic sim-host noise (self-heal at the next fresh
  render), wiki byte-exact (`census var=5 calc=0 prop=17` first pass, `blocks=324 links=310`,
  `snap bytes=198815`, the standing revisit-missed noise row present), ev-test
  `RESULT inPlace=sameDoc jsErrors=0 found=1 input=1 change=1 submit=1 bub=1 btn=1 doc=1` (counter
  verdict intact; block numbers grew to 46/48/52 because the probe's canvas grew — the tap fence
  counts counters, not blocks). Render dump cap 40→56 (the probe's whole canvas fits: 52 blocks).
  Sims killed + counted 0 after every one of the 11 runs. Score 74→76.
- [x] **API-P5 ES modules** *(M)* — `<script type="module">` through the loader with relative
  import resolution so `import './x.js'` works. **Done when:** the probe's module rows PASS and the
  jsext suite is unchanged. **CLOSED (receipted):** the loader-side module lane in jsbridge.c —
  both scanners flag the element via the shared type-is-module rule (jsext.c's three emission
  points + the legacy extract loops), and the router swaps the classic run for a C-side transform
  before any engine sees the source (general-purpose: same lane on every engine, vendored sources
  untouched). Probe battery: modin (inline module runs), modabs (named bindings from the
  `about:p5m0` store module), modstar (`* as ns` from `about:p5m1`), and modrefuse (the path-only
  import on the opaque about: base refuses with the browser's module-specifier semantics — the
  honest engine error row, `kids=3` proves no leak) → `apimod: kids=3 refuse=PASS` on every
  render walk. The `file://modpage.html` gate proves the relative bar: `import './p5rel.js'`
  resolves through the flat file-road join, `p5rel.js` itself imports `./p5rel-child.js`
  MODULE-relatively (base `file://p5rel.js`), children run before the importer (graph order),
  `RELA === 7 + RELC` → `modrel: PASS`, `modabout: PASS` (absolute about: import from a file
  base), `modpage: pass=2 miss=0 kids=2`. jsext **8/0 unchanged**; tap1 `inPlace=yes jsErrors=0
  blocks=23 errCard=1`, HN `blocks=11 links=226` ×2, Bryan `48/18` (+ the documented chronic
  abort, self-heals ×4), wiki census `var=5 calc=0 prop=17` / `324/310` / `bytes=198815`, evtest
  `sameDoc` counters all 1 (the +4 block delta = the module battery itself) — every standing
  receipt byte-identical. Receipts banked: /tmp/api_p5_{probe,modpage,jsext,tap1,hn,bryan,wiki,
  evtest,prodboot}.txt. Score 76→82.
- [x] **API-P6 DOM conveniences** *(M)* — `template.content`, `el.closest()`, `el.dataset`, neighbors.
  **Done when:** the probe's template/closest/dataset rows PASS and the gates are byte-identical.
  **CLOSED (receipted):** `template.content` lands with the browser's shape — the builder now CAPTURES
  `<template>` subtrees (dom.c: the element registers normally and stays reachable — id/querySelector —
  while its children parse into a DETACHED `#template-content` fragment, parent-NULL ⇒ inert to
  getElementById, render, census and every tree walk; nodeType 11 on all three engines; `content`
  materializes an EMPTY fragment on first access for DOM-created templates; nested templates each own
  a fragment; the free-pass visitor frees the owned detached roots, which also dies the pre-existing
  per-document shadow-root leak). `el.closest()` rides the same SW5 machinery as the querySelector
  walk (`css_parse_selector` + `qs_matches`), self-first through element ancestors, null for unusable
  selectors — wired through jsbridge + all three adapters (an `xs_el_closest` log-after-JFree bug
  caught in review before first run). `el.dataset` = a fresh camelCase snapshot per access (writes do
  not write back — `setAttribute` is the browser's road; documented boundary), added to duktape + mujs
  (XS NR already had it). Append family fixed to the MDN shape on all three engines: appendChild/
  insertBefore return the child, removeChild returns the removed node. Probe battery (six rows):
  `tplparsed` (fragment = nodeType 11, 2 children, inner ids intact), `tplinert` (inner ids invisible
  to getElementById, the template element itself reachable), `tplcreated` (created + empty-fragment
  mount), `closest` (self + ancestor + id climb + miss ⇒ null), `dataset` (camelCase + live
  read-back), `appendret` (all three returns identity-true) →
  `apidom: pass=6 miss=0`, and the P0 `template` MISS flips into `apisum: pass=12 miss=2` (standing
  `apifetch 4/0`, `apihist 7/0`, `apicon 8/0 sent=17`, `apimod kids=3 refuse=PASS`). THE PHASE'S REAL
  REPAIR (found while wiring the battery): the adapters' getElementById walks pushed every child at
  once (O(width)) and ABORTED the whole scan when `top + childCount > 64` — a body with 65+ element
  children silently hid every later id from getElementById (the probe page itself tipped into this:
  mid-page ids returned null and scripts 3/4 threw "cannot coerce null to object"). One shared
  O(depth) `dom_find_by_id` (parent+child-index frames in the free-pass/qs-walk shape, document order,
  width-proof, `_dN` fallback retained) now serves all three engines, and dom.c's paged-road helpers
  (`find_first_stub`/`find_id_ram_only`) got the same reframe. Cross-engine smoke on the seam
  (`PLUTO_NR_AUTOTEST_ENGINE`): mujs 4/6 and duktape 4/6 — every feature row PASSes on both; the two
  misses are strictly the `!==` wrapper-identity checks (those engines push fresh wrappers per access
  — the pre-existing shape, now documented; XS NR's per-node wrapper cache keeps identity; duktape's
  nodeType also gained the fragment rule during the smoke). Gates on the final source: tap1
  `inPlace=yes jsErrors=0 blocks=23 errCard=1`, jsext 8/0, HN `blocks=10 links=229` ×2 (verified
  live-content drift: today's HN HTML ships ONE `s.gif` spacer, not two — upstream change, rows
  identical), Bryan `48/18` (+ the documented chronic abort, self-heals), wiki census
  `var=5 calc=0 prop=17` / `324/310` / `bytes=198815` — byte-identical — evtest `sameDoc` counters
  all 1, `blocks=65 (was 61)` = the +4 battery blocks. Parked pre-existing gaps, documented not faked:
  `className` accessor (the battery uses `setAttribute('class', …)`), storage (Phase 7 lane), workers
  policy. Receipts banked: /tmp/api_p6_final_{probe,tap,jsext,hn,bryan,wiki,evtest,prodboot}.txt +
  engine smokes /tmp/api_p6_smoke_{mujs,duk}.txt. Score 82→88.
- [x] **API-P7 General-purpose localStorage/sessionStorage** *(M)* — **CLOSED 2026-10-20.** One shared
  road (`Source/html/webstorage.{h,c}`) serves all three engines: adapters bind `localStorage` +
  `sessionStorage` host objects (XS NR: one proto + two host instances; duktape: per-instance methods
  + hidden kind prop; mujs: userdata + registry proto) onto the same origin/claim/load/save machine.
  **Origin rule** (scheme parsed at the FIRST colon — `about:` has no `//`): http(s) → `scheme://authority`
  host-only (HN's `news.ycombinator.com` and `news.ycombinator.com/` bind the SAME origin — paths never
  split origins), `file:` → one shared `file://` bucket (WebKit/Chrome parity), `about:` → per-URL
  bucket (house pages isolated from each other; no foreign site can land there), data:/unknown/empty →
  the opaque `"null"` origin = fail-store (empty reads, quota-failed writes, never claims a slot),
  >8 origins in one run → overflow-fail store (each previously-saved section stays on disk and re-loads
  on a later run). **Caps (documented, browser-shaped):** key ≤56, value ≤192, 16 entries/origin, 8
  origins resident/run, 4KB advertised per-origin quota (browsers 5MB); both the value-length and the
  total-bytes refusals throw `QuotaExceededError: per-origin storage quota exceeded` and store nothing.
  **Persistence:** localStorage per origin in the game Data sandbox (`webstorage_js.txt`, `@origin`
  sections + TAB-separated escaped lines, one bounded ≤12KB image write per mutation, in-memory image
  re-adopted after every save so a cleared section can never resurrect from a stale parse); sessionStorage
  = in-memory per browser run (single-tab model). **Probe receipts (all engines):** `storage: PASS` —
  roundtrip + remove→null + sessionStorage roundtrip + the quota subtest live on XS, mujs AND duktape:
  `[webstorage] quota: origin=about:apiprobe want=5134 have=6 cap=4096 key='biggie'` + the `[p7q]
  QuotaExceededError` console catch row + `getItem('biggie') === null`. `apisum` P0 → **13/1** (only
  `worker` remains MISS; mujs/duk keep their `9/5` standing untouched). **Cross-boot gold receipt:**
  the battery's persistent counter — boot 1 `visits=1` writes `p7v 1`; boot 2 reads `bytes=22 entries=1`
  → `visits=2` → writes `p7v 2`: localStorage survives process death through the same Data road as
  storage.c. The ledger ran 1→11 across the phase's 11 probe-page boots with every increment accounted.
  **Battery edits stayed in-place** (no canvas shift) and carried TWO self-caught bug receipts: the
  origin parse originally required `://` so `about:apiprobe` fell into `"null"` (fixed: first-colon
  scheme), and the section-header match compared a `\n`-terminated header against newline-less lineLen
  so loads silently skipped (fixed: strict line equality, `@about:apiprobe2` can never match
  `@about:apiprobe`). A third engine catch: `new Array(5001).join('x')` aborted the mujs battery
  silently (engine-meter class) — replaced with 9 concat-doublings, quota receipt intact on all
  engines. Gates re-run on the final battery: tap1 `inPlace=yes jsErrors=0 blocks=23 errCard=1`
  byte-identical, jsext 8/0, HN `11/225` ×2 (live-content drift class — counts per era, verified by
  era), Bryan `48/18` (+ chronic abort self-heal), wiki `var=5 calc=0 prop=17` / `324/310` /
  `bytes=198815` byte-identical (`origin=file://` receipt), evtest `sameDoc jsErrors=1 (before 1)
  blocks=65 (was 61)` all counters 1. Production restored + boot-verified (6 heartbeats, no autotest
  defines, no webstorage rows — home loads no page). Score 88→92.
- [x] **API-P8 Worker contained-stub lane** *(M)* — **CLOSED 2026-10-20.** The P0-planned
  "contained-stub policy lane" ships as the honest browser-shaped failure story: a real background
  thread does not exist inside this browser's single cooperative pump, so the constructor SHIPS
  (a page doing `new Worker(url)` would otherwise die on a ReferenceError and stop) and every
  construction behaves exactly like a worker whose script always fails to start. One shared router
  lane (jsbridge.c `worker_open/get/terminate/post/pump` + `worker_pin`, the same stable-slot +
  sweep + bracket discipline as timers/XHR) + per-engine adapters: XS NR hosts a rooted prototype
  with `xsNewHostConstructor` instances (the SW5 constructor-flag finding applied), duktape builds
  per-instance-method wrapper objects with null handler props, mujs mirrors its userdata-with-box
  XHR shape; all three pin the wrapper engine-side (`xsRemember` slot; duktape stash heapptr
  `\xFFpluto.wobj.<id>`; mujs `js_ref` registry key) through `jsbridge_worker_pin` so the pump can
  reach it next frame. The worker SCRIPT never compiles, is never fetched, never executes:
  postMessage is absorbed silently (receipted logarithmically — two logged per stub then a single
  "silently" line, so a post-loop page cannot flood the log), terminate BEFORE delivery cancels the
  pending error (browser: the load was stopped), after = a no-op. The `error` event fires ONCE at
  the next pump frame with `ev = {message}` (never synchronously — the failed-script-load analog);
  the CURRENT onerror is consulted at fire time (browser at-fire semantics — handlers may be
  (re)assigned after construction); a stub without a callable onerror delivers nothing; every
  delivery rides the SAME metering/try-catch/abort bracket as the XHR completions (abort → engine
  reset). Documented caps: 8 stubs resident per page + delivery-less shapes beyond (the page keeps
  running; logged once). **Probe receipts (all three engines):** `[worker] construct #1
  url='workerstub.js' (contained stub — script never compiles or runs)` → `[worker] postMessage #1
  absorbed (stub: data dropped — no thread)` → the battery's `[p8w] worker onerror: the worker
  script is not running on this device (single-threaded browser: worker stub)` console row →
  `[worker] onerror delivered #1 present=1`. **`apisum` → `pass=14 miss=0` on XS** (the P0 battery
  is ALL-PASS at last); mujs and duk flip their `worker` row inside their standing shape
  (`9/5` → `10/4`, the remaining 4 = the documented frozen-engine rows, `currentscript` among
  them). Gates re-run on the final source: evtest byte-identical at every stage
  (`52/56/57/58/59/61/65` with unchanged bytes — `blocks=65 (was 61)`, `[ev-test] PASS` again:
  input=1 change=1 submit=1 bub=1 btn=1 doc=1), apicon `pass=8 miss=0 sent=17` standing,
  `storage: PASS` with the quota rows standing, visits ledger 13/14/15/16 accounted (continues
  1→11 from P7 + 12 from the first P8 sim). The P7 real-page gate receipts (tap1/jsext/HN/Bryan/wiki)
  remain valid — their pages never render the probe and the lane is additive (a new global +
  a new pump site; no existing road touched). Receipts banked:
  /tmp/api_p8_{probe_final,evtest,smoke_mujs,smoke_duk,prod}.txt. Score 92→95.
- [x] **API-P9 Wrapper identity + className accessor** *(M)* — **CLOSED 2026-10-20.** The two
  remaining parked DOM-fidelity gaps from P6 land together. **Wrapper identity:** the browser
  identity model — ONE wrapper object per DOM node for the page's whole life — now holds on
  EVERY engine. A bridge-scoped per-node cache (`JsElwEntry elw[JSBRIDGE_ELW_MAX=128]` +
  `elwCount/elwOverflow` in jsbridge_internal.h; the bridge is memset(0) per page attach, so the
  cache is fresh per page) consults each engine's element factory before building: a hit pushes
  the cached wrapper, a miss builds fresh + pins it engine-side. Engine-held refs per the
  established pin contracts: mujs = a `js_ref` registry key (stable interned object key; popped
  dup; unref'd at page close in the listener-loop precedent), duktape = a heapptr whose only
  strong ref lives in the global stash under the hidden key `pluto.elw.<i>` (i = the entry
  index; the heap dies with the engine at page close, as documented), XS NR = untouched (its
  own P1-lane state arrays + xsRemember slots). The cache is aliasing-safe within a page:
  `dom_remove_child` DETACHES nodes (never frees), so a node pointer stays unique for the
  page's life; the whole DOM frees with the doc, after the engine close. Beyond cap = honest
  degradation (logged once: fresh wrappers from there on — none of the gates hit it). **className
  accessor:** the P6 parked gap closes on all three engines — reads map 1:1 onto the `class`
  attribute (never NULL; `dom_get_attr` at each adapter), writes ride ONE shared road in
  jsbridge.c (`jsbridge_el_class_set`: router owns the single budget charge, then the
  classList family's `cls_before`/`cls_after` snapshot/dirty-receipt pair — rewalks and the
  transition-lane note see it exactly like `setAttribute('class')`, whose store is identical).
  **Battery:** apidom gains its 7th row `classnames` in place (setter + read-back, the
  `getAttribute('class')` mirror, a rewrite, and the setAttribute reverse-mirror) — 6→7, no
  reorder of the standing six; the identity rows needed no battery change. **Probe receipts
  (all three engines):** `closest: PASS` (mujs/duk were `MISS:badup` — fresh ancestor wrappers),
  `appendret: PASS` (mujs/duk were `MISS` — fresh return wrappers), `classnames: PASS`,
  `apidom: pass=7 miss=0` (mujs/duk were `4/2`; XS was `6/0`); standing everywhere:
  `apisum: pass=14 miss=0` on XS, `pass=10 miss=4` on mujs/duk (the documented frozen-engine
  rows, `currentscript` among them), worker chain (`[p8w]` + `delivered #1 present=1`),
  apicon `pass=8 miss=0 sent=17`, `storage: PASS` + quota rows, visits ledger 17/18/19/20
  accounted (continues 1→16 + the four P9 gate boots). Gates re-run on the final source:
  evtest `[ev-test] PASS` with every stage exactly +1 block (`53/57/58/59/60/62/66` — the
  classnames row; counters input=1 change=1 submit=1 bub=1 btn=1 doc=1, `jsErrors=1 (before 1)`
  unchanged), probe `apisum 14/0` + apidom `7/0` (three engine builds), production restored +
  boot-verified (5 heartbeats, no autotest rows). Receipts banked:
  /tmp/api_p9_{probe_final,evtest,smoke_mujs,smoke_duk,prod}.txt. Score 95→96.
- [x] **API-P10 Async http(s) module graphs + dynamic `import()`** *(L)* — **CLOSED 2026-10-20 (all gates green; the API Missing list EMPTIES — the browser-API-surface row goes COMPLETE).**
  The full async lane on the standing P5 module road (transform-in-C, every engine gets the
  same lane, engine sources untouched), all receipts:
  1. **Transport in the router, `http_client.c` untouched** — the single-flight client keeps its
     cancel-first parity: the lane serializes ONE wire fetch behind `http_is_loading()==0`, and
     `http_get`'s contract was **corrected against the receipts (0 = refused-to-start, 1 =
     STARTED, possibly still in the access-arm wait — the inverted check of the first build
     swallowed every successful start)**. Per-flight watchdogs: ~20s silence/re-supersession
     requeue ONCE (receipted), ~40s terminal refuse.
  2. **Bridge-scoped flight table** `modFlight[6]` (`JsModFlight`: key/importBase/src≤16KB/
     depth/index/pendCount/childKeys/impLoaded/epoch/state) — fresh per page (the attach
     memset(0)), epoch-tagged with the standing `g_xhrEpochSeq`; ledger `modState` gains
     **3 = remote-flighting**; stale runs sweep with receipts.
  3. **Run-lane restructure** — `jsbridge_module_run_key` serves http(s) module-RELATIVE through
     `url_resolve` exactly like the about:/file roads (`data:`/`javascript:` stay refused);
     ledger ensure → flight enqueue → pending; parents **park** holding their source;
     `mod_finish_run` runs at drain; completion cascades parents by key (pending counts down).
     receipts prove the full chain: `net: enqueue remote module … (depth 2)` →
     `parked 'file://p10relay.js' on 1 remote child(ren)` → root parked →
     `net: delivered 12643 bytes status=200` → `drain: … (depth 2)` →
     `cascade: 'file://p10relay.js' settled child …` → `drain: 'file://p10relay.js' (depth 1)` →
     cascade → `drain: 'file://modpage.html#p5mi2' (depth 0)`, all in ONE pump frame.
  4. **Dynamic `import()`** — `mod_scan` records rename spans at ANY brace depth (the
     "(nested)" receipts) word-boundary-lexed with strings/comments/templates protected; the
     dot-preceded-word guard ALSO fixed a standing P5 cut bug (`obj.import(...)` kept as a
     property call). `mod_build` emits `__p5di(` for each span + a per-module prelude binding
     the module's own importBase so calls stay one-arg. Classic-script `import()` stays the
     engine's reserved-word handling (boundary documented).
  5. **Shim + native bridge** — the ES5-capable page shim (`globalThis.__p5dynimport` +
     the `__p5dynq` ticket registry) injects ONCE per page at module_root (receipts
     `p10 shim: injecting (before module script N)` + `latched`); each engine gains the native
     `__p5modRequest(spec, base)` returning `k:/p:/x:` strings — mujs `js_newcfunction`,
     duktape `duk_put_global_string` (before the xhr block), XS `xsDefine` (BUDGET_OR_THROW).
     Tickets settle at the pump via tiny `.r(...)/.j(...)` evals — never re-entrant
     `run_script` inside the native.
  6. **Module pump** — `jsbridge_modules_pump(JsBridge*, nowMs)` from the per-frame pump site
     (main.c `js_timers_update`, after xhr_pump): HOLD-drain flight processing, the watchdogs,
     the flood-head start, dynq settle evals; a 1Hz `pump-diag` receipt while flights are
     armed (kept: it root-caused the modpage freeze below and stays silent otherwise).
  7. **Battery — 4 new rows, `apimod: kids=7 refuse=PASS`** (the plan's `kids=6` text correction:
     the row helpers live on `globalThis` so a throw inside a promise handler can never reject
     the `.then` chain — that throw was the run-1 `unhandled rejection` abort, fixed by the
     global-writer lane): `moddyn` (dynamic about: import + namespace asserts), `moddynid`
     (double-import `===` identity — both tickets resolve via the `k:` road), `modmiss`
     (unknown module → the promise REJECTS with the browser message class `Failed to fetch
     dynamically imported module: <spec>`, the catch handler asserts the text), `modnet`
     (the pinned `punycode@2.1.1` CDN module — conditional-PASS: fetched → assert exports,
     failed → assert the rejection; the fetched branch asserts `encode/decode/toASCII/…` for
     real). **List exports ESCALATED to real support**: `export {local[, local [as exported]]}`
     now PARSES + materializes (aliases included — `mod_add_export_pair` + the `expLocal`
     table; receipts `export {list} parsing (P10 list form)` + `export {list} registered 6
     names`); `export {list} from 'spec'` stays the standing boundary refusal.
  8. **Gate catches fixed along the way** — ledger cap 8→12 (the https + the 9th inline module
     cap-refused at 8, receipted); the modpage freeze ROOT-CAUSED with the pump-diag: the
     NR-autotest revisit pass was tearing the fixture page down (the normalized
     `file://modpage.html/` open-fail → state 3 → `g_pageJs` gone → no pump); fixed two ways:
     the `file:` fixture skip in the autotest (the standing `about:` probe precedent — a
     revisit servives a pre-chain snapshot with no JS eval either way) and the GENERAL
     file-loader trailing-slash tolerance to a real file referent (`file://x.html/` now opens
     `x.html` with a receipt — a real bug my diag exposed).
  9. **Frozen-engines upgraded, honestly** — mujs + duk keep their documented ceilings
     (`apisum: pass=10 miss=4` exact): the battery feature-DETECTS the import path
     (`typeof __p5dynimport !== 'function' || typeof Promise !== 'function'` → the rows write
     honest `MISS` instead of throwing — `apimod: kids=5 refuse=FAIL` on both + the ceiling
     log `[apimod] modnet: no dynamic-import path (engine ceiling)`). AND the P10 body-error
     receipt lane (`[module] body error in '<key>': <msg>` + mujs `[js] script N threw`)
     SURFACED a standing silent mujs death: muJS has no `globalThis`, so EVERY module body
     died ReferenceError on the prelude since P5 (hidden behind the sticky first-error
     channel). Fixed bridge-side with the ES2020 identity alias — the P5 chain is ALIVE on
     mujs now (the standing suite runs where compile ceilings let it; the p5mi1 refusal
     throws now receipt properly).
  10. **Final gate set (all green, receipts in `/tmp/api_p10_*.txt`)** — XS probe:
      `apimod: kids=7 refuse=PASS` + all 4 rows + every standing sum exact
      (`apisum 14/0`, `apifetch 4/0`, `apihist 7/0`, `apicon 8/0 sent=17`, `apidom 7/0`,
      `P4COUNT 17`, storage/worker PASS, MODX-LEAK absent); modpage:
      `modpage: pass=3 miss=0 kids=3` + the full static file→file→https chain receipts +
      `reuse` discipline (never a second wire fetch for a landed child); evtest: **PASS** with
      the +4 exact stage accounting (60/62/66 → 64/66/70); mujs/duk smoke gates as in (9);
      prod restore clean (0 autotest rows, 50fps heartbeats, refusals=0).
- **Honest non-features (contained stubs + documented policy names):** service workers, IndexedDB
  stay told "not available on this device" and the page keeps running; workers are no longer a
  pure refusal — the contained-stub lane ships the constructor + the standard onerror receipt
  path (API-P8 above).
- **Goal: DONE.** ~50% → ~96% (through P9) → **100% sealed by API-P10's async module lane** —
  the Missing list emptied and the row went COMPLETE (2026-10-20), with the non-features
  policy-documented rather than faked.

## Update log

- **2026-10-20** — **API-P10 CLOSED: the browser-API-surface program finishes end-to-end.** Async
  http(s) module graphs + dynamic `import()` landed (router-side single-flight transport,
  bridge-scoped flight table with park/cascade drains, ES5 page shim + per-engine native
  `__p5modRequest`, module pump; list-export form materializes with aliases; `JSBRIDGE_MAX_MODULES`
  8→12); probe battery grew the four async rows (`apimod: kids=7 refuse=PASS` on XS, honest MISS
  rows + strict feature-detects on mujs/duk `kids=5`); file gate `modpage: pass=3 miss=0 kids=3`
  on the full static file→file→https chain; the autotest's `file:` fixtures skip the
  snapshot-revisit phases (the `about:` precedent) and the file loader now tolerates the
  normalized stray trailing slash on file referents; the body-error receipt lane
  (`[module] body error` / mujs `[js] script N threw`) + the mujs `globalThis` identity alias
  surfaced and fixed the standing silent mujs module-prelude death. Gates re-run green on
  engines 4/1/0 + evtest (the +4 exact phase-1 stage accounting) + prod restore clean.
  Status column COMPLETE (100%), Missing list emptied. Device untouched (sim only).

- **2026-10-18** — initial snapshot. R56g–R60 recorded in `SESSION_PLAN.md`
  (form fallback engine, domDirty marks, boot auto-nav removal, re-walk
  contract + layout guard, script residency + chunked routing). Device
  deployed + boot-verified; sim regression green (tap1 ×3, TAP2 ×3 exact
  baseline). Gitbook state: fixes branch pushed; AVIF merged to main;
  avif branch deleted local+remote.
- **2026-10-18 (later)** — Roadmap TODO added (user-approved start of the
  HTML+CSS 100% program). Git: user-locked — no commits/pushes by the agent
  until instructed.
- **2026-10-18 (Phase 0 first receipts)** — skip-class instrument built +
  verified exact on `about:cssprobe` (v1 fixture live in route table +
  bookmarks + snap bypass; harness skips snapshot phases for about: probes).
  Honored-property vocabulary audited: ~27 names (12 via apply_prop + 15
  via query sites). Two gap receipts: pseudo-element content silent (P7);
  external `<link>` stylesheets unfetched (usage pass item). TAP2 anomaly:
  site home page breaches the engine wall at hydration (×3, cache-
  independent); tap1 green exact. All sims killed after runs.
- **2026-10-19 (Phase 0 CLOSED)** — Instrument compile-gated
  (`PLUTO_CSS_SKIP_TALLY`, default 1) so an off-instrument rig exists without
  git surgery. Off-instrument TAP2 reproduced the abort byte-for-byte →
  anomaly SEALED: site content at the engine-wall margin, containment held
  every time; tap1 ×3 green; TAP2's cycle baseline is content-blocked until
  the site shrinks or the wall margin grows (R61 idea, forwarded). Real-page
  pass: Wikipedia `prop=41` + 688 blocks green; bryanwandrych.com and HN
  ship external-only CSS (engine never spins). Pseudo-element content lane
  confirmed silent (Phase 7 target). `about:cssprobe` re-verified exact
  after gate restoration. Sim-only per user (remote session). TEMP-counter
  removal plan written, awaiting the word.
- **2026-10-19 (Phase 1 CLOSED)** — Selector language finished. Built: `+`/`~`
  sibling combinators (rule parser + `css_parse_selector` for querySelector;
  matching via a bounded 256-test backtracking search that only engages when a
  chain contains a sibling combinator — all-descendant/child chains keep the
  R57-fast ancestor walk byte-for-byte), attribute operators (`~= |= ^= $= *=`
  parse + match; value reader rebuilt so operator rules actually store their
  compare value — the first build silently collapsed operators to presence
  tests, caught by the probe's KEEP controls), simple `:not()` (one simple
  token: tag/.class/#id/[attr]-presence; comma lists and value compares
  still refuse), and interactivity states `:hover/:focus/:active/:visited`
  = pclass 8 ALWAYS-MATCH (policy (a), documented; one line to reverse).
  Gauge 0.6: unknown-property NAMES recorded (top-5 gliding) →
  `[css-propnames]` row; Wikipedia's 41 unknowns now named:
  `border-color x24 clear x2 list-style x2 flex-shrink x1 counter-reset x1`.
  Fixtures: probe v2 (hide-based canaries + controls per feature; checker
  in main.c scans the FULL block list, `[cssprobe]` rows assert per canary).
  Receipts: probe `PASS (0 fails)` + tally exact `sib=0 attrOp=0 pcls=0
  medF=2 var=1 calc=1 prop=5` ×2; tap1 ×3 identical green
  (12:09:55 / 12:10:49 / 12:11:39); Wikipedia re-run: prop=41 unchanged,
  688 blocks/672 links, 18ms — the new selectors cost NOTHING on pages that
  don't use them. Engine notes: `CssSimple` +8B (sheet-text pointer +
  3 bytes) ≈ +65KB worst-case per-doc engine heap (fixed-size tables, freed
  at page swap); `siblingInfo` DOM hook added (NULL = sibling combinators
  never match, graceful). Sim-only session; all builds sim; every run killed
  + verified 0 procs.
- **2026-10-19 (Phase 0.3 EXECUTED)** — TEMP Wikipedia-stall workload
  counters REMOVED (all 9 statics, the `[css-stats]` row, every increment
  and the wt0/t0 timers). `css_log_stats` remains as the
  `[css-skip]`/`[css-propnames]` emitter, now compile-gated entirely.
  Verify: probe `PASS (0 fails)` tally BYTE-IDENTICAL; tap1 green
  identical (`12:28:14 inPlace=yes jsErrors=0 blocks=23 errCard=1`);
  zero `[css-stats]` rows anywhere. One build-hygiene lesson re-learned
  twice this session: `touch` + `setup.o` removal is required on EVERY
  SIMDEFS change including the plain production build (make otherwise
  keeps a stale dylib — caught by strings count, re-verified clean).
- **2026-10-19 (Phase 1 FULLY CLOSED)** — see P1.5 row for the full
  receipt set. Summary: TEMP strip executed (above) + external-stylesheet
  fetch LIVE (Stage C). Post-1.5 gates: probe exact; tap1 ×3 identical
  (each with the live `7618B` fetch receipt); Wikipedia unchanged with
  `load.php` 98304B loaded (cap); HN + bryanwandrych.com home joined and
  stable (fresh + rewalks). Scorecard 75→78. Repo restored to plain
  production sim build (0 errors, harness strings 0, clean boot,
  `css-ext` silent at idle); deployed device build untouched; git still
  user-locked (nothing committed).
- **2026-10-19 (Phase 2 CLOSED)** — `var()` + `calc()` RESOLVE. Custom
  properties: engine-level doc-order table (name/value via sheet text;
  `add_prop` intercepts `--name` before the rules table AND the names
  gauge; last-wins, importance beats order; caps 24×11-char). A
  lookup-time resolution layer (`css_resolve_value`) fronts BOTH value
  exits (winning_core return + apply_prop consume): var(--name[,
  fallback]) substitution (one level, balanced-paren fallback, exact
  case-sensitive names) then whole-value calc(). Calc: expr/term/factor
  recursive descent (depth≤8), px/%/unitless milli-unit fixed point,
  "Npx"/"N%" canonical output; em/rem/etc, mixed-unit, composites
  REFUSE → raw value flows (pre-Phase-2 behavior byte-identical for
  taint-free values — parity guarantee). Probe v3 (new canaries):
  `PASS (0 fails)` — VAR-HIT hides via walker resolution, VAR-KEEP
  guards leaks, VARFALL-HIT proves fallback, direct rows
  `calc-width=64px` / `var-display=none`; tally
  `sib=0 attrOp=0 pcls=0 medF=2 var=3 calc=2 prop=5` exact. Gates:
  tap1 ×3 identical green (13:26:53/13:28:28/13:30:05, live 7618B fetch
  receipts each); Wikipedia +1 block (689 — FIRST live render delta
  from a resolved skin-sheet value; links/prop/names unchanged, no
  stall, the 1.5 byte-cap skip row logs as built); HN + bryanwandrych
  unchanged. Engine notes: ring scratch (4×72B) co-resolves paired
  lookups; resolution aborts on overflow (never truncates); checker
  self-bug fixed (per-query mask markers); `:root` pseudo-class stays
  refused → fixture custom props ride a plain class. Scorecard 78→80.
  Repo restored to plain production sim (0 errors, strings clean, boot
  green, idle heap 39KB); device untouched; git user-locked.
- **2026-10-19 (Phase 3 CLOSED)** — @media evaluated FOR REAL,
  general-purpose. The existing evaluator's vocabulary grew to the
  full MQ4/5 set against a documented device truth table (400×240,
  dppx 1, monochrome, hover:none / pointer:coarse, light scheme,
  reduce motion/transparency, more contrast, forced palette,
  standalone, SDR, LCD-fast, scroll, opaque). Length grammar:
  px/em/rem(16px base)/vw/vh + MQ4 unitless-zero, milli-px exact
  compares (width==600px stays exact); aspect-ratio A/B + bare
  (integer cross-multiply); resolution dppx/dpi/dpcm/x (+webkit
  device-pixel-ratio aliases, bare = dppx; 2x branches false → 1x
  assets serve). Unknown features still census + evaluate false.
  Link media= attr now runs the SAME evaluator (forward-decl from
  css_ext_scan_links; print honored per the 1-bit design; "" =
  every medium). Probe v4: `PASS (0 fails)` — 9 new media canaries
  (min-width held, 9000px dropped, 22.5em lane, unitless-0 +
  landscape chain, not-negation keep, comma-OR, dark-false keep,
  contrast-more HIT, unknown-feature containment keep), tally exact
  `medF=1 var=3 calc=2 prop=5`, all prior canaries byte-stable.
  Gates: tap1 ×3 identical; Wikipedia PIN-stable (desktop min-width
  breakpoints correctly false at 400px — negative-direction
  containment proven live); HN + bryanwandrych unchanged incl.
  rewalks, no wall perturbation. Scorecard 80→82. Plain production
  restored (0 errors, clean boot, 39KB idle); device untouched; git
  user-locked.
- **2026-10-19 (Phase 4 CLOSED)** — Positioning & z-index: the css.h
  "all static" note replaced by a SPEC-ANCHORED positioning lane.
  Vocabulary consumed in apply_prop: position fixed/absolute =
  CSS_F_OOF; top/left/right/bottom with any non-auto value =
  CSS_F_OFF (0px is an offset; auto/inherit/initial/unset carry
  none); z-index with a numeric value = CSS_F_OZ (invalid
  declarations carry no signal). Walker skip-gate policy:
  out-of-flow WITH an overlay signal = page chrome, DROPPED (a
  compositor would draw it on top; the flat flow has no overlay
  layer — reader-mode precedent, one-line reversible). Out-of-flow
  WITHOUT a signal = the CSS static-position rule: flows exactly
  where written. relative/sticky flow (sticky chrome scrolls away
  rather than eating the 240px viewport). z-index alone inert.
  Design receipts gathered BEFORE implementation by curling both
  gate-site sheets: HN = zero positioning (lane provably inert);
  bryanwandrych.com = .skip + .hp off-canvas chrome suppresses
  (fidelity UP — they rendered as stray blocks before), ::before
  rule untouched, nav overlay display:none already, sticky/relative
  flow. Probe v5 `PASS (0 fails)`: 6 new canaries (FIXED-HIT,
  ABS-HIT, ABSOFF-HIT dropped; ABSSTATIC-KEEP static-position rule;
  ZKEEP z-alone inert; REL-KEEP), tally `medF=1 var=3 calc=2
  prop=4` exact (position family left the names gauge). Gates:
  tap1 x3 identical; Wikipedia PIN-stable 689/672 (vector
  positioning lives in already-evaluated media blocks — Phase-3
  synergy); HN links 230->229 = upstream front-page drift (curl:
  zero positioning surfaces, lane cannot act); bryanwandrych 48/18
  stable incl. rewalks. Scorecard 82->84. Plain production restored
  (0 errors, strings 1/0, boot green, 39KB idle); device untouched;
  git user-locked. Out of P4 scope (documented): floats remain
  census-only — P6 flex/grid supersede them on modern sites; 2-D
  compositor semantics do not exist in the flat-flow renderer.
- **2026-10-19 (Phase 5 CLOSED)** — Overflow regions: the flat page IS
  the only scroll region; nested regions cannot exist. Vocabulary
  consumed: overflow (hidden arms CSS_F_OVH; auto/scroll/visible =
  inspected no-ops), text-overflow (ellipsis -> CSS_F_ELLIP == DOC_INF_
  ELLIP ABI bit, non-inherited, carried within the declaring subtree at
  the element-open merge), clip/clip-path (concealing shapes ->
  CSS_F_CLIPN: inset with nonzero draw / legacy rect), and
  -webkit-line-clamp (count rides the walk_box_spacing hint transport
  into DocBlock.clampLines -> BreakOpts.maxLines). Walker policy:
  position:absolute/fixed + overflow:hidden + (clip shape OR width&
  height <=2px) = the W3C visually-hidden recipe -> dropped (author
  intent; BW carries the recipe verbatim in .card-hit>span). Breaker
  policy: text-overflow:ellipsis + nowrap cuts the overflowing single
  line; -webkit-line-clamp:N caps N wrapped lines — both append a
  "..." tail (first clamp attempt put the tail on a phantom +1 line;
  fixed to cut AFTER the budget-filling push). Probe v5 PASS (0
  fails): SROFF-HIT/SRTINY-HIT drop, SRKEEP/OVKEEP boundaries keep,
  ell-clip lines=1 width=371 and clamp-lines lines=3 want 3 (layout-
  level structure assertions via the new battery probe
  layout_test_probe_maxlines — doc objects keep full text by design so
  the cut can ONLY be asserted between breaker and emitter). Census:
  width/height joined the known list (the fixture exposed the gap —
  table code always queried them); tally exact prop=4. Gates: tap1 x3
  identical; Wikipedia pin-stable 689/672 (prop 41->40 = the gauge
  receipt); HN 10 blocks (230 links = upstream drift again); BW 48/18
  byte-identical census. Scorecard 84->86. Plain production restored
  (0 errors, strings 1/0, boot 39KB idle); no new warnings in touched
  files; device untouched; git user-locked. Residuals documented:
  DIV-wrapper clamp flattening ambiguity (wrapper clamp honors only
  when the wrapper IS the text block), text-overflow:clip census-only
  (no second ABI bit), size-only sr variants (no clip/tiny) keep
  flowing.
- **2026-10-19 (Phase 6 CLOSED)** — Flexbox & Grid core (P6a+P6b, the
  column-axis honest subset): the flat 1-bit page IS the column axis, so
  the flex/grid container lane consumes what the stack honors and leaves
  the 2-D distribution family as census vocabulary (css.h policy
  paragraph). Containers: display:flex/grid -> CSS_F_FLEXCT (walker
  lane bit; the container box stays a block) + the family joined
  known_prop (gap trio, flex-*, order, justify/align/place, grid
  templates/areas/auto — census names consumed live on the BW sheet:
  gap x17, align-items x7, justify x5, grid-template-columns x3).
  Gap transport: the gap AFTER each child block mounts as
  DocBlock.gapApplied (halved-px scale: 16px -> 8; rem/em rides a
  fraction-precise fixed point — .75rem = 12px exact, BW parity; vw
  x4px, pt x4/3; % refused, negatives refused; row-gap wins the
  shorthand; flex scope ALWAYS replaces the outer knock-on — even
  gap:0 closes the outer slot between items) and a LIFO tail rule at
  the container's CSS exit closes the LAST child's slot to the outer
  scope's gap (dropped own-blocks die cleanly with the drop). Centering:
  align-items:center gated on the FLEXCT+AIC carry (inert on plain
  block boxes, CSS-correct), align-self:center element-scoped;
  walk_flex_block mounts at ten block-open sites (own HTML align wins)
  and layout consumes gapApplied at all six branch advances — gap is
  real vertical rhythm. Probe v6 PASS (0 fails): 13 new canaries assert
  mounts DIRECTLY off DocBlock objects (FIXGAPA gap=8 / FIXGAPB gap=0 =
  the last-item tail rule exact; .5rem -> 4; row-gap 12px -> 6 beats the
  shorthand; FAICC/FAICD/FXSB align=center; FXNA/AINC/FGNPX inert
  boundaries), tally exact prop 4->2 (gap/grid + flex family left the
  gauge), P1-P5 canaries byte-stable. Gates: tap1 identical (blocks=23
  errCard=1); HN 10/229 stable; Bryan 48/18 stable census byte-identical
  (the sheet's real 1rem/.75rem gaps mount inside existing spacing); the
  Wikipedia gate caught an UPSTREAM page change (curl-proven:
  wiki/Playdate now serves a 67KB play-date stub page 132/85; the
  console article moved to wiki/Playdate_(console) — future gates use
  it) with the prop collapse 40->3 = the expected gauge receipt on
  vector-2022's flex-heavy sheet. (The nr-autotest revisit tail row
  FAILs by its own cache-off nav-start purge contract — fires identically
  on the harness's own page with byte-stable renders; not a P6
  regression.) Scorecard 86->88. Plain production restored (0 errors,
  documented warning set only, boot green 40KB idle — +1KB = fixture
  text); sims killed after every gate (verified 0); device untouched;
  git user-locked. Residuals documented: 2-D distribution = policy
  non-feature; inline-only children carry no horizontal gap; stray
  after-last-child text un-mounted; the between(nested-child,
  inner-box) prev-side slot unmountable; inline-style gap not
  transported (sheet-only lane); rem margins stay the P9 residual.

### 2026-10-19 — P7 pseudo-elements + focus states CLOSED (88→90)
  Generated content LANDS: walk_pseudo_emit's punctuation-only guard
  was silently killing BOTH pseudo lanes (the P0 `>>` canary class died
  by it) — refined so the stray-line guard drops ONLY block-lane
  siblings while the inline lane emits (span before+after inline,
  quotes/escapes/U+00A0 resolved, attr(data-url) brackets print; the
  acidtest [href] canary stays on its own fixture). ::marker = lane 4:
  li opens query it, the text rides DocBlock.markerText (NULL default
  glyph, "" removes bullet, none/normal keep default) and layout draws
  it as the bold bullet. strip_pseudo grew 4→15 entries: marker +
  first-line/first-letter/selection/placeholder/backdrop/view-transition
  = consumed-inert lane 3 (the compound-drop class from real sheets
  gone; an unknown ::-vendor tail now counts in the census instead of
  vanishing). Truth pseudo-classes with HONEST matchers (not
  always-match): :disabled/:enabled/:checked/:placeholder-shown/:root/
  :target/:empty (spec-exact comment-only-counts-empty via the new
  CssDom.childless accessor); :target's fragment fills from
  doc->baseUrl at every walk (css_set_target; case-sensitive; no
  fragment = nothing matches); :focus-visible/:focus-within joined the
  Phase-1 always-match family (policy (a)). Probe v7 PASS (0 fails),
  95 rows: marker doc-field asserts exact (MK7 + "" = bullet removed),
  root-var/checked/disabled/enabled/placeholder-shown/empty hide
  through real matcher hits with un-disabled/un-checked boundaries
  visible, focus-within hides, target keeps (fragment-less URL ->
  the honest negative); tally exact sib=0 attrOp=0 pcls=0 medF=1
  var=4 calc=2 prop=2 (pcls stays 0 — the truth + consumed ::-family
  never reach it; var 3->4 = the :root var; sib/attrOp = the retired
  always-zero columns, fixture header now says so); P1-P5 canaries
  byte-stable. Latent ABI bug caught in the lane work: css.h declared
  6 params for css_pseudo_content vs 8 defined (C tolerated it) —
  header fixed in lockstep; the ::after call site still passed legacy
  0 (now meaning "element rules") — caught by the first probe run,
  fixed to 2. Real-site receipts (curl): vector-2022 ::after x54
  ::before x10 :root x2 :empty x4 :checked x2 :focus-visible x2 — all
  compound-drops before, all consumed/matched now (Wikipedia census
  pcls=0); Bryan Next chunk ::marker x1 :disabled x2 :focus x2 :root
  x1; HN news.css neither family, all-zero census stands. Gates: tap1
  byte-identical (inPlace=yes blocks=23 errCard=1); HN 11/226
  fresh==rewalk (229->226 upstream drift, byClass pre-exists, 0
  ERRORs); Bryan 48/18 stable all-zero census byte-identical;
  Wikipedia RE-PINNED to the new gate URL wiki/Playdate_(console)
  (the P6-closure upstream move): 324/310 fresh==rewalk stable,
  first-walk census pcls=0 var=5 prop=17, 0 ERRORs. Scorecard 88->90.
  Plain production restored (0 errors, documented warning set only,
  boot green 41KB idle — fixture/field growth, stable heartbeats);
  sims killed after every gate (verified 0); device untouched; git
  user-locked. Residuals: first-line/first-letter/selection/
  placeholder/backdrop = consumed-inert policy names (per-line/per-
  char typography not a 1-bit-flow behavior); counters()/url()/quotes
  in content still drop the whole value (P0 policy); block-lane pure
  punctuation stays guarded; :target updates ride full navigations
  (no scroll-spy); focus/hover/active/visited remain always-match
  (device truth, reversible by one parse line).

### 2026-10-19 — P8 HTML interactive set CLOSED (90→92)
  The recon found four of the five named features already deep
  (details toggle + open-attr + summary extraction w/ closed children
  skipped; dialog open-attr gate = closed renders nothing; template
  inert; select option/optgroup collection + receipt-tested tap
  cycling) — the phase built the genuine gaps. SRCSET/SIZES:
  `img_pick_srcset` parses the spec candidate grammar; width
  descriptors take the SMALLEST >= CSS_VIEWPORT_W(400) else the
  largest, densities prefer exact 1x else smallest >=1 else the
  closest-to-1 sub-1, bare lists take the first, mixed layer
  w->x->bare, unusable parse -> the plain src stands (spec fallback),
  and a usable srcset REPLACES src even when both exist. Both image
  sites rewired; `<picture>` dropped its "last source wins" inversion
  (FIRST source whose media= holds, via the newly exported
  css_media_holds — the @media evaluator as a public fn; type= needs
  no gate, the decoder dispatches by magic). sizes/loading/decoding =
  consumed-inert vocabulary (documented in the helper comment). RESET
  BUTTONS: `<button type=reset>` had produced NO block — arm fixed;
  DocBlock.buttonType carries the element type; tap flow = click
  dispatch (JS may preventDefault) then the HOST reset, never a
  submit; no-JS pages run the host reset directly; formless resets do
  nothing. Reset basis = 4 captured defaults (defaultValue,
  defaultChecked, defaultSelIndex, DocOption.defaultSelected);
  host_form_reset walks same-form items (the serializer's formAction
  test), restores all four, re-walks only when changed. MULTIPLE
  SELECT: taps accumulate (arrival marks selected), submission loops
  EVERY selected option as its own name=value pair (the seen[] dedupe
  bypassed — the name is supposed to repeat); singles byte-identical.
  SUMMARY-LESS details prints the user-agent "> Details" toggle line
  (was an empty invisible fold). Probe v8 PASS (0 fails): 6 text rows
  (template inert, dialog pair, closed-details body hidden) + 4 srcset
  doc-field asserts (PLAINIMG keeps src; SRCSETW three.png = smallest
  of {200,900,480} covering 400; SRCSETD van@1x = exact-1x preference;
  SRCSETF unusable srcset= keeps src) + msel model (3 opts,
  multiple=1, selected [1,0,1]) + the button battery (RNRESET type=
  reset; FMBTN = the bare-button default-type receipt) + the
  details-default label row (label, not inline — off the k[] battery
  by design); tally byte-stable sib=0 attrOp=0 pcls=0 medF=1 var=4
  calc=2 prop=2 (HTML-only canaries) + P1-P5 canaries byte-stable.
  Real-content receipt: the Wikipedia gate shows live "[img] srcset
  pick:" rows — 250px thumbs upgrade to their 500px candidates under
  the panel policy. Gates: probe v8 PASS; tap1 inPlace=yes jsErrors=0
  (was 0) blocks=23 errCard=1 byte-identical; HN 11/225 fresh==rewalk
  (curl-proven upstream drift, 30 titlelines live; byClass pre-exists;
  census all-zero; 0 ERRORs); Bryan 48/18 stable incl. rewalks,
  census byte-identical all-zero, 0 ERRORs; Wikipedia 324/310 EXACT
  baseline + census exact pcls=0 medF=0 var=5 calc=0 prop=17,
  fresh==rewalk, 0 ERRORs. Scorecard 90->92. Plain production
  restored (0 errors, documented warning set only; boot green 41KB
  idle stable heartbeats refusals=0); sims killed after every gate
  (verified 0); device untouched; git user-locked. Residuals:
  template.content stays out of the JS bridge (render-side inertness
  is spec-render-correct; SPA cloning = a JS-phase bridge item);
  select size>1 = a one-line field (no multi-row widget on the flat
  panel; size consumed); multiple-select has no deselection UI (the
  reset button or a JS option.selected rewrite is the way back; full
  re-walks rebuild options from the DOM); dialog stays an open/no-
  open render gate (no showModal; ::backdrop rides the P7
  consumed-inert lane).

### 2026-10-19 — P9 fidelity polish CLOSED (92→94)
  Phase-9 greening of the 1-bit flow, on the four prior probes' receipts.
  (1) **One shared unit grammar** (`fresh_len_px`, document.c): px/bare,
  rem/em ×16, vw 400/100, vh 240/100, **vmin 240 / vmax 400**, ch/ex ×8
  (approximations documented in the header), pt 4/3; % answers as
  per-mille ONLY when the caller opts in; sign + fraction (r/p) kept.
  Consumers rewired: `walk_box_px` falls back into it (the P6 rem-margins
  residual LANDS), `gap_len_px` re-bodies through it (cap + no-neg + first
  token kept), `box_num` stops reading `%` as pixels (the legacy
  `"50%"`→25px wrong turn repaired — % now honestly refuses), the Milli
  @media parser consumes vmin/vmax. (2) **CSS translate() = a real paint
  offset**: `parse_translate` composes translate/X/Y (px + %, other fns
  skipped), `walk_tf_fill` mounts halved px + %-of-own-box on the new
  `tfXpx/tfYpx/tfXpct/tfYpct` DocBlock fields with inline-beats-sheet
  (the whole property — a sheet translateY contributes nothing under an
  inline translateX), layout consumes through `tf_apply_items` at ten
  branch lanes (badge/heading/math/paragraph+quote rail/li/code/image/
  table/meter — separators + form widgets skip by policy). Rotate/scale/
  skew/matrix/transform-origin = consumed census vocabulary (2-D
  compositor non-feature). (3) **Dark-gradient hero polarity**: the
  luminance scale now reads GRADIENT backgrounds too — stop-average
  (`css_gradient_luma`) on the inline routes and the sheet route through
  apply_prop's background-color arm (shorthand's first-stop token fans
  out) — dark gradient heroes VISIBLE again (color #000 on #050505).
  Probe-found bug receipts (each caught by a probe run, fixed, and
  re-run to green): ① the fresh parser never consumed its unit letters
  (trailing-junk check voided EVERY token — rem gaps/margins read zero),
  ② per-mille was ×1000 (100× over), ③ translateY had no write path at
  all, ④ translate()'s first arg double-wrote y. **Probe v9 PASS
  (0 fails)** — 11 new rows: tf field battery ×5 exact (6 / -6 / +5,-25 /
  2 rotate-skipped / 3 inline-beats-sheet), VMINHIT absent (120vmin MQ
  consumed), GRADHERO invert=1, MRG2REM top=16 (2rem), MRG50PCT indent=0
  (% refuses); tally byte-stable `sib=0 attrOp=0 pcls=0 medF=1 var=4
  calc=2 prop=2` (all new decls ride KNOWN vocabulary — transform/
  transform-origin joined known_prop); all P1–P8 canaries byte-stable
  (FGAPC `gap=4` exact after the reader fix). **Gates**: tap1
  `inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1` byte-identical; HN
  11/225 fresh==rewalk census all-zero 0 ERRORs; Bryan 48/18 byte-
  identical census; Wikipedia 324/310 EXACT + census EXACT
  `pcls=0 medF=0 var=5 calc=0 prop=17` (the vector sheet's counted
  vocabulary untouched; live srcset 250→500px picks standing), 0 ERROR
  rows. Scorecard 92→94. Plain production restored (0 errors; warnings =
  the documented 3× baseUrl + bundler/xs set; boot green, 41KB idle
  stable across heartbeats, refusals=0); sims killed after every run
  (count verified 0); device untouched; git user-locked. Residuals:
  translate skips form widgets/separators by policy; sheet-route
  gradient polarity keys the FIRST stop (a light-first-stop gradient on
  a dark average leans on the inline route's full-value scan); exotic
  units (Q, dvw-family) refuse as unknown suffixes by design.

### 2026-10-19 — P10 transitionend/animationend immediate-fire CLOSED (94→95)
  The last phase of the HTML+CSS roadmap: the declarative animation and
  transition vocabulary now delivers its END EVENTS, wired across
  css.h/css.c (the `@keyframes` registry + `animation-name` value
  resolver), document.c (walk_anim_engage at every block-open lane with
  infinite/zero-count/paused guards, the hemorrhage-proof dedupe map on
  (node,name), the transition-hot gate reading class/id attrs),
  jsbridge (the mutation-notes ring noted at nine tree-mutation sites
  and the kind-4 end-event listener class — stored, and NEVER kind 0,
  which click dispatch matches without a type filter), and main.c (the
  post-render drain: animation lane zeroes its pending ring and
  dispatches; transition lane takes the notes, checks the gate,
  dispatches, and logs the gated lane rows — silent when nothing to
  drain, which is why Wikipedia shows no rows at all). The fixture
  suite caught the one real bug before any receipt shipped: fresh-parse
  walks wrote the dedupe map into the STACK DomResult while later
  renders map against the HEAP twin — a Walker field now carries the
  right instance per phase, and the double-dispatch died with the fix.
  **Probe v10 PASS (0 fails)**: the guard battery holds (registered
  `pf` engages; `nokey` unregistered; `paused` and `infinite` refuse;
  ANFALLDOWN/ANIMPAUSE/ANIMINF keep the field absent), direct rows
  `keyframes pf known + nokey absent`, `transition=color .3s`
  (resolved through the timeline list — the first typing used the
  query list and printed empty), `animationend lane: 1 run(s)
  dispatched`; tally byte-stable `sib=0 attrOp=0 pcls=0 medF=1 var=4
  calc=2 prop=2` (P9's baseline honored; the P10 vocabulary joins
  known_prop invisibly where its decls are in never-counted streams).
  **about:jsext suite 8/0** with the mounted-animation receipt:
  `PL10ANIMMOUNT1` from an ext-script-created element whose listener
  registered before the append (exactly-once; no `2` rows anywhere),
  `PL10TRANS1` from the classList-toggle timer inside a stored
  kind-4 listener (exactly-once). **Gates (all four), P10 binary**:
  tap1 `RESULT inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1`
  byte-identical (6 quiet `0 armed` lane rows on the React contact
  form); HN 11/225 fresh==rewalk, census all-zero, 0 ERROR rows;
  Bryan 48/18 byte-stable census with the lane rows logging
  `0 armed & dispatched` (mutations noted, no transition vehicle in
  the held streams); **Wikipedia 324/310 EXACT** fresh==rewalk==snap
  with the census byte-identical to P7/P9 (`pcls=0 medF=0 var=5
  calc=0 prop=17`, propnames row identical) — the session's own
  "17→13" census prediction was retired by the receipt: the trimmed
  sheet's animation/transition declarations sit in streams that never
  count, so the strongest form of the vocabulary join = ZERO
  perturbation on the heaviest real page. The one live-run oddity
  (links=309 once, same binary) was closed by experiment rather than
  assumption: the article is unedited since 2026-10-13, two
  back-to-back curls are byte-identical, and the `FILE_URL` twin runs
  on a byte-fixed upstream snapshot render 324/310 BYTE-IDENTICAL
  TWICE — engine deterministic on fixed input; the wiggle was
  served-side chrome, and `render-error(discarded)` = the
  pre-existing jsext-prefetch abort path inside the documented
  revisit-noise contract (never previously logged because earlier
  gate greps never asked for it). Scorecard 94→95. Sims killed after
  every run (count verified 0); device untouched; git user-locked.
  Residuals (documented): static inline victims complete on the
  pre-script fresh walk (the R54 pump defers scripts past the first
  render — the browser-faithful race; the mounted scripted animation
  proves the delivery path), keyframes bodies stay skipped (names
  registry only — no per-frame interpolation on a 1-bit flat panel),
  transitions need a walk-visible attr gate (JS-set styles are
  house-registered separately), and partially-filled class names
  score as full-name matches by the resolver's registry-first policy.

### 2026-10-19 — API-P3 the page's own address CLOSED (68→74)
  The engine's address book: the bridge journal (`histUrl[8][260]`,
  doc-base entry 0 seeded lazily) written by real
  pushState/replaceState (same-origin head guard, browser-faithful
  fragment/query/path/opaque-about resolution in the new
  `jsbridge_hist_resolve_url` — the resolver taught url_parse's about:
  branch its query/hash compartments, network pages uninvolved),
  read by NINE live location getters (the bridge-time snapshot gone),
  walked by back/forward/go (clamped), and latched into
  popstate/hashchange deliveries that run at the next engine-pump
  frame through the metered xsTry listener call. pushState is
  STRUCTURALLY wire-free: no http_* call, no pendingNav, no app-bar
  change — the router layer is uninvolved by construction.
  document.title's setter went browser-faithful (dom_set_text replaces
  the title node's text; the old append glued 'API Probe'+'Pluto J3').
  Probe receipt `apihist: pass=7 miss=0` ×N rewarks (apisum 11/3 and
  apifetch 4/0 byte-identical alongside); engine rows banked in
  `/tmp/api_p3_probe.txt`. Gates: jsext 8/0, tap1 byte-identical, HN
  11/225 ×2 (#79/#166), Bryan 48/18 ×5, Wikipedia twin
  census/propnames/324-310/bytes=198815 byte-exact, ev-test PASS
  (sameDoc, counters 1 each). Sim-host observation kept on the record:
  a Bryan-revisit-boundary timer aborts on the memory envelope and the
  engine resets (chronic ×2 runs, same frame; self-healed ×5 metrics
  before any journal/delivery code could be involved). Probe dump cap
  24→40 in the sim-only seam. Production build restored + boot-verified;
  sims killed after every run (11 runs, count verified 0); device
  untouched; git user-locked.

### 2026-10-19 — API-P4 console.log/warn/error CLOSED (74→76)
  The console surface became the real log lane on the NR engine:
  log/info/debug/warn/error each tag their own `[console.*]` row in
  the file (the receipt surface, device truth; the rendered panel
  never changes from a console call — the probe's consoclean row
  asserts that honestly: `out.children.length` delta ZERO across the
  whole console storm), printf substitution rides the first string
  arg (%s/%d/%i/%f to String, %o/%O/%j the console shape, %c consumes
  silently, %% literal, an out-of-args token prints literally like
  Chrome, extras append space-separated), objects/arrays render the
  console shape (top-level strings bare, nested quoted, arrays
  `[a, b, …n]` with 4 shown, objects `{k: v, …}` with 6
  own-enumerable keys from the engine's own iterator, `[Function]`/
  `[Object]` labels, depth 2 with bounded bytes, cycle-safe by depth
  construction), and console.assert owns browser semantics: truthy =
  SILENT (no row), falsey = an ERROR row ('Assertion failed: …'
  through the same substitution lane), never throws. info/debug/dir
  are real methods now; count, the group family, time and trace stay
  deliberately absent (documented boundary — an unknown method stays
  unknown, no fake surface). Engine-carried notes: array elements
  read via xsGetIndex and object values via xsGetAt(keySlot) —
  fxNewNameC interns plain names, so string-built "0" ids miss the
  index flavor (caught by the fixture: `[7, 8, 9]` rendered
  `[undefined, undefined, undefined]` in v1; exact after the fix);
  the shared xs_console_format_into owns its trailing NUL (an
  off-by-one memcpy'd prefix swallowed the first assert receipt —
  caught and fixed in-line by the same probe run); line budget 320
  vs the logger's 512 row cap. Probe "Console receipts" battery
  `apicon: pass=8 miss=0 sent=17` — consolog/consowarn/consoerr/
  consoinfo/consodebug/consolong/consoassert/consoclean all PASS;
  the log-side counts MATCH: 16 `[console.*]` rows + `P4COUNT 17` +
  the documented silent truthy assert (`P4NOASSERT` = 0 rows in
  every run). Gates: jsext 8/0 with its `[jsext-t]` markers now
  travelling the tagged lane (byte-identical verdict); tap1
  byte-identical (`inPlace=yes jsErrors=0 blocks=23 errCard=1`); HN
  blocks=11 ×2 (links 226 vs the standing 225 = one extra link in
  today's live front-page DOM — fresh==rewalk byte-consistently ×4;
  site drift documented, engine shape unchanged); Bryan 48/18 ×5
  (+1) with the documented chronic revisit-timer memory abort
  reproduced as sim-host noise (self-heal at the next fresh render);
  wiki byte-exact (census var=5 calc=0 prop=17, 324/310, snap
  bytes=198815, the standing revisit-missed noise row present);
  ev-test PASS (sameDoc, counters 1 each; block numbers grew to
  46/48/52 as the probe's canvas grew — the fence counts counters).
  Probe dump cap 40→56 in the sim seam (the 52-block canvas fits).
  Receipts banked: /tmp/api_p4_{probe,jsext,tap1,hn,bryan,bryan2,
  wiki,evtest}.txt. Production build restored + boot-verified; sims
  killed after every run (11 runs, count verified 0); device
  untouched; git user-locked.
- 2026-10-20 — **API-P5 ES modules closed** (~76→82). The loader-side
  module lane: both scanners flag `<script type="module">` (the shared
  type-is-module rule walks the type attribute — data filtered first so
  the proven data filter stays byte-identical), the router transforms
  import/export in C before any engine sees the source (IIFE module
  scope, import-first graph on the per-document ledger with cycle/
  depth/cap receipts, `globalThis.__p5mod` namespaces, named/default/
  star import forms, static export kinds materialized post-body);
  imports resolve module-relatively through url_resolve (the journal's
  resolver — the opaque-about refusal is the SAME rule browsers apply)
  and fetch on the page's own roads: about: = the internal store, file:
  = the Data-dir loader rule + the flat file-road join. Probe: `apimod:
  kids=3 refuse=PASS`; the `./p5x.js` import throws its honest load
  error (MODX-LEAK absent). Relative bar: file://modpage.html →
  `./p5rel.js` → `./p5rel-child.js` (nested, module-relative base),
  children import-first, `modpage: pass=2 miss=0 kids=2`. jsext 8/0;
  tap1/HN/Bryan/wiki/evtest byte-identical (Bryan's chronic abort
  self-heals as documented; evtest +4 blocks = the module battery).
  Production build restored + boot-verified (heartbeats, heap clean).
  Receipts: /tmp/api_p5_{probe,modpage,jsext,tap1,hn,bryan,wiki,
  evtest,prodboot}.txt — 10 sim runs this phase, every process killed
  and count-verified 0; device untouched; git user-locked.
- 2026-10-20 — **API-P6 DOM conveniences closed** (~82→88).
  `template.content` = browser shape (builder capture: the template
  element stays reachable while its children parse into the detached
  `#template-content` fragment — nodeType 11, parent-NULL/inert; empty
  fragment on first access for created templates; free-pass visitor
  frees owned detached roots, also killing the per-document shadow-root
  leak). `el.closest()` rides the shared `css_parse_selector` +
  `qs_matches` machinery on every engine; `el.dataset` snapshot getter
  added to duktape + mujs; append family returns the touched node per
  MDN. Probe battery `apidom: pass=6 miss=0` + `apisum: pass=12 miss=2`
  (the P0 `template` row flips PASS). The phase's real repair: the
  adapters' O(width) getElementById walks aborted whole at 65+ element
  children (the probe shipped exactly that — mid-page ids went null);
  one shared O(depth) `dom_find_by_id` now serves all engines, document
  order, width-proof (dom.c paged-road helpers got the same reframe).
  Cross-engine smokes (PLUTO_NR_AUTOTEST_ENGINE seam): mujs 4/6, duktape
  4/6 — feature rows all PASS; the two misses are the pre-existing
  fresh-wrapper `!==` identity shape (documented; XS NR keeps identity
  via its cache; duktape's nodeType gained the fragment rule in the
  smoke). Seven gates re-run + banked on the final source: tap1
  `inPlace=yes jsErrors=0 blocks=23 errCard=1`, jsext 8/0, HN `10/229` ×2
  (upstream drift verified: one `s.gif` spacer in today's HTML, not two),
  Bryan `48/18` (+ chronic abort self-heal), wiki `var=5 calc=0 prop=17`
  / `324/310` / `bytes=198815`, evtest `sameDoc` all 1 with `blocks=65
  (was 61)`. Production restored + boot-verified (fps 50/60 heartbeats,
  heap 43KB, no autotest defines). Parked gaps documented: className
  accessor, wrapper identity (mujs/duk), storage = Phase 7 lane.
  Receipts: /tmp/api_p6_final_{probe,tap,jsext,hn,bryan,wiki,evtest,
  prodboot}.txt + /tmp/api_p6_smoke_{mujs,duk}.txt — 33 sim launches
  this phase (gate/rerun/smoke + diagnostic iterations), every process
  killed and count-verified 0; device untouched; git user-locked.
  Operational lesson banked for every future phase: make tracks source
  mtimes, NOT define flags — SIMDEFS changes are inert until forced
  (touch all sources + `rm -f build/pdex.dylib` before every gate
  build); the build log's clang line is the only trustworthy receipt
  that a define landed.
- **2026-10-20 (Phase 7 CLOSED)** — General-purpose localStorage +
  sessionStorage (API-P7). Shared road `Source/html/webstorage.{h,c}`
  + per-engine adapters; origin rule = scheme at the first colon
  (`http(s)://authority`, one `file://` bucket, per-URL `about:`,
  opaque `"null"` fail-store, 8 origins resident/run → overflow
  fail-store with disk sections intact); caps 56/192/16-entries/4KB
  quota, refusals throw `QuotaExceededError` and store nothing;
  persistence via `webstorage_js.txt` `@origin` sections in the game
  Data sandbox (image rebuilt + re-adopted per save, cleared sections
  never resurrect); sessionStorage = per-run memory. Probe `storage:
  PASS` across XS/mujs/duktape with the LIVE quota receipt
  (`want=5134 have=6 cap=4096` + `[p7q] QuotaExceededError` console
  row), `apisum` 13/1 (only `worker` MISS), standing cross-boot
  counter `visits` 1→11 across 11 probe boots (boot 2 = the
  persistence gold receipt: `entries=1` loaded, `p7v 2` written).
  Self-caught in-phase receipts: `://`-only origin parse bug (about:
  fell to "null"), newline-suffixed section-header match never firing
  (strict line equality now), and mujs's silent abort on `new
  Array(5001).join()` (replaced with concat-doubling). Gates:
  tap1/jsext/HN/Bryan/wiki banked (HN `11/225` = era drift), evtest
  re-run byte-identical `blocks=65 (was 61)`. Production restored +
  boot-verified (6 heartbeats, zero autotest defines, zero webstorage
  rows on home). Receipts: /tmp/api_p7_probe_final.txt +
  /tmp/api_p7_probe_persist.txt + /tmp/api_p7_{tap,jsext,hn,bryan,
  wiki}.txt + /tmp/api_p7_evtest_final.txt +
  /tmp/api_p7_smoke_{mujs,duk}_final.txt + /tmp/api_p7_prod.txt.
  Score 88→92. Device untouched; git user-locked.
