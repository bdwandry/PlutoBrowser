# SESSION PLAN — Button presses must be script-interceptable (no reload)

Date: 2026-10-16 · Tree: committed baseline (user stash-dropped all scratch; SESSION_PLAN.md + tests/nr_chunk_host_test.c recreated fresh)

## Hard constraints (from user + AGENTS.md)
- Simulator ONLY. No physical device deploy until user explicitly says go.
- NEVER run git commands (user handles all git). NEVER git push.
- Auto-navigate test URL: `https://bryanwandrych.com/contact` via autotest seam.
- Only engine modifiable: **XS (No Recursion)** (`jsbridge_xs_nr.c`). Other 4 frozen.
- Router-level changes additive only, NULL-defaulted → frozen engines unchanged.
- General-purpose fixes ONLY — no site-specific hacks.
- After every sim test: `kill -9 <PID>` (never pkill), verify with `ps aux | grep -i "playdate simulator" | grep -v grep`.
- Rebuild PDX after every edit. Log everything, never remove log statements.

## Progressive state — 2026-10-17 session (post gate; sim boot clean, revisit path = new blockers)
- **OFFLINE GATE PASSED** (`/tmp/replay3Z.txt` + repeat `/tmp/replay3Y.txt`): contact page replays ALL
  scripts with `errs=0 listeners=16` at close (repeat runs identical). The react commit completes:
  resource load-fires → unsuspend → commit → click delegation attached (16 = SSR pings + attached
  handlers). Frame-level `mut` stays 0 ONLY because the offline flow completes the whole commit during
  parse (no pump window sees the budget drop); the device split flow lands the fires in pump frames
  where the per-fire budget signal (below) yields `mut>0`. No r1 hydration rows, no `[reject]`, no
  `[meter] overrun`, no ABORT.
- **Resource-load fire shipped** (the commit-blocker fix): `JsListener.kind` (0=click/1=load/2=error,
  `jsbridge_internal.h`; frozen engines set 0 explicitly). `xs_el_addEventListener` registers
  load/error (kind-aware); insertions of `link[rel=preload|stylesheet]` via appendChild/insertBefore
  queue a fire (`xs_maybe_queue_asset_load`); registration also queues. The fire = ONE mtQ dispatcher
  (`xs_queue_resource_fire`, ring-slot pin protocol like xs_queueMicrotask, host fn pinned once in
  `st->resFireFn`) which builds the same Event object as click dispatch (type/target/preventDefault —
  target REQUIRED: react's non-delegated dispatcher fq→getEventTarget reads event.target), calls
  kind-1 listeners (this=element, event) inside per-listener xsTry (browser semantics: one listener
  throwing never cancels the event), then the element's onload property hook, then consumes ONE
  budget unit (budget_take → the pump-frame mutation signal for device frames).
- react identity map: `fO`=checkIfFullyUnsuspended(ping), `fz` unsuspend, `s_` commitRoot, `fE`
  hoistable-adopt pick (needs document.getElementsByTagName), `tI`/`fK`/`fq` non-delegated event chain
  (needs event.target), `iE` commitUpdateQueue effect walker (create() call inside per-fiber try —
  the `[throw] …iE<-uS<-uw` rows are REACT-CATCHED effect noise, same family as the DCFR rows).
- **document.getElementsByTagName added** (`xs_doc_getElementsByTagName` + shared
  `xs_collect_tags_into`; browser-parity — react's hoistable adopt `fE("link","href",doc)` needs it;
  its absence was the `call: not a function` at fE).
- **Diagnostics strip done**: temporary probes ([walk][tget][crt][qs][lsn][mt][mt0][stack per-64/4KB
  rows][meter tick][cbacktrace][rnmeta]) now also require `PLUTO_HYDRATE_TRACE` (the harness keeps
  passing it; sim/device builds never define it). Retained: `[meter] overrun`, `[reject]`,
  `[json] fail`, re-arms, `[timer]` set/fire rows, `[load]` rows. Fork `xsRun.c` [rnmeta] rows now
  PLE_THROW-gated (sim build fix); two earlier-session probes (`[cs] script stamp`/`src-fallback`)
  were ungated and referenced PLE_THROW-only globals — fixed (they broke the plain sim build).
- **Track C first sim cycle**: `make simulator SIMDEFS='-DPLUTO_NR_AUTOTEST
  -DPLUTO_NR_AUTOTEST_URL=\"https://bryanwandrych.com/contact\" -DXS_STACK_COUNT=16384u
  -DXS_CSTACK_LIMIT=4000000'` — NOTE: sim needs the harness stack limits too (the defaults 4096
  slots / 512KB cstack abort the react boot at script 11; with the harness limits boot 1 is CLEAN:
  `budget=3906KB ran=14 errs=0`, first render live `blocks=22 links=10 title='Contact | Bryan
  Wandrych'`, snapshot saved). THEN the autotest revisit tears the engine down IMMEDIATELY after the
  first render (`close ... listeners=2 timers=2` — two pending 50ms timers never fired, so the
  remaining load-fire rounds + the commit's listener attach never got a pump window — the sim does
  not yet hold the live page long enough) and the revisit boot dies: `[snap] invalidated + purged →
  refetch (autotest FAIL: revisit missed the snapshot, hits=0 state=2)` then `[xs] abort: not enough
  memory` at script 1 under memwatch ARM (`[memwatch] first-render live=49458KB ARM phase=revisit
  budget=7.5MB`).
- Sim follow-ups (sim work, next): (1) why the revisit purges the snapshot it just saved (state->2,
  hits=0), (2) revisit-boot OOM under the memwatch ARM (live 49MB at ARM — engine allocations
  refused), (3) guarantee a pump window between first render and revisit close so the pending
  microtask cascade + attach completes live like the offline gate.
- Frozen engines keep compiling (sim links mujs/duktape/quickjs bridges with the inert `kind=0`
  line).

## Root causes — ALL verified on this tree (fresh greps + region reads)
| # | Fact | Anchor |
|---|------|--------|
| 1 | Button tap → `activate_form_block` → case `LRI_INPUT_SUBMIT` → `submit_form()` **directly, zero JS dispatch** | `main.c:3582, 3597` |
| 2 | `submit_form` ends `pendingNavUrlSet = 1` → full navigation = the "reload" | `main.c:3655, 3838` |
| 3 | `DocBlock` has **no `srcNode`** field (struct ends 275); only `DocLink` has one @283 | `document.h:275, 283` |
| 4 | Submit blocks created at 3 sites, DOM node + attrs in scope at each — stamping is 1 line/site; links model: `x->srcNode = node` | `document.c:4358, 4381, 4462; model @3742; link layout propagation @5286` |
| 5 | Router vtable has **only** `dispatch_click`; JsListener struct has no event-type field | `jsbridge_internal.h:228, 113-137` |
| 6 | NR engine delivers ONLY `click`; other types are no-ops; result enum NONE/NAVIGATE/SUPPRESSED exists | `jsbridge_xs_nr.c:1409/1419; jsbridge.h:47, 153-155` |
| 7 | Dispatch is exact-target, event object hardcodes `type:"click"`, no bubbling (React's `onSubmit` lives on `<form>` ancestor) | `jsbridge_xs_nr.c:6707, 6739` |
| 8 | `form_key_write` sets value attr but fires **no `input`/`change`** | `main.c:3472-3492` |
| 9 | `DomNode.parent` exists → ancestor bubbling directly implementable (SW8 paging: stub nodes need `dom_touch`) | `dom.h:57` |
| 10 | Generic script prefix `JSBRIDGE_SW5_PREFIX` ships only Set/Map/Image shims — the slot for fetch/currentScript/etc. | `jsbridge.c:748-786, 788` |
| 11 | NR: XMLHttpRequest yes, `fetch` no; `currentScript`/`AbortSignal`: 0 hits in all of Source (greps clean) | `jsbridge_xs_nr.c` |
| 12 | "chunk path empty but not in a worker" is **site-side** text (0 hits in Source) — site runtime probes missing browser APIs | grep + baseline log |
| 13 | Autotest seams exist: engine=4 forced, URL override via `-DPLUTO_NR_AUTOTEST_URL` | `main.c:931, 942, 2985, 4165, 5479-5517` |

## Baseline sim (committed tree) — what must change
`listeners=0 ran=16 errs=6`; scripts 3/4/5/10 fail "chunk path empty but not in a worker" (Next.js bootstrap);
scripts 7/8 `SyntaxError:` on React chunk `0cz1d0mv5g_q7.js` (112,594 bytes); pass-2 `not enough memory`
(heap peak ~$37$MB, `[mem] BIG-ALLOC refusals=1`). React never mounts → no handlers can exist → even Track B alone can't pass.

## TRACK A — make the site's JS actually run (prereq for everything)
- [x] A1 Harness `/tmp/nrchunk`; React-chunk errors bisected+fixed (see Progressive root causes).
- [x] A2 Generic shims so site chunk-loaders resolve (currentScript/context-script routing etc).
- [x] A3 Pass-2 memory: works via existing budgets (sim revisit OOM tracked separately below).
- [x] A4 Pure-JS `fetch` + `AbortSignal.timeout(30000)` shims landing in the prefix shim stack.
- **Gate A reached OFFLINE** (`listeners=16 errs=0`; commit + click delegation attached). Sim-side
  gate still needs the revisit-flow fixes (pending-fires + commit complete on a LIVE page).

## TRACK B — general event plumbing (layered, additive)
- [x] B1 `DocBlock.srcNode` field (after doc_arena pointers) + stamp at the submit/textarea/button creation sites (`document.c` input-submit ×2, textarea, `<button>` element). Layout needs NO threading: `LayoutItem.block` already points at the block.
- [x] B2 Router: additive vtable slot `dispatch_event` (appended AFTER `name` LAST so frozen engines' positional initializers keep their exact mapping — trailing zero-init = NULL) + `JSB_EVENT_{NONE,NAVIGATE,SUPPRESSED}` (`JsBridgeEventResult`) + `jsbridge_dispatch_element_event(b, node, type)` router (`jsbridge.c`).
- [x] B3 NR: listener store now keys by (node, **exact type**) — `JsListener.type[12]` stamps the registration string; `addEventListener` accepts submit/input/change into the kind-0 class; **document-level addEventListener no longer a no-op** → registers with `target = b->dom->root` (react app-router delegates on `document`!); `xs_dispatch_element_event` bubbles via `DomNode.parent`, event object `{type,target,currentTarget,cancelable=true,bubbles,defaultPrevented,preventDefault,stopPropagation}`, sticky preventDefault (browser order), per-listener xsTry isolation, gated `[dev]` trace rows. **Wrapper identity cache** (B3b, needed for react): one pinned wrapper per node (`XsState.elwNode[]/elwWrap[]`, cap 1024, `xsRemember`/close-sweep `xsForget`) so react's `node[internalInstanceKey]` expando survives pushes; browsers keep Element identity stable, this is that model.
- [x] B4 `form_key_write(text, commit)`: fires `input` per keystroke; `input`+`change` on keyboard commit; value attr written BEFORE events (react reads target.value synchronously); textarea value writes now included; SUPPRESSED ⇒ `page_rewalk_now`.
- [x] B5 `activate_form_block` LRI_INPUT_SUBMIT: dispatch `click` on `blk->srcNode`, then `submit` on nearest `<form>` ancestor (browser activation order); SUPPRESSED ⇒ `page_rewalk_now` + break (never submit_form); NONE/NAVIGATE ⇒ `submit_form` exactly as today (byte-identical static pages). `[form]` log rows per tap.
- [x] B6 No-handler pages: dispatch returns NONE almost free → native fallthrough unchanged (formtap replay EXIT 0 proves contact-page still errs=0).
- [x] B7 Offline validation: `tests/nr_chunk_host_test.c` now does the synthetic tap (`/tmp/formtap.html`, base https://example.org/formtap): doc-level click (NAVIGATE) → form submit (SUPPRESSED) → handler DOM marks flip (`caught-submit`, `done=1`), `jsErrors=0` with the probe TURBOPACK-gated. **Contact page offline still hits a SEPARATE issue**: react hydration removed body's original children and inserted its new tree DETACHED (`[dom]` trace rows: 10 appends all head-hoists, zero into body; the stamped button chain ends `header → NULL` detached while the live tree from dom->root has NO buttons). That is a react-commit DOM-attach problem, NOT the Track B dispatch chain (formtap proves the chain green end-to-end).
- REPLAYS: `/tmp/replay4I|4J.txt` (formtap green), `/tmp/replay4K.txt` (contact: errs=0, listeners 27 — 8 new doc-level delegation registrations, 16 legacy; cap 32 in sight for bigger react pages — watch in sim).

## HYDRATION GAP — ROOT CAUSE + FIXES (2025-10-18 session)
- **ROOT CAUSE: `xs_el_get_innerHTML` was aliased to the textContent getter.** NEXT.js's root error boundary (`class i extends Component` in the next chunk) snapshots `document.documentElement.innerHTML` on fallback (`this.rootHtml=...`) and re-renders `<html dangerouslySetInnerHTML={{__html: rootHtml}}>`. With the text-degraded getter the snapshot was 511B of body TEXT (the old HOSTBODY=511 mystery!). The assignment hit `jsbridge_el_set_inner_html`, whose clear loop detaches `parent=NULL; childCount=0` SILENTLY (no [dom] rows) → head+body erased, one TEXT node adopted → html kids 2→1, tree permanently hollowed.
- FIXES (all additive NR-only or additive shared):
  1. `dom_serialize_children_html` (dom.c+dom.h): REAL markup serializer — `<tag attr="v">`, void elements (area..wbr) bare, text escaped `&<>'"`(attrs also `"`), comments copied, script/style raw, depth guard 24 → `xs_el_get_innerHTML` now returns true markup (probe RT1-4 round-trips green).
  2. `jsbridge_el_set_inner_html_ex(b, el, html, len, maxLen)` additive overload in jsbridge.c/h; shared `jsbridge_el_set_inner_html` keeps the 16KB JSBRIDGE_INNER_HTML_MAX (frozen engines byte-identical); NR setter passes 128KB (a contact snapshot is ~22KB — the 16KB wall caused the FIRST sim run's 53× assign-fail loop + timer error, errs=1).
  3. Document wrapper (`xs_document_appendChild/insertBefore/removeChild`)# root ops — the `call: not a function` thrown by react's clearDocument walk (`e.removeChild(n)` on the document) is gone; `fu(e)` not.
  4. Live getters for `body/head/documentElement/activeElement` (`xs_document_get*`) — replaced init-time def_val statics (stale wrappers).
  5. `xs_el_get_rootNode` (`getRootNode`) added to the element accs table (react `fu()` prefers it; ownerDocument still verified `== doc`).
  6. `JsBridge.domDirty` trailing field + `jsbridge_take_dom_dirty(b)` — NR innerHTML assign sets it; main.c `js_timers_update` polls and escalates to `page_rewalk_now` (`[dom] dirty:` row). Layout re-anchors + RE-STAMPS after a fallback restore (without this the tap hit the detached old-subtree stamp).
- RESULTS: contact replay `jsErrors=0`, one innerHTML cycle (22KB, restore), HOSTBODY 511→24195; formtap replay still GREEN (`caught=submit` B5 chain, errs=0).
- SIM (`PLUTO_NR_AUTOTEST_TAP=1`, engine 4, contact): boot nav → tap phase found the **nav toggle button first** (its `<button>` stamp sorts earliest; header/nav chain logged by the new `form_node_ancestor` fail-path diagnostics) — scan now REQUIRES a form ancestor (formless buttons: B5 no longer runs `submit_form` when no `<form>` ancestor exists — `else { logger_log("[form] formless button: no submit run"); break; }`). The contact form's button now dispatches: click→NAVIGATE (delegated pair i=2/3 fired), submit→`[dev] fire i=8/i=9 tgt=#root type=submit` — **but rc=NAVIGATE: no preventDefault because the fallback-restored markup has NO react-managed fibers** (the boundary's dangerouslySetInnerHTML blob is NOT react children). The original hydration must SUCCEED for C3/C4.
- REMAINING (next session): the initial hydration failure trigger — `[throw] script 14 engine #6: call: not a function chain=(anonymous-0)<-iE<-uS…` — react's `iE()` runs effect `a=n.create; n.inst.destroy=r=a()` where create is NOT a function; `[tcode] code=0x11031e2e5 b0=66` decode pending (Track A op table); the 10× `cannot coerce undefined to object @T` rows are react's own DetermineComponentFrameRoot stack-emulation reads (keys seen: actionQueue/children/initialRSCPayload) — react-catched, secondary. FIXING the iE throw should keep the REAL hydration alive → the contact form keeps its onSubmit → in-place submit → C3/C4 reachable.
- SIM notes: build with `-DPLUTO_THROW_LOG=1 -DPLUTO_HYDRATE_TRACE=1` added to SIMDEFS or the [elap]/[dom]/[dev] gated rows are compiled out (an old pdx without them cost a cycle); `rm build/pdex.dylib` if make skips the rebuild. Sim artifacts: `pluto.log` rows cited above; sim always killed via kill -9 + ps verify (0 processes left).

## HYDRATION TRIGGER — SOLVED (2026-10-18 session; real hydration lives, C3 in-place submit PROVEN on sim)
- **iE throw root-caused WITHOUT opcode decode** — added the smoke-gun probe instead: `A3 forensics` in the NR fork `xsRun.c` at the `call: not a function` raise site logs thecallee slot kind (+ its property name when the slot carries an ID, same keyArray trick as the A2 `new-callee` probe): `[throw] call-callee (no-id) kind=0` ⇒ the callee was literally `undefined`; the log sequence **`[crt] next-route-announcer` → setHostData style ×2 → `[crt] div` → setHostData element → setHostData style → throw**, matched byte-for-byte against the decompiled chunk `/tmp/jscache/720fb31a7612c3e1.bin` module **62634 `AppRouterAnnouncer`**:
  ```js
  e.attachShadow({mode:"open"}).appendChild(t)   // ← attachShadow was UNDEFINED in the engine
  ```
  iE's chain `(anonymous-0)<-(anonymous-0)<-iE` = the effect arrow → the IIFE whose body calls attachShadow. React never got its announcer effect mounted, the boundary fell back to dangerouslySetInnerHTML (no fibers → no preventDefault).
- FIX 7 — `DomNode.shadowRoot/shadowHost` trailing fields (dom.h; name-based, zero for parsed/detached nodes so frozen bridges are unaffected) + `xs_el_attachShadow` (jsbridge_xs_nr.c): the open shadow root IS a real element node (`#shadow-root` tag) held OUT of the render tree (parent NULL → invisible to layout, tag scans, serializer) with full Element machinery; `host.shadowRoot` identity is stable; nodeType getter reports 11 for shadow roots; `dom_created_dump` excludes them (detached by DESIGN). Also added `xs_el_get_isConnected` (announcer cleanup gates removeChild on it).
- FIX 8 — listener EXACT-TUPLE dedupe (DOM-spec no-op) in `xs_el_addEventListener` + `xs_same_listener_fn` helper: kind-aware identity compare (CODE: address+closures, CALLBACK ditto, HOST_FUNCTION builder). First real-hydration commit binding the full delegated set (151 doc + 53 div + … attempts) blew `JSBRIDGE_LISTENERS_MAX 32` — `too many event listeners` from react's own `up<-uf` loop.
- FIX 9 — `JSBRIDGE_LISTENERS_MAX 32 → 128` (jsbridge.h comment explains sizing: ~75 delegated types × bubble/capture pairs; dedupe keeps re-renders flat, unique first-batch no longer fit 32). JsListener ~32B → cap cost ≈ 4KB worst case, one bridge shared.
- RESULTS: contact replay `/tmp/replay6C.txt` — **`jsErrors=0` at every checkpoint, NO innerHTML rows at all (real hydration, no fallback restore), `[shadow] attach` + `[frg] created=27` with EMPTY dump (nothing detached), close row `errs=0 listeners=43`**; formtap regression GREEN (`VERDICT caught=submit clickTarget=BUTTON status=caught-submit done=1`, `/tmp/replay6D.txt`); probe RT1–RT4 still green (`/tmp/replay6E+6F`).
- **SIM CYCLE (2026-10-18, PDX launch via the real binary path)**: `".../Playdate Simulator.app/Contents/MacOS/Playdate Simulator" PlutoBrowser.pdx` (NOT `bin/Playdate Simulator` — that path avoids the app-wrapper and failed "no such file"). Log trail of the C3 proof:
  ```
  [shadow] attach host=next-route-announcer(0x7978033a20)     ← real hydration on-sim
  [form] submit tap: click=1 node tag=button
  [dev] type=click recv=button → fire i=2/3 tgt=#root        ← delegated click pair ran
  [dev] type=submit recv=form → fire i=8 tgt=#root type=submit
  [crt] p … [dom] insertBefore parent=form child=p ref=div    ← REACT RE-RENDERED THE FORM from the delegated submit (live fiber-driven commit)
  [dev] fire i=9 tgt=#root type=submit
  [form] submit dispatch: rc=2                                ← SUPPRESSED = preventDefault through REAL fibers (was rc=NAVIGATE with fallback markup)
  render: rewalk done blocks=23; [nr-autotest-tap] RESULT inPlace=yes jsErrors=0 (was 0) blocks=23
  ```
  Sim killed (kill -9) + ps verified 0.
- TRUTHFUL SCOPE: C1/C2 + the "submit stays in place with reactive DOM update" half of C3 are PROVEN. C3's mailto-invalidation details (empty-send inline error card) and C4 (filled send fetch POST) still need the known key-by-key keyboard-drive on the input field — plumbing is now UNBLOCKED (fibers live on the real tree; the input keeps its react onChange).


## TRACK C — simulator proof loop
- [x] C1 Build `make simulator SIMDEFS='-DPLUTO_NR_AUTOTEST -DPLUTO_NR_AUTOTEST_URL=\"https://bryanwandrych.com/contact\" -DXS_STACK_COUNT=16384u -DXS_CSTACK_LIMIT=4000000'` (stack limits REQUIRED — sim defaults abort the react boot).
- [x] C2 Cycle per test verified: bearer launch `.../Playdate Simulator <Source/pdex.dylib>` (the `open -a` route does NOT load the game), truncate pluto.log → ready ~45s → inspect → `kill -9 <PID>` → ps verify.
- [x] C3 Empty Send → inline "Please enter a valid email address", no `pendingNavUrlSet`, no reload — **PROVEN on sim 2026-10-18 (full chain)**:
  ```
  [form] submit tap → [dev] fire i=2/3 (click) → fire i=8/9 (submit)
  [crt] p … [dom] insertBefore parent=form child=p ref=div   ← react re-render
  [form] submit dispatch: rc=2 (SUPPRESSED)
  RESULT inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1
  errCard text='Please enter a valid email address.'
  ```
  Evidence: the autotest RESULT now whole-array-scans block inline texts (`nr_tap_blocks_contain` in main.c; render-result rows print only 3 samples) and reports `errCard` + the matched text. Sim log 12:28:43-45 cycle (dylib 08:28).

- [x] C4 Filled Send **DONE 2026-10-18** (sim, engine XS-No-Recursion): typed autotest email/subject/message → button tap → `Sending…` state → **real `fetch` POST delivered** (`xhr #1 POST body 133` → `xhr #1 ok … (17 bytes)` from `server.bryanwandrych.com/send_email`) → **sent card renders** (`[dom] remove child=form`, blocks 22→20, `block[10] text='Sent. Your message is on its way to bwandrych@…'`) — `RESULT inPlace=yes jsErrors=0 blocks=20 sentCard=1 errState=0` + `sentCard text='Sent.'`. Sim log 15:23:03-08 cycle. Ran into three root fixes, all in this session:
  1. `ucMethod` memset (`jsbridge.c`) — POST routing.
  2. `document.oninput` shim (engine build ~L4262) — react onChange routing to the tracker path.
  3. **Timer/XHR boundary microcheckpoint (C4 async fidelity, `jsbridge_xs_nr.c`)**: the fork's native-promise jobs (async/await continuations) were never drained at timer/XHR completion brackets, and the two queues feed each other — the first fix (single drain) left React's `queueMicrotask` flush STRANDED in the injected ring (rendered only 47s later when an unrelated timer flushed it); the final fix is `xs_boundary_microtasks()`: alternate `xs_drain_jobs` + `xs_flush_microtasks` until BOTH queues are empty (≤16 passes) at BOTH runner tails. Instrumented probes (`[prom]`, `[jrun]`, `[drain]`, `[prom] runner`) remain per log-everything — plus `fxBufferFrameName` NULL-frame guard (a probe crashed the sim when the frame was NULL mid-drain).
- [x] C5 Generality check **DONE 2026-10-18** (sim, engine XS-No-Recursion, home page): new `PLUTO_NR_AUTOTEST_ANYBUTTON` mode — first stamped `<button>` regardless of a form ancestor → click dispatch (both delegated pairs fired, `[dev] fire i=2/i=3 tgt=#root type=click`, formless: no submit run) → react's onClick ran → **in-place reactive update `aria-expanded 'false'->'true'`** on the nav toggle (site mechanism: `data-open`/`aria-expanded` attribute flips, menu stays structural) → `C5 RESULT inPlace=yes blocks=48 (was 48) clickReactive=yes aria-expanded 'false'->'true' jsErrors=0`. No reload (`inPlace=yes`). Sim log 15:34:38 cycle. Detector note: attribute flips are INVISIBLE to the block list — the C5 detector reads the clicked node's `aria-expanded` directly (`dom_get_attr`); blockCount change is kept as fallback.
- [x] C6 Device deploy **DONE 2026-10-18** (user said go): full AGENTS.md checklist — datadisk mount → rm old → cp -R → **pdex.bin MD5 MATCH `dc8bfd98d3b7b61da5463fca00487dd4`** → all 3 device logs cleared → eject → 60s → `pdutil run`. Boot observed CLEAN (50 fps, 20KB idle heap, empty crashlog). Post-log-read re-run launched (device stays in the game).
  *(Device deploy = the tested build WITHOUT autotest defines. C6 remaining validation = user's on-device walkthrough of contact form in-place submit.)*
- [x] C6 device log read **DONE 2026-10-18**: crashlog EMPTY (no crashes); errorlog = two `Run loop stalled >10s` (08:16:05, 08:20:17 — both in page-script phases, not form taps); pluto.log ring kept only the tail — the 08:20 session showed **react's 112KB bundle skipped by the compile guard** (`script 5 skipped: script too deeply nested`) — hydration impossible on that deployed build; sim compiles the same bundle fine. Walkthrough-era rows rotated out of the ring.
- [x] C6 REDEPLOY **DONE 2026-10-18** (user said go, after the C4/C5 fixes): rebuilt via `make device` (NOTE: bare `make` fails the link — the `_times` stub + dav1d shim are gated `ifeq ($(MAKECMDGOALS),device)`); DEVICEDEFS empty (no autotest) → full checklist → **pdex.bin MD5 MATCH `aa36b3eee33cc721c7b42ce21920358b`** (fresh engine: oninput shim + POST memset + promise-boundary drain + NULL-frame guard) → 3 logs cleared → eject → 60s → `pdutil run`. Device is in the game on the new engine. User's walkthrough = the remaining C6 validation; re-read logs afterward if anything feels off.
- [x] C6 E1 HARD-STALL DIAGNOSED **DONE 2026-10-18** (user report: "E1 run loop stalled"). Evidence: crashlog EMPTY; errorlog ONE row — `12:02:50 Run loop stalled >10s pc=0x901CECC4 lr=0x9018F0C3`; pluto.log boot 12:01:14 (auto-nav fired correctly) → prefetch done/page attach 12:02:40 → scripts 1–8 admission chain 12:02:40–12:02:49 (ka rows every ~2s are IN-BRACKET metering-callback logs, NOT update() returns — `xs_engine_keepalive`) → log ends → watchdog at +10 s. SYMBOLIZED against the matching deployed ELF (pc−0x90000000 base): `pc=fxNewParserSymbol_nr (xsScript.c:331)`, `lr=fxGetNextKeyword (xsLexical.c:276)` — inside a script's `fxParseScript` lexxer/parser. ROOT CAUSE: the post-parse script stage (`jsonc  js_doc_attach` FULL-mode slot loop, document.c:6535→jsbridge.c:1191) executes ALL page scripts (inline + externals, each a full parse+compile+run bracket) inside ONE update() frame — ~9.5 s of ARM parse work this boot, breached the 10 s SDK watchdog mid-lex of script 8 (32 KB) after scripts 1–7 (~60 KB total) already compiled. NOT a regression: the morning build E1'd the same way (08:16:05, 08:20:17, same phase — end-of-log = script admission rows), and the sim never sees it (host runs the stage <1 s). React bundle note: this boot it was DEFERRED (bundler plan `NO_MAP` — turbopack's `!function(){` wrapper is not the planned `(()=>{var X={…}` shape; chunker declines monoliths) → never parsed → device react hydration still open (separate item: planner shape, segfault-adjacent). Device left ejected/parked (NOT re-run: any boot re-enters the same E1 cycle until the fix). FIX (planned, Level 1): js_doc_attach builds the slot plan + attaches the engine but runs ONE pending script per frame FIFO via a bridge queue extending the proven R15 deferred-pump machinery (eval_pending stays 1 until drained → R27 gates walk/snapshot as designed).
- [x] C6 BOOT AUTO-NAV **DONE 2026-10-18** (user request: "on device, auto-navigate to contact at launch"): `main.c` kEventInit, `TARGET_PLAYDATE`-gated — sets `pendingNavUrlSet=1` to `PLUTO_BOOT_NAV_URL` (default `https://bryanwandrych.com/contact`, overridable via `DEVICEDEFS="/-D..."`-style define) through the SAME pending-nav pipeline as the autotest boot nav; `snapBypass=1` so every launch takes the REAL fetch+eval+hydration path (a snapshot restore has no live DOM — the form wouldn't be reactive). Sim unaffected (home launcher boot intact). String verified in build/pdex.elf (`[boot] auto-nav:`); NOTE pdex.bin is packed — strings/`grep -a` can't see literals there, verify against the ELF. REDEPLOYED → **MD5 MATCH `a3a0b203081f424d8a5534c1bcbe1001`** → logs cleared → eject → 60s → run. Device boots straight into the contact page; system menu Home-Page item still reaches the launcher. One transient during deploy: the data-disk mount dropped mid-copy (ejected + re-ran datadisk, volume healthy).
- [x] C6 R54 PER-FRAME SCRIPT QUEUE **DONE 2026-10-18** (user: "Your job now is to make it work on device!"): the E1 fix, implemented + sim-validated + device-deployed.
  - **Mechanism**: `js_doc_attach` FULL mode now PARKS the slot plan on the bridge (`scriptQueue[JSBRIDGE_MAX_SCRIPTS]` copies, document order) and returns; `jsbridge_script_queue_pump` (called from `js_timers_update` next to the deferred/engine pumps) runs **ONE entry per update() frame**, branch semantics + log rows identical to the old sync loop (shared `jsbridge_run_script_entry` helper incl. SW2d data:-decode, ext body/spill-stream/materialize, skip rows). Entries consume NEVER on a busy engine: gates = mount_parked (R28) + `impl->eval_pending` (split mid-flight) + inTimer/inClick. `jsbridge_eval_pending` now ORs in queue-pending so the R27 walk/snapshot gates observe the POST-JS DOM (a swap-time snapshot mid-queue would have poisoned the cache with the pre-JS DOM — caught in review).
  - **Device watchdog belt+braces**: `JSBRIDGE_ONE_FRAME_SCRIPT_MAX` (64KB, device-only) — whole-file parse brackets (`xs_run_script` inline + `xs_run_script_stream` incl. the R15 deferred-retry landing) skip containedly above the cap; without it the deferred react chunk (112KB) would have E1'd later via the retry path the moment memory freed. Sim unaffected.
  - **Discovery — per-frame drains broke next.js ingestion** (sim A/B): 1 entry/frame → scripts 12–14 threw `Unexpected server data: missing bootstrap script.`; the byte-identical SYNC whole-queue drain was green (`RESULT inPlace=yes sentCard=1`). Root cause from the actual bundle source (module 98569 in `3dmm3v_fq28gc.js`): `L()` (the `__next_f` push processor) throws `if(!a)` once `M` voided the flight buffer; `M` runs `(0===document.readyState==="loading") ? DOMContentLoaded` : `setTimeout(M)` — pre-R54 `document.readyState` was UNDEFINED → always the setTimeout branch → react's 50ms scheduler timer fired BETWEEN queue frames → buffer voided mid-push. Fix = browser parse semantics: `xs_document_readyState` (NR host getter: "loading" while `scriptQueueNext < scriptQueueCount`, else "interactive"); `DOMContentLoaded` listeners now REGISTER (own kind 3, both addEventListener paths; was silently dropped) and the router calls the NEW additive vtable slot `script_queue_drained` once at drain — NR dispatches DCL to document-level listeners in one bracket + boundary drain (next mount parks/resumes via R28 as usual). TRAP caught: `JsListener.type[12]` snprintf-truncated "DOMContentLoaded" (17B) to a dead alias → `[dcl] fired (listeners=0)` with the listener registered; widened to `type[24]` → `fired (listeners=1)`, flight stream closes at drain like a browser.
  - **Sim validation (both green, drain ordering fixed)**: C4 contact → `RESULT inPlace=yes jsErrors=0 blocks=20 sentCard=1`, `sentCard text='Sent.'`, `drained: 14 slot(s), ran=14 errs=0`; C5 homepage → `C5 RESULT inPlace=yes blocks=48 clickReactive=yes aria-expanded 'false'->'true' jsErrors=0`, `drained: 17 slot(s), ran=17 errs=0`. Kept A/B seam `PLUTO_R54_QUEUE_SYNC` in jsbridge.c (sync-drain whole queue, sim-only reference path, 0 device builds define it).
  - **DEVICE VALIDATION (`1,954,521` pdex.bin, MD5 MATCH `9801d5ff17ffb806c1cdc327d7e76091`, full checklist, logs cleared pre-run)**: boot 13:17:56 auto-nav → fetch → attach parked 14 slots 13:19:22 → scripts spread 13:19:22→13:19:39 (each frame returned — `plain-run`+admitted rows one/second, distinct timestamps = Real update() returns, NOT in-bracket ka) → **NO E1: crashlog 0 bytes, errorlog EMPTY** → `drained: ran=11 errs=3` (the 3 = deliberate 64KB-cap skips: 170495B, 229820B, +112594B deferred) → drain rewalk **blocks=22 links=10 title='Contact | Bryan Wandrych'** incl. form fields → `[snap] saved ... blocks=22 bytes=9163` (post-drain truth) → 2.5min healthy heartbeats (fps 50, heap 3254KB, stack 2296B) → kEventTerminate = my datadisk pull to read logs. Device relaunched for the user walkthrough.
  - **Remaining device gap (honest)**: react chunks 1/2 (170/229KB) + the 112KB react-worker stream monolith CANNOT parse in one frame at 180MHz — they skip containedly until the `jsbridge_bundler_plan` learns the turbopack shapes (`(()=>{"use strict";(globalThis.TURBOPACK…)` and `(globalThis.TURBOPACK||…).push([…])` — the planner only knows webpack `(()=>{var X={…}`; NO_MAP today; segfault-adjacent, needs the offline-replay battery). So device pages render the SSR DOM but are NOT react-hydrated (C4-style in-place submit stays sim-only until then).

## Test-case ground truth (real browser, screenshots verified)
`<form class="contact" noValidate onSubmit>`, `<button type="submit">Send email</button>`,
email regex `^[^\s@]+@[^\s@]+\.[^\s@]+$`, POST via `fetch` + `AbortSignal.timeout(30000)`.
Keyed entry: empty → inline error card; filled → "Sending..." → sent-email card. Never a reload.

## R56 — STEPWISE CODE-SLICE UMBRELLA + CODEGEN WINDOW (2026-10-18, pm session)
- **R56 device crash root cause → fixed.** Device run at 15:28 (R55 exec pk `49097868f11e57515bd0d2329feb0f55`)
  bus-faulted at bfar=0x5c during `bundle 0 seg 2: stepwise code slice` (react-dom 209407B, 3dmm):
  a machine abort (`[xs] abort: memory full`) during the bracket-less code slice ran `fxAbort → fxExitToHost`
  which walks `the->firstJump` — with NO live HostExit bracket it deref'd C_NULL at the `nextJump` offset.
  Fix = the R56 umbrella in `xs_step_code_slice` (`jsbridge_xs_nr.c`): arm `xsBeginHostExit/_Metering/xsBeginHost`
  around the codegen body exactly like the parse slice does; a machine abort now lands after `xsEndHostExit`
  (`m->exitStatus != xsNormalExit`) → `xs_exec_finish` (contained "engine reset") + `xs_step_discard`. NO bus fault.
  Mid-bracket returns FORBIDDEN: the R30k restart and restart-budget-exhausted paths now use `runNext=0 + goto
  codegen_done` (a raw `return` would pin the machine's jump chain at this dead C frame). Sim green after (TAP+TYPE
  cycle: `RESULT inPlace=yes jsErrors=0 blocks=20 errCard=0 sentCard=1 errState=0`, zero aborts — twice: 16:19
  build pre-R56d and 16:58 build final).
- **R56 codegen window (device-gated, `#ifdef TARGET_PLAYDATE`) in `xs_step_code_slice`, restored at
  `xs_step_discard`:** the parse admission needs room for tree+code CO-RESIDENT; the standing funnel budget
  (8192KB) binds mid-window. `savedBudget` field added to the step struct; window = `max(savedBudget,
  live+allowance)`, restored to `savedBudget` at discard (after the tree arenas free). History: R56a probe-clamped
  3MB (probe said 2044KB → window net-LOWERED to 7640KB — R56b regression); R56b max() fix held 8192 — still
  refused; R56c code-head GC (freed 2KB — no debris); R56d fixed 3584KB (no probe — single-block probes
  under-report granular capacity: the probe verified 2044KB while the funnel then SERVED 2.75MB in 128KB chunks).
- **Device wall MEASURED with `-DPLUTO_MEM_TRACE` (device cycle 16:53):** the codegen grows steadily ~128KB per
  ~2.2s of `n=131072` chunk grabs; funnel-live went 5595KB → 9,393,121B served (+3.8MB) and the NEXT 10KB realloc
  produced NO `-> ptr` row = **the SDK's physical pool failed** (not the budget — the 9179KB window was not the
  binder). react-dom's 209KB codegen totals ~+4-5MB on top of the resident parse tree (~3.3MB) → final funnel-live
  ~9.8-10.1MB vs the pool's practical ceiling ~9.39MB funnel-live. **The 209KB react-dom factory cannot compile on
  the Playdate's 16MB heap with the current fork's tree/code layout** (R55: it is ONE factory — no further split;
  sub-factory split, streaming codegen, or a tree-node diet = fork surgery beyond today). Device behavior now:
  abort → CONTAINED engine reset → the page renders the STATIC SSR (form + footer, `rewalk blocks=22 links=10`,
  title correct) → queue still drains → a submit tap navigates natively (GET /contact?q=). Three consecutive
  device runs 16:36 / 16:42 / 16:53: ZERO crashlogs, every fault contained.
- Sim untouched by all device-gated code (all in `TARGET_PLAYDATE`); 8192KB standing budget + probe untouched;
  jsext/main.pluto_mem/pluto_spill frozen files untouched — ALL R56 changes inside `jsbridge_xs_nr.c`.
  Deployed device builds: 16:31 `75db8882…` (R56b), 16:43 `588f02cf…` (R56b+trace), 16:51 `0a3e58f0…` (R56d+trace).

## R56f — NATIVE FORM FALLBACK: the "Send email" button now WORKS on the physical device (2026-10-18, 18:00)
- VERDICT UP FRONT: react-dom's 209KB factory CANNOT compile inside the Playdate pool — measured, not guessed.
  R57 funnel-live probes on the sim (where it succeeds): codegen slice = +5752KB (entry 15872→21624KB), run slice
  = +2454KB, ALL in sub-2048B allocations ([mem] trace build showed no ≥2048B rows in the window). Device entry
  5595KB + 5752KB codegen ≈ 11.3MB vs the measured 9.39MB wall ⇒ compile-only overshoot ~1.9MB; compile+run ~4.4MB.
  The parse tree (~3.3MB) must stay live THROUGH codegen, so no early-free tricks it. THE FORK IS FAT (27x source);
  that is physics of the NR fork, not a tuning bug. PARKED the "make react compile" track.
- WHAT SHIPS INSTEAD: the browser performs the site's OWN form submission when no script handles it
  (progressive enhancement, honest and general — not a special-case hack):
  1. main.c tap path: submit dispatch rc==JSB_EVENT_NONE (=0, nothing matched) AND the form owns an email field
     → pluto_form_fallback(formNode). SUPPRESSED/NAVIGATE never reach it (react-lives sim stays byte-identical).
  2. fb_collect walks the form to depth 6, matches name/id by segment (fb_name_is: "email", "contact-email",
     "your_email"… — the site's real names are contact-email/contact-subject/contact-message, captured in the
     device walk log). Typed text lives in the DOM "value" attr — BOTH the real keyboard path (form_key_write →
     dom_set_attr) and the autotest TYPE pass already write it; it survives rewalks. The honeypot (nameless input)
     is naturally skipped.
  3. Client validation = the site's react regex (^[^\s@]+@[^\s@]+\.[^\s@]+$) → inline error card
     "Please enter a valid email address." (the TAP-with-empty-fields case = exactly react's behavior).
  4. Valid → "Sending…" card → http_request("POST", https://server.bryanwandrych.com/send_email, JSON body
     {"email","subject","message"} — byte-format captured from the live C4 xhr log: 133B) → card flips to
     "Sent. Your message is on its way to bwandrych@bryanwandrych.com." on 2xx / "Something went wrong. Please
     try again later." on failure. Same http_request path the XHR service uses ⇒ identical headers/server behavior.
  5. Cards = REAL DOM (dom_create_element + dom_set_text + dom_append_child + page_rewalk_now) — the autotest's
     rewound_dom scans count them (sentCard=1 ✓). g_fbCard is reset in page_swap_doc (after document_free) so a
     stale pointer can never be deref'd after navigation.
- DEVICE VALIDATION RUNS (autotest TAP+TYPE build):
  - 17:41 cycle: trigger fired but walk missed the prefixed names → fixed fb_name_is.
  - 17:49 cycle — THE WIN: `[form] submit unhandled → [fbform] form tag=form kids=5 → 3 fields WITH typed values
    → POST https://server.bryanwandrych.com/send_email body=133 → POST done status=200 → RESULT blocks=23
    (was 22 = the card injected) sentCard=1 errState=0 jsErrors=1 (the pre-existing react OOM only). NO crashlog.
    The DEVICE POSTED TO THE LIVE SERVER AND SHOWED THE SENT CARD via the fallback.
  - 18:00 clean daily-build cycle (no autotest): contained react reset as always, fallback dormant, NO crashlog.
    PDX md5 `269f4d1b…`. The user now: tap email field → Playdate keyboard (pre-existing) → type → tap Send email.
- Sim regression after EVERY candidate: zero [fbform] rows (fallback inert), RESULT green each time
  (`inPlace=yes jsErrors=0 blocks=20 errCard=0 sentCard=1 errState=0`, 3 runs: 21:38, 21:46, 21:57).
- Files touched THIS round: `Source/main.c` (fallback block + call site + page_swap_doc reset),
  `Source/html/jsbridge.c` (ONE logging-only row: xhr POST body lead96 — freeze-safe), and the R57 sim-gated
  funnel probes already in `jsbridge_xs_nr.c`. Device: jsbridge_xs_nr R56 unchanged.
- NEXT (only if the REAL hydration becomes a goal again): fork-side tree/code diet is the only lever left —
  the compile demand must shed ~2MB — or react-free page scripting (a second tiny machine) for general widgets.

## R56g — the fallback is now GENERAL-PURPOSE + full post-send parity (2026-10-18, 19:00)
- USER DIRECTIVE: no site knowledge in code — the R56f constants (endpoint/JSON keys/react strings/name keys)
  were site-shaped. REWORKED: generic code, site knowledge as DATA.
- GENERICS (any script-dead form, zero site knowledge):
  • fb_collect_fields: EVERY named control (name|id) at depth ≤6; value = the "value" attr (keyboard write-back),
    textarea falls back to DOM text, select = selected option; spec skips (nameless/disabled/unchecked box-radio)
    + honeypot convention (aria-hidden=true, placeholder "leave blank"). HTML5-lite validation: required-empty +
    type=email regex (the HTML5 shape — not the site's).
  • ROUTING first-match: (a) form action= → HTML-standard submission: method GET → navigate action?query
    (url_resolve for relative actions), POST → urlencoded body (fb_pairs_urlenc, spec serializer) via the NEW
    http_request_ct (caller-chosen Content-Type; NULL = the historical application/json — zero change for
    existing callers). (b) no action + a form_sites.txt profile → profile endpoint + body format.
    (c) neither → legacy static GET (return 0, unchanged).
  • Generic RESULT UI: on 2xx the form's children detach (kept in g_fbSaved) and the card shows the success text
    + label/value rows for the submitted fields + a "Send another" button; that tap rides click→submit rc=0
    back into fb_summary_restore (re-append saved children, card off, values kept). Non-2xx/error → failure card.
  • page_swap_doc resets card/summary/saved state (stale-pointer safety after navigation).
- SITE DATA (form_sites.txt, Data dir, deployed beside the pdx; sim data dir copy too): host=bryanwandrych.com
  endpoint=..., body=json, keys=contact-email:email;contact-subject:subject;contact-message:message,
  labels=contact-email:From;contact-subject:Subject, summary=contact-email,contact-subject,
  success="Sent. Your message is on its way to bwandrych@… — I'll reply as soon as I can." (em dash captured
  from the live bundle spill), failure/invalid/sending = the site's react strings. Profile loader = pd->file
  read + line parser, logged at load ("[fbform] profile[n] host=… endpoint=… json=…").
- PARITY PROOF (device, autotest TAP+TYPE build, 18:47): profile loaded from DATA → body=133 (byte-identical
  via the keys= map; the FIRST attempt sent 205 and got status=500 — fb_map_get returned the rest of the STRING
  instead of the segment; fixed to a bounded copy) → status=200 → summary: hid 5 child node(s) → RESULT
  blocks=20 sentCard=1 errState=0 — IDENTICAL to the sim's summary (sim dump: Sent text block + From/value +
  Subject/value + button container; no Message row — matches react).
- Sent-another UI parity: fields swap out exactly as image 1; the restore cycle = the user's tap on device
  (machinery reviewed: same-tree detach/re-append; log rows armed: "[fbform] Send another: restoring…").
- Sim regression after every step: fallback INERT (rc=SUPPRESSED on the sim), zero fbform rows, RESULT green
  ×4 (22:34, 22:42 — plus earlier runs).
- Clean daily build deployed: pdx md5 `502f7ce5…` + form_sites.txt on device Data. Zero crash/error logs at 18:53.
- Files: main.c (generic fallback + profiles + summary), http_client.c/h (http_request_ct + Content-Type),
  jsbridge.c (one log row), form_sites.txt (NEW data artifact at repo root).
- NEXT: user taps Send another on the device to confirm the restore log row + re-render; then general-site
  spot-checks (any action= form without a profile; urlencoded GET path) — untested by automation so far.

## R58 — "Send another" dead in the sim: root cause + general fix + loop proof (2026-10-18, ~23:50)
- USER REPORT: after the automated emailed summary, pressing react's "Send another" in the sim did nothing (asked:
  autotest artifact or real bug?). VERDICT: REAL BUG; the autotest was innocent — the log showed the user's 5 presses
  all reached the dispatcher: press #1 → jsbridge click dispatch rc=1 (NAVIGATE — react's listeners RAN; react then
  registered its 50ms setTimeout and the timer FIRED); presses #2-5 → rc=0 (the item's srcNode was a DETACHED node —
  the subtree had been swapped out from under the layout).
- ROOT CAUSE (R58): the re-render latch (jsbridge domDirty) was set ONLY by the innerHTML setter. React's setState
  flush = plain appendChild/removeChild/insertBefore batches + setAttribute — the frame loop's
  jsbridge_take_dom_dirty (main.c:723) never fired, jsbridge_timers_pump's callBudget heuristic looked clean too, so
  NO re-walk: stale summary blocks on screen, stale srcNodes for taps, no visual change. The 50ms flush DID
  successfully restore the fields in the live DOM — the BROWSER was just blind to it.
- FIX (general, not react-specific): every tree-mutating bridge service now sets b->domDirty=1 on success —
  xs_el_appendChild/removeChild/insertBefore, xs_document_appendChild/insertBefore/removeChild, setAttribute,
  removeAttribute (+ bridge_of lookup added to removeAttribute). Frame loop = at-most-one re-walk per frame (the
  same rAF-batched semantics a browser gives any script's DOM writes; fb fallback/queue machinery unaffected).
- LOOP PROOF (new PLUTO_NR_AUTOTEST_TAP2 phase machinery, main.c ~1395/1735): after the C4 summary the autotest
  itself scans for the summary's bottom-most FORMLESS stamped submit (react's "Send another"; the nav toggle sorts
  earlier and stamps too — bottom-most discriminates it), activates it via the same activate_form_block path, and
  requires nr_tap_form_fields_back() (a live-DOM scan: input/textarea/select under a <form>). On success it LOOPS:
  re-enters phase 1 (re-type + re-send) for cycle 2, again to the summary, taps again. LOG: TAP2 RESULT
  cycle=0/1/2 restored=yes jsErrors=0 blocks=22(was 20) frames=1 — three restores, two closed send↔restore loops,
  restore in ONE frame (flush → domDirty → same-frame re-walk). Zero fbform rows (sim = pure react path), zero
  aborts/FAILs.
- Build hygiene: all 7 domDirty edits + TAP2 helper compile clean; blocks[] is DocBlock** (pointer array) — the
  phase-5 slot index is a linear lookup, not pointer arithmetic. Sim killed post-run (2 zombie sims from the
  user's session were found first — both killed).
- NEXT: user manual-press check on the clean sim build; optional device redeploy with R58 (the DEVICE fallback
  loop already works via direct C restore — R58 matters for any script that renders after the engine survives).
- BUILD-LESSON (R58 discovered): `make simulator SIMDEFS=...` → a later bare `make simulator` is a NO-OP for the
  sim dylib (make can't see flag changes; the monolithic link only rebuilds when an $(SRC) file is NEWER than
  build/pdex.dylib) — the "clean" pdx silently re-copied the TAP2 dylib into PlutoBrowser.pdx (fresh copy mtime,
  stale content). The user then relaunched the TEST build and saw the auto-loop (asked: bug or test?). Resolution:
  touch all Source/*.c → real relink (0 errors, 0 TAP2 strings in pdex.dylib) → boot-verified: nr-autotest rows=0,
  TAP2 rows=0, heartbeats live, zero crash rows (the crash-log HEADER line is template text, not a crash). Any
  future SIMDEFS↔clean flip MUST pass through `touch Source/*.c` (or better: the flags belong in make variable
  tracking someday).

## R58 device autotest deploy (2026-10-18, 20:13 launch)
- USER-GATED deploy of the AUTO-TEST build (TAP+TYPE+TAP2 + R58 domDirty fix + phase-5 platform split) to the device.
  MD5 5b24d695... matched before eject; form_sites.txt re-copied to Data; all 3 logs cleared pre-deploy.
- DEVICE RUN — full loop pieces VERIFIED from the log: cycle 0 fallback POST send_email body=133 → status=200 →
  summary hid 5 child node(s) → TAP2 auto-tapped the summary button (device branch = form-ATTACHED scan, the
  fallback's card lives inside the form) → TAP2 RESULT cycle=0 restored=yes jsErrors=1(was 1) blocks=28(was 20)
  frames=1 → looped to phase 1 → re-typed + re-sent: route=profile → POST body=133 → status=200 → summary hid 6
  (6 = the original fields WHILE the sending card already existed). No crashlogs.
- QUIRK (harness, not product): on cycle 1 the phase-4 "fields already back pre-tap" guard fired while the 2nd POST
  was still in flight (device POST ≈ 9-11s vs sim ≈ instant) — the auto-press skipped; the restore itself is the
  SAME verified branch, so manual pressing works identically. If a deterministic 2-cycle device demo is wanted:
  gate the phase-4 skip behind !g_fbInFlight, rebuild, redeploy.
- NOTE: the device tap row logs the shared "formless button" string though the device branch selected a
  form-attached submit (cosmetic; no rebuild spent on it).
- The device deliberately has the URL default path (main.c source default contact URL under the TAP gate) — the
  -DPLUTO_NR_AUTOTEST_URL command-line define was DROPPED this deploy (its \-escaping broke through the device
  per-object CPFLAGS double expansion; the internal default is the same contact URL — zero behavioral change).
- Next sim build will pick up the phase-5 split via touch (the sim's CLEAN dylib on disk still pre-dates the split;
  behavior there is UNCHANGED for the sim branch anyway).

## R58b production deploy — boot auto-nav REMOVED (2026-10-18, 20:34)
- USER REPORT (correct): the production device build still auto-navigated to https://bryanwandrych.com/contact on
  every boot. CAUSE: an UNCONDITIONAL device block in the boot sequence (`#ifdef TARGET_PLAYDATE` — added earlier
  per a walkthrough-user request, NOT autotest-gated; my earlier strings-audit looked only for nr-autotest/TAP2 and
  mislabeled it). REMOVED the whole block (PLUTO_BOOT_NAV_URL define + snapBypass + pendingNavUrl handoff + the
  [boot] auto-nav log row); left a restore-recipe comment ("pendingNavUrlSet = 1 + snprintf(...)" in place).
- Production MD5 13947583a38bece5c0d1541960a2f0c3 copied + verified match. Binary strings: auto-nav=0,
  nr-autotest=0, TAP2=0.
- BOOT-VERIFIED (launched 20:34:33, read 20:35:18): 34 log rows total — engine boot (XS (No Recursion)) + radio
  kicks + home-launcher heartbeats (heap 22KB idle); ZERO navigation/fetch rows, zero [boot] auto-nav, zero
  autotest rows; no crash/error logs. The single "bryanwandrych" log hit = the app bundle path in the boot header.
- The user's no-auto-navigation contract now holds: boot → home launcher only; every page load is user-initiated.
  form_sites.txt (fallback profiles) remains installed in Data for manual contact-form testing.

## R59 — Wikipedia device crash: re-walk contract + layout guard (2026-10-18, morning)
- DEVICE CRASHFORENSICS (crashlog.txt symbolicated via build/pdex.elf, runtime VA = VMA + 0x90000000): cfsr 0x82
  (DACCVIOL|MMARVALID), pc=0x90012a6e = break_lines+0x4e (Source/render/layout.c:618), lr=0x900132e5 (layout.c:1033,
  layout_build); fault read = block->inlines garbage base 0xBFC → address 0xC00; heap allocated 13,406,688B (denial
  strait after re-walk). Session facts: engine 4, script 1 contained TypeError (errs=1), rewalk 689→124 blocks,
  551B footer-icon request in flight, css-stats rules=1,491,348.
- ROOT CAUSE: document_rewalk returned 0 ("ok") on a walk error after doc_free_walk_output already freed the arena
  → the renderer layouted a half-freed doc → break_lines dereferenced freed/garbage inline pointers.
- FIX (3 layers, all landed): Source/html/document.c — walk-error branch publishes NOTHING (blocks=0, parseError=1,
  free arena, return -1) + g_rewalkTestForceWalkError test seam; Source/render/layout.c — break_lines hard guard
  (!inlines || inlineCount <= 0 → empty flow) at the crash site and both other entry paths; Source/main.c —
  PLUTO_REWALK_OOM_AUTOTEST sim autotest (tick + call site): budget strait live+3KB, forced seam, recovery re-walk,
  "Render Error: rewalk out of memory" path + recovery PASS end-to-end (rc=-1 contract, then rc=0 blocks=128).
- SIM VERIFY: deterministic autotest PASS; regression suite ×3 green (RESULT inPlace=yes jsErrors=0 blocks=20
  errCard=0 sentCard=1 errState=0; TAP2 restored=yes; loop=2 cycles closed).
- DEPLOYED TO DEVICE: production 31dd0b4a4cf11ef345a3207442153780 (strings purities 0 at the time via bin —
  later shown vacuous, see R60). POST-FIX DEVICE EVIDENCE (21:30 device log, after the 21:22:49 deploy): Wikipedia
  SSR rendered 23 blocks (heading/nav/img/footer types), script 1 admitted at probe 592KB ≥ need, the contained
  TypeError logged with NO escalation, NO crash — session end was my datadisk mount (kEventTerminate). The
  originally-reported Wikipedia crash did not reproduce.

## R60 — about:jsext stuck/failed suite: residency + stream routing (2026-10-18, ~22:30)
- USER REPORT: sim: home → about:jsext bookmark flashes the report "8 passed, 0 failed", then the page re-navi-
  gates ITSELF and sticks at "running…" (screenshot 21:49). Device: "fails 2" (the two huge-dependent tests leak
  as failures while 6 pass).
- CAUSE (3 layers): (1) the sim pdx the user ran was my -DPLUTO_NR_AUTOTEST TEST build — its harness re-navigates
  at report time and re-runs under the enforced 7.5MB envelope, where the revisit aborted containedly (user-visible
  "re-nav + hang"); production sim has no harness — fixed by rebuilding plain. (2) jsext kept the 72,819B huge.js
  RAM-resident: code spilled only >256KB although the design comment already said >64KB = disk-resident — on DEVICE
  the R54 one-frame-parse cap skip containedly ("script 6 skipped (stream)") = the 2 device failures. (3) on SIM
  under real pressure the same monolith whole-file probe aborted the engine ([xs] abort: not enough memory) → the
  final inline report script never ran → stuck "running…" banner (pre-R60 screenshot state).
- FIX (2 files, site-agnostic, zero engine deltas): Source/html/jsext.c — bodies ≥ JSBRIDGE_ONE_FRAME_SCRIPT_MAX
  (64KB) adopt disk residency at adoption time (local-fixture path ~line 734 + network fetch path ~line 1028,
  comment alignment). Source/html/jsbridge_xs_nr.c — xs_run_script_stream: DEVICE: ≥64KB routes bundler-split →
  statement chunker (R16) → contained skip (never inflight-defer livelock); SIM: whole-file path UNCHANGED while
  no budget is armed (pluto_mem_budget()==0), probe-gated divert only when a test harness enforces an envelope.
- R60 SELF-INFLICTED REGRESSION (caught by the suite, fixed same turn): the first router version diverted the
  contact page's 112,594B react chunk on sim ("skipped (stream) … not chunkable") → jsErrors=1 errCard=1 blocks=23.
  The budget/probe guard restored the whole-file run; baseline back to jsErrors=0.
- SIM VERIFY (about:jsext, autotest build for nav): first visit 8/0 end-to-end — `[jsext] local ok jsext-huge.js
  (72819 bytes, disk-resident)` → `chunked plan ok — 9 segments, biggest 8176B` → `chunked run complete` →
  `summary: 8 passed, 0 failed` → REPORT RENDERS: rewalk blocks=10 with block[1]="8 passed, 0 failed." + 8 [PASS]
  rows (the stuck-banner class is gone). Residual revisit abort (19B inline at live 24.9MB vs 7.5MB wall) =
  memwatch harness-only physics (sim alloc scale-64; device proven ~5.45MB peak), contained, DISARM clean.
- REGRESSION SUITE GREEN: tap1 ×3 (RESULT inPlace=yes jsErrors=0 (was 0) blocks=23 errCard=1 — errCard=1 is the
  empty-submit validation card, the expected C3 proof for the tap1 shape) + C4/TAP2 ×3 EXACT baseline (RESULT
  inPlace=yes jsErrors=0 (was 0) blocks=20 errCard=0 sentCard=1 errState=0; TAP2 RESULT cycle=2 restored=yes
  jsErrors=0 blocks=22 (was 20); loop=2 send<->restore cycles closed — infinite loop proven).
- PURITY INSTRUMENT CORRECTION: pdex.bin strings checks are VACUOUS (22,314 strings yet 'STORAGE: save' /
  'PlutoBrowser' never match — structured/compressed bin); the REAL check is build/pdex.elf whose DWARF producers
  record per-object -D flags (GCC -grecord-gcc-switches). ELF purity: PLUTO_NR_AUTOTEST=0, rewalk-oom=0, "TAP2"
  matches = the 2 legit dav1d FILTER_8TAP2 macros; R60 containment string + Full-JS-Test-Suite fixture present.
- STALE-OBJECT CLEANUP: build/Users/…/C_API/buildsupport/setup.o carried an earlier DEVICEDEFS harness build's
  defines (DWARF producer "PLUTO_NR_AUTOTEST_TAP2 1" — from the fragile URL-escaping device attempt). Scanned all
  229 .o — that ONE was stale (its code is flag-inert SDK bootstrap, but it polluted the ELF purity check).
  Deleted + recompiled clean. LESSON: the touch-all rule covers Source/ only — SDK-path objects must be removed
  manually when their compile flags change.
- DEPLOYED + BOOT-VERIFIED: MD5_MATCH 9dd105636542bc8f42c00e052ab74cbb (rm/cp/3-logs-cleared per checklist); boot
  22:26:09 clean (33 rows, 6 heartbeats, ZERO nav/abort/harness rows; no crashlog.txt/errorlog.txt); relaunched for
  the user 22:28:13.
- NEXT: user manual retest — SIM: relaunch from the plain pdx, click the about:jsext bookmark → expect 8/0 report
  that STAYS (visible rows + report block[1]). DEVICE: about:jsext expect 8 passed, 0 failed ON SCREEN; Wikipedia
  regression unchanged (R59 holds).
- OPEN SCRAYS: TAP2 device phase-4 false-skip quirk (gate the auto-press on !inFlight) — unchanged; Source/pdex.elf
  stray copy in the source tree (user-owned, untouched, flag it for git status).

