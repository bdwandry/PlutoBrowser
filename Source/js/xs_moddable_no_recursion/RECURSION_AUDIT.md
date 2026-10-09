# RECURSION AUDIT — xs_moddable_no_recursion

Goal: **every recursion whose depth scales with page/script input is converted to an
explicit heap structure** (worklist / pool frame / state machine), so the game-task
C stack (61.8 KB) cannot overflow from parsing/executing content. This file is the
running record of the manual file-by-file read of `sources/*.c`. Updated as each
file is read and again as each item is converted. Findings are never deleted; they
get a status.

Status legend:
- `INPUT-SCALED` — depth grows with content; **must convert**
- `CONVERTED` — was recursive, now heap-iterative (verify note kept)
- `CONSTANT` — read; recursion depth fixed/small or already iterative
- `NONE` — read; no recursion found in file

Build flags that matter: `mxMetering=1` (host loop budget, not stack), game-task
stack = 61.8 KB, engine heap = budgeted (device SW3a gate).

Error path note (applies to all conversions): `fxPatternParserError` longjmps out of
arbitrary depth. Any heap structure used during regex parse must be freed on BOTH
the success and the longjmp path. Parser term chunks already are (freed by
`fxPatternParserTerminate`, called on both paths in `fxCompileRegExp`).

---

## Read progress (48 .c files in sources/)

| # | File | Status |
|---|------|--------|
| 1 | xsre.c | READ — R1–R3 **CONVERTED** (frame machine verified), R4 **CONVERTED** (below) |
| 2 | xsJSON.c | READ — parse walk CONVERTED; revive ITERATIVE; R5 exception-safety DONE (verified vs stock); R6 **DONE** (stringify = run-stack pump; 64KB proof) |
| 3 | xsRegExp.c | READ — NONE (split_aux already flattened; hosts are natives) |
| 4 | xsTree.c | READ — NONE live (fxNodePrintTree recurses but is behind dead `mxTreePrint`) |
| 5 | xsArray.c | READ — NONE (quicksort = explicit 30-deep txSortPartition stack, insertion fallback) |
| 6 | xsMemory.c | READ — CONVERTED (GC mark = heap worklist + drain; fxMarkInstance = pointer-reversal) |
| 7 | xsSyntaxical.c | READ — CONVERTED (pump helpers verified; minor cleanup: triple `pumpBase` assignment) |
| 8 | xsCode.c | READ — **R7 CODE EMITTERS CONVERTED 100% (mixed-mode pump complete; 79/79 gxCodeSteps kinds in stage machines, zero native emitters left; PROG 44/44 + MODULE 6/6 payload-exact; ASan clean; deep-AST compile survival proven)** |
| 9 | xsScope.c | READ — CONVERTED (nodeWalk pump + fxBodyNodeHoist/fxClassNodeHoist fixes from earlier) |
| 10 | xsSourceMap.c | READ — NONE (uses the iterative JSON value pump) |
| 11 | xsRun.c | READ — R9 **DONE** (soft C-stack guard at the 4 native-reentry entries: catchable RangeError; hard parser abort preserved as backstop) |
| 12 | xsModule.c | READ — R10 **CONVERTED** (fxLinkCircularities + fxLinkTransfer + fxOrderModule = heap pumps/loop; stock parity incl. 64KB-stack proof) |
| 13 | xsSnapshot.c | READ — NOT IN BUILD (excluded from SRC_DEVICE); guarded-only recursion (R8 note) |
| 14 | xsMarshall.c | READ — R11 **CONVERTED** (all three transfer walkers = heap frame-stack pumps; stock parity incl. quirks verified) |
| 15 | xsDebug.c | READ — NONE in this build (mxDebug OFF; fxDebugEval/fxDebugLoop dead code; fxBubble iterative) |
| 16 | xsProxy.c | READ — NONE live (traps are natives; target-behavior delegation is flat; NO trap-chain recursion) |
| 17 | xsError.c | READ — NONE (fx_Error_aux leaf; disposable-stack natives) |
| 18 | xsGlobal.c | READ — NONE (fx_trace_aux leaf; eval hosts go through fxRunScript=R9) |
| 19 | xsProperty.c | READ — NONE (property-list walkers all flat loops; fxGroupBy/fxGetIterator/fxIteratorNext iterative) |
| 20 | xsFunction.c | READ — NONE (fxStepAsync = promise re-arm, no self-call; async stepping is job-queue driven) |
| 21 | xsGenerator.c | READ — NONE (fx_Generator_prototype_aux + fxAsyncGeneratorStep re-enter fxRunID per resumption, generator state lives in heap slots; iterator-helper `*_step` callbacks are per-item natives driven by loops) |
| 22 | xsPromise.c | READ — NONE (resolve/then/finally all queue jobs; fxRunPromiseJobs = flat while loop; fxResolvePromise iterates thens list) |
| 23 | xsString.c | READ — NONE (split_aux/replaceAux flattened; all loops) |
| 24 | xsLexical.c | READ — NONE (scanner: fxGetNextCharacter/fxGetNextString/fxGetNextTokenAux all loops; JSX tokenizers loops) |
| 25 | xsTree.c (fxParserTree) | READ — NONE (entry only; fxProgram/fxModule wrappers → pump) |
| 26 | xsPlatforms.c | READ — NONE (fxParseScript = setjmp wrapper around tree/hoist/bind/code phases — all converted phases) |

## Remaining files quick-verdict (leaf-host files, no recursion seen reading defs+calls)
- xsDate.c, xsDataView.c (c_qsort with native compare fn = C-depth 1), xsMapSet.c,
  xsObject.c, xsType.c, xsBigInt.c, xsAtomics.c, xsAPI.c (fxToString verified),
  xsArguments.c, xsSymbol.c, xsNumber.c, xsBoolean.c, xsMath.c, xsLockdown.c,
  xsDefaults.c, xsProfile.c, xsCommon.c, xsmc.c, xsum.c, xsScript.c (chunk
  allocator + error reporters, flat), xsGlobal.c, xsError.c.
  These are natives/helpers whose repeated fxX( calls target OTHER functions
  (helpers on different inputs), not self/mutual recursion. Verified by reading
  the candidate bodies above.

---

### xsProxy.c (verified)
- Every trap (fxProxyGetOwnProperty etc.): mxProxyDeclarations + one handler
  `mxRunCount` + `mxBehavior*` on the TARGET (flat dispatch, no proxy chain
  recursion — target behavior is read once, not per-prototype). Verified NOT
  recursive even for proxy-of-proxy: getPrototypeOf walks via
  `fxGetPrototype` loops in the caller paths. VERIFIED iterative.

### xsError.c / xsGlobal.c / xsProperty.c (verified)
- fx_Error_aux: single-pass constructor helper, no self-call.
- fx_trace_aux: formats and calls fxBubble (frame-walk loop) — flat.
- fxNext*/fxQueue* property helpers: linked-list appends — flat.

### xsDebug.c (verified DEAD in build)
- Build has no `mxDebug` (checked device .o: only fxReport*/fxBubble_nr/
  fxGenerateProfileID_nr exported; fxDebugLoop/fxDebugEval absent).
- Had mxDebug been on: fxDebugEval→fxDebugEvalExpression→mxRunCount is one
  frame per debugger eval (user-triggered, not page-controlled). N/A now.

---

### xsRun.c (interpreter + native bridges)

- `fxRunID` (421): ONE flat computed-goto threaded-code loop; JS→JS calls are
  frame swaps on the machine's HEAP slot stack inside the same loop. No recursion.
- **R9 DONE 2026-09-30. Native→JS re-entry C-recursion — guarded, not
  converted (fxRunID is monolithic).**
  - `fxCheckCStackSoft` (xsRun.c, static): throws `mxRangeError("stack
    overflow")` when the C stack is within 8KB of `the->stackLimit` — the
    same `fxCStackLimit()` anchor the hard check and the parser use, so the
    soft guard always fires BEFORE the hard aborts (fxCheckCStack
    XS_NATIVE_STACK_OVERFLOW_EXIT and fxCheckParserStack), making deep
    eval-of-eval chains catchable instead of fatal.
  - Installed at the entry of `fxRunScript`, `fxRunEval`, `fxRunForAwaitOf`,
    `fxRunUsed` — before any state mutation (throws unwind through the
    machine jump, restoring the value stack). Inert on hosts whose
    `fxCStackLimit` is a no-op data address (never reached).
  - Baseline (pre-fix, real 60KB budget): 2000-deep eval chain aborted with
    status 9 (XS_NATIVE_STACK_OVERFLOW_EXIT from the parser) — in BOTH
    binaries. Post-fix: fork throws catchable `RangeError: stack overflow`
    (`e instanceof RangeError === true`, script continues and reports
    `alive=true` at 5000-deep); stock unchanged (hard abort).
  - Normal-depth margin: 800-deep plain call tree under the same 60KB budget
    runs to completion identically on both binaries — the guard does not fire
    early.
  - Parity on no-op hosts: full xrt battery 321/322 identical (deeper.js =
    the known R11 stock-segfault/fork-clean case); ASan/UBSan clean on the
    18-test suite and the module tests (xrt_san/xrm_san rebuilt).

### xsModule.c

- `fxLoadModules`/`fxLoadModulesFrom`/`fxExecuteModules`/`fxExecuteModulesFrom`:
  queue-driven loops — iterative. VERIFIED.
- **R10 DONE 2026-09-30. `fxLinkCircularities` converted to a heap run-stack
  state machine (sxLinkRun; phases ENTER → MERGE → STAR). Stock recursed once
  per `export * from` source (star re-exports) with no guard at all.**
  - The recursive call is replaced by pushing a child run (module, circularities
    copy, stars slot) that completes before the parent resumes at its saved
    transfer cursor — identical call/return order. Children only read/write XS
    heap lists (circularities copy, stars), so no parent register state crosses
    the boundary; the parent's export/reexport cursors are recomputed or carried
    in the run record (`run->reexport` survives suspension).
  - Value-stack order preserved exactly: reexports and circularitiesCopy+stars
    pushed inside the merge phase; the two mxPop()s per star transfer run in the
    driver's inStar handshake right after `fxLinkStarMerge` (stock: the pops at
    the end of each star-transfer iteration); the final mxPop() at merge end.
  - The star-merge block (ambiguity: `ambiguousModule != starModule` → null
    export) is stock code verbatim in `fxLinkStarMerge`.
- **R10b DONE. `fxLinkTransfer` converted to a loop.** The only self-call is a
  tail call (`transfer` is not a per-level parameter), and every throw site
  reports values tied to that same transfer, so reassigning module/importID and
  restarting is equivalent. Stock's `fxCheckCStack` fired only after the chain
  was already on the C stack; the loop keeps C-stack O(1).
- **R10c DONE. `fxOrderModule` converted to a heap worklist (sxOrderFrame with
  per-node paused transfer cursor).** Found during verification: stock's
  post-order DFS over the module graph (also unguarded) crashed at the same
  depths as the link walkers — it is part of the same pipeline
  (fxLinkModules runs it first). Traversal order preserved: node unlinked from
  the queue on first visit, "from" subtrees ordered first, node appended to the
  order list last (stock's post-order append; dedup by module reference kept).
- Verification (harness `/tmp/nrharness/xrm.c` + file finder/loader in
  `mods/loader.c` driving `fxRunImportNow` → `fxLinkModules` end to end):
  functional parity vs stock — basic import (2-module), 3-deep `export *`
  chain (`r=blue,green,pink,red`), ambiguous-star resolution (`p5`),
  namespace reads (`p4`); 100-deep star chain (`r=100 cnt=100`) at 8MB AND
  64KB stacks; 500-deep direct re-export chain at 8MB AND 64KB; ASan/UBSan
  clean on all 7 module tests. **Headline: 2000-deep direct re-export chain on
  a 64KB stack — stock SEGFAULTS (fxLinkTransfer C recursion), fork returns
  the correct value.** Star-500 hits a separate engine limit byte-identically
  on both binaries (parity, not a stack issue); at extreme depths the shared
  O(N²) circularities-copy heap pressure GC-crashes both before link (crash
  report: fxMarkReference during load) — out of scope, identical in stock.
- R11 note: the xsMarshall conversion is untouched by this round; xrt battery
  re-run after R10: 317/318 identical (deeper.js = known stock-segfault/fork-
  clean case from R11).

### xsSnapshot.c

- NOT COMPILED: `xsSnapshot.c` absent from SRC_DEVICE (45 stock sources listed;
  snapshot excluded by upstream design — Playdate device has no snapshot feature).
- For completeness: fxIndexReference/fxMeasureChunk/fxWriteSlot/fxReadSlot/
  fxProjectSlot chains are native-recursive over instance graphs with
  `fxCheckCStack` guards (823–1030 area). **R8: if ever added to the build,
  convert like GC-mark (worklist). Today: N/A — not linked.**

### xsMarshall.c

- **R11 DONE 2026-09-30. `fxMarshallSlot` ↔ `fxMarshallReference`/`fxMarshallChunk`,
  `fxMeasureSlot` ↔ `fxMeasureReference`, `fxDemarshallSlot` ↔ `fxDemarshallReference`
  all converted to heap frame-stack pumps** (`sxMarshallFrame` list + `gxMarshallFrames`,
  `fxMarshallFramePush`/`fxMarshallFramePop`, `fxMarshallFramesFreeAll`; c_malloc,
  fxAbort on OOM). Zero C recursion left in the file; `mxCheckCStack` removed from
  `fxMarshallReference`.
  - Marshall pump: stages Enter/Instance/Array/List/Private/Child/ProxyHandler/
    ProxyTarget/WaitChild/WaitProxyHandler/WaitProxyTarget. ENTER gate reproduces
    stock's switch statement-for-statement (buffer layout, carve-and-write, mark/
    link bookkeeping, elision slots identical; ArrayStage inlines the elision carve
    without setting `elision.value.at.id`, matching stock's buffer bytes).
    `push_ref_child:` label sits on ChildStage; REFERENCE/ERROR/Proxy slow paths set
    a Wait* stage + target address then re-dispatch. Finish block: read ok/route off
    the child, pop it, then `fxMarshallChildDone(parent, route, ok, ...)` — ChildDone
    runs AFTER the child is popped (use-after-free ordering).
  - `fxMarshallChildDone`: route switch advances the output cursor (Array/Private
    unconditional; Instance only `if (ok)` — this preserves the 3-key
    first-property-lost quirk; List sets `list.last` + advance, ungated). Parent-stage
    switch advances the source cursor (Instance/List/Private `aSlot=aSlot->next`,
    Array `slot++`) and resolves Wait stages: WaitProxyHandler → ProxyTargetStage;
    WaitChild/WaitProxyTarget must NOT be popped here — they re-dispatch into their
    own finish block, which pops them and cascades ChildDone to their parent. (The
    original direct pop was the last bug: it skipped the wait frame's route cascade →
    stale parent cursor → double carve → cur>size → 32-byte heap overrun, ASan WRITE
    at fxMarshallKey. O0 passed by luck.)
  - Measure pump: `fxMeasureReference` keeps stock's 3-way gate and calls a wrapper
    that pushes a frame and runs `fxMeasureSlotRun`. Every pop site runs
    `fxMeasureChildDone(frame)` then pops — adornment balance is critical: ChildDone
    `mxPop()`s the child's `mxPushAt` adornment when the parent stage (Instance/Array/
    PrivateChildren) consumes it; DoneStage self-pops (slow-reference parent).
  - Demarshall pump: ENTER REFERENCE transform stashes `aResult` on the frame;
    `fxDemarshallFrameFinish` applies the reference patch
    (`value.reference = value.error.info; kind = XS_REFERENCE_KIND`), then
    `fxDemarshallChildDone` + pop.
  - Throw/catch wiring: `fxDemarshall` saves `base = gxMarshallFrames` before `mxTry`,
    catch unwinds `while (gxMarshallFrames != base) fxMarshallFramePop();`; the
    `fxMarshall` catch branch calls `fxMarshallFramesFreeAll()`; `fxMeasureThrow` frees
    all frames first (error message reads only value-stack adornments).
  - Quirk preservation (byte-for-byte vs stock, O1): r62 3-key roundtrip loses FIRST
    property, r47 `{w:5}`→{} (STOCK-QUIRK-CONFIRMED cnt=2 probe=5), q4/q8 `{}`.
  - Verification: 18/18 exact parity at O1 and O0 (p1–p3, q1–q10, r47/r62/r63,
    rt_basic, rt_dbg3); 301-file battery: 297 exact match, 3 false flags (identical
    outputs, shell rc artifact), deeper.js = stock SEGFAULT vs fork clean abort;
    ASan/UBSan clean on all 18 (incl. former fxMarshallKey OOB site); stress_o2
    rebuilt, bundle pipeline OK. Deep-nesting: with `ulimit -s 64` (61.8KB game-task
    emulation) stock segfaults at 1500-deep (array AND object graphs) while the fork
    returns normally; at full 8MB stack both binaries stay in parity to the shared
    metering abort (5k-deep); ≥10k-deep dies identically in the out-of-scope GC mark
    recursion on both.

### Runtime coercion paths verified iterative
- `fxToString` (xsAPI.c 273): switch + `goto again` loop; nested arrays stringify
  via `Array.prototype.join` (xsArray.c 2184) = FLAT element loop (each element
  stringified at the same C depth). `Array.prototype.toString` calls join via
  mxRunCount (one frame, not per-element). No depth scaling. VERIFIED.
- `fxToPrimitive` (xsType.c 270): single mxRunCount(1) (Symbol.toPrimitive),
  no self-recursion. `fxCheckCStack` absent but depth doesn't scale. VERIFIED.

---

### xsJSON.c

**R6 DONE 2026-09-30. `fxStringifyJSONProperty` converted to a run-stack pump
(sxStringifyRun; phases HEAD → ARRAY/OBJECT).** Stock recursed once per array
element / object property with only `mxCheckCStack` (hard abort, too late on
the 61.8KB game-task stack).
- Run record: key-slot address (stable across child pushes — child args go
  ABOVE it), container instance, loop cursor (`index` / `at`+`property`
  scratch address), and the container's separator cell (`ownFlag` with
  `flagAddress` pointing at the PARENT's cell — children mutate the same cell
  stock's by-reference `aFlag` mutated: first child emits no comma, later
  children emit ","+indent; undefined elements set it for the next child).
- Stock argument convention preserved: [key][value][wrapper] pushed top-down;
  HEAD phase derives `aKey/aValue/aWrapper` per stock. toJSON and replacer
  re-enter JavaScript exactly like stock (mxRunCount); rawJSON shortcut, the
  XS_LEVEL_FLAG cycle guard (set on container enter, cleared on normal
  completion and by the outer catch on throw) and the read-only-value /
  cyclic-value errors are stock-verbatim.
- Value-stack order per phase matches stock; children push/pop their own args
  symmetrically so parent anchors stay valid across child runs.
- Verification vs stock: 6/6 exact parity (nested objects/arrays, escapes,
  toJSON via Object.prototype, replacer array + indent variants incl.
  multi-line/tab byte-identical, cycle `cyclic value`, undefined holes in
  arrays, empty containers); throw-in-toJSON cleanup + post-throw reuse
  parity; re-entrancy (stringify ∘ parse) parity; ASan/UBSan clean on all 8.
  **64KB-stack headline: 500-deep runtime stringify — stock SEGFAULTS, fork
  completes.** (Deep JSON *literals* die in the shared parser recursion
  before runtime on both binaries — out of scope.) Full xrt battery after
  the change: 330/331 identical (deeper.js = known R11 case).

**R5. `fxReviveStack` (static) not exception-safe — CONVERTED + VERIFIED (2026-09-29)**
- Parse-side walk is longjmp-safe: `the->jsonWalkFree = fxJSONWalkFreeAllHook`
  (set in fx_JSON_parse, cleared after; interpreter catch path calls it).
- Revive frames (`fxReviveStack`, c_malloc'd, static) now have their own
  protection: `fx_JSON_parse` wraps the revive phase in mxTry/mxCatch; the
  catch calls `fxReviveFreeAll()` (frees every abandoned frame, NULLs the
  static), clears BOTH machine fields (`jsonWalkFree`, `walkContext`), and
  rethrows. Success path clears both fields too (a stale walkContext could
  otherwise fire from a later unrelated throw inside a nested native).
- The parse phase's catch (innermost jump target since this change) frees the
  abandoned walk frames itself and clears both fields — the interpreter-boundary
  hook would NOT fire for throws caught by this inner mxCatch.
- Re-entrancy (reviver calls JSON.parse): the shared `fxReviveStack` is pumped
  per-invocation. `fxReviveJSON` captures `base = frame->next` ONCE in a C
  local (never re-read from the heap — root frame is freed when it pops, so a
  `root->next` condition would be use-after-free); the pump loop runs while
  `fxReviveStack != base`, and delivery to a parent is skipped when the parent
  IS the base (outer pump delivers the root result). Inner invocation pumps
  only its own frames; the outer's suspended frames below `base` belong to the
  outer pump, which is blocked inside its mxRunCount(3) while the reviver runs.
- Two R5-era bugs found and fixed by testing: (1) object-key cursor was
  double-advanced (advance in delivery AND in the while condition) → after the
  last key `at` became NULL and the object branch re-created keys forever,
  leaking call slots to a value-stack abort; (2) the initial loop condition
  `while (fxReviveStack)` let an inner pump consume outer frames →
  "call: not a function" corruption of the outer's in-flight call window.
- VERIFIED: 6-test runtime suite (xrun harness, real machine + interpreter):
  basic revive, reviver-throw mid-walk, re-entrant parse (success + throw),
  parse-throw stress x50, 600-deep revive + 300 throw/free cycles (where stock
  would need 600 C-stack frames; on the device's 61.8KB stack stock crashes),
  edge semantics (root primitive key "", keys-array filter, this-binding,
  delete path). 6/6 identical verdicts vs stock binary. Full battery 189/189;
  502KB bundle parity holds; stringifier sanity pass.

**R6. `fxStringifyJSONProperty` recursion — INPUT-SCALED, TODO**
- Self-recursion per array element (1464) and per object property (1516);
  depth = container nesting (attacker/page controlled). Also does
  `mxRunCount`/`mxGetID` (host callbacks can nest: toJSON/replacer).
- It runs at page-JS runtime on the game-task stack. `mxCheckCStack()` guard
  present (clean failure, not conversion). Plan: trampoline frames keyed on
  (instance, cursor) with stage machine for the open/close brackets + comma
  logic; level flag already provides cycle detection.

### xsCode.c

**R7. Code-emission walk — CONVERTED 100% (mixed-mode pump complete; updated 2026-10-01, R7/B5d)**
- State: `fxNodeCodeBody` stage machines live for ALL 79 gxCodeSteps rows
(79 distinct WC_ kinds; zero kind-0 rows, zero native emitters left). Blocks:
B2 expressions, B3 call/new/chain/template, B4a statements/loops/try, B4b
for/switch/catch/try + parser try/catch AST fixes, B5a params/object/array/
function + WC_EXPRESSIONS_THIS/DELETE wrappers, B5b DECLARE family/DEFINE/
FIELD/CLASS, B5c INCLUDE/IMPORT_CALL/MODULE/PROGRAM + root shims, B5d final
sweep — BINDING, BINDING_ASSIGN, BINDING_REFERENCE, ARRAY_BINDING_ASSIGN
(for-of destructuring protocol), OBJECT_BINDING_ASSIGN, PARAMS_BINDING,
COMPOUND_NAME, DELEGATE (yield*), MEMBER_AT_ASSIGN, MEMBER_AT_THIS (dormant
machine gated), MEMBER_THIS, PRIVATE_MEMBER_THIS, REGEXP, SUPER. The
WC_NATIVE/WC_NASSIGN/WC_NREF/WC_NTHIS wrappers remain as safety nets only.
- Pump invariants learned (all three bit us): (1) pop-before-epilogue — a
  machine's epilogue dispatch must run with its own frame already popped;
  (2) every resume path that parks mid-iteration must mirror the skipped tail
  of the parked iteration (cursor advance AND flag updates) or the loop
  re-dispatches the same child forever; (3) loop-carried flags must be set
  BEFORE the child dispatch if a later iteration reads them, since a parked
  child returns early.
- Session bugs found + fixed (all caught by the payload gold standard):
  1. xsSyntaxical.c `fxFromExprRun` captured `base = parser->fromExprStack`
     AFTER the converter helpers (fxArrayBindingFromExpression etc.) pre-pushed
     their container frame — the container frame sat below base and never ran,
     so `[o.x, o.y] = ...` kept its raw Assign(Member, Array) shape (stock
     rewrites it to ArrayBinding). Fixed with `fxFromExprRunBase(..., base)`
     taking the stack pointer captured BEFORE the pre-push.
  2. xsCode.c WC_ARRAY stage-3/4 resume cases did not advance `frame->v1` →
     infinite loop whenever a converted child element parked (IIFE element in
     a spread array). Fixed by mirroring the skipped cursor/index advance.
  3. xsCode.c WC_EXPRESSIONS set `previous` flag after the park check → the
     POP between comma items was lost when item 1 parked (1-byte payload diff
     on `o.x = 1, o.y = 2`). Fixed by setting the flag before the dispatch.
- B5b session bugs found + fixed (all caught by probes/payload gate):
  4. xsSyntaxical.c `fxClassExpressionStep` case 11 ran the stock "no heritage"
     base/host fallback on EVERY re-entry — including via case 1 after the
     heritage sub-parse — pushing an extra base NULL onto the node stack. The
     CLASS(6) slots then shifted by one (heritage/init lists filled with
     garbage; crash in fxClassNodeHoistItems). Fixed with a `!f->b0` guard
     matching stock's if/else-if structure. Every heritage class was affected.
  5. xsSyntaxical.c `fxPropertyNameStep` case 1 (plain computed name, no
     modifier) never consumed `]` — stock consumes it via the shared tail
     `fxGetNextToken` — so `["k"+1] = 9` failed with "missing ;". Fixed.
  6. xsCode.c WC_CLASS stage-4 item-loop head dereferenced the cursor before
     re-checking end-of-list; skipped (non-emitting field) items re-enter
     stage 4 with v1==NULL → segfault. Fixed with the stock `while (item)`
     re-check at the head.
  7. xsCode.c WC_CLASS constructorInit epilogue missed stock's FIRST
     GET_LOCAL_1(constructor) before the dispatch (stock emits it both before
     and after) → 2-byte payload deficit and `new: not a constructor` at
     runtime for static-field classes. Fixed.
- B5d sweep findings (all caught by the new probes/payload gate):
  14. **fxCheckStrictBindingStep tail-pop infinite loop** (xsSyntaxical.c) —
      four recursive cases did `fxParserCallNode(...); break;`: the call pushes
      the child frame, then fxParserReturn pops the CHILD (stack top) instead
      of the caller, and the pump re-ran case 0 forever. Any arrow with default
      parameters `(x = 1) => ...` hung. Fixed with `P_RESUME(1)` (the standard
      CALL+resume contract).
  15. **fxArrayBindingStep rest-resume missing `]` match** — stock's rest exit
      breaks to the SHARED tail which still matches `]` before pushing the
      ARRAY_BINDING node; the machine's resume case pushed without it, leaving
      `]` unconsumed ("missing ;" on `let [a, ...rest] = ...`).
  16. **fxObjectBindingStep slot mixups** — the rest-resume (case 5) and the
      empty-`{}` exit ORed `f->u0` (the binding TOKEN) into root->flags
      instead of `f->u1` (the accumulated mxSpreadFlag); case 4 (computed key)
      used fxParserCallFlag for K_BINDING, whose machine reads its token from
      f->t1 (CallTokenFlag layout) — the token was delivered as FLAGS and t1
      stayed stale. Three one-line fixes.
  17. **fxFromExprRunBase stage-0 delivery wiring** — the item-walker frames
      (kFromArrayExpr, kFromBindingItems) are pushed with their FIRST item
      already pending but gated the deliver-wiring on stage==1, so the first
      conversion was ignored and the (already mutated) item re-converted →
      NULL/corruption on nested destructuring parameters. kFromObjectExpr
      KEEPS the stage==1 gate by design: the driver descends into the raw
      PROPERTY node there (no driver case), so its first deliver is C_NULL
      and the frame's property scan is what descends into the value.
  18. **WC_BINDING_ASSIGN tail-pop** (xsCode.c) — the machine's final Assign
      dispatch was followed by its own pop; while running, that pops the
      freshly pushed CHILD and the machine re-runs its default case forever.
      Fixed with the pop-before-epilogue rule (pop self first, then tail-
      dispatch through locals). Every new machine now dispatches in an earlier
      stage than it pops.
- B5c fixes + findings (all caught by probes/payload gate):
  8. **Running-clear rule** — a plain-code helper called from a machine stage
     (nodeWalkRunning=1) that internally calls fxNodeDispatch* would park its
     child frames; the pump then resumed them LIFO *after* the whole parent
     frame — wrong order vs stock's inline emission. Every such site wraps the
     helper with save/clear/restore of nodeWalkRunning (fxScopeCodeDefineNodes
     at all 10 machine-internal sites; fxSpreadNodeCode in WC_PARAMS).
     Dispatch-free helpers (fxScopeCoding*, fxScopeCodeRetrieve/Store/Used/
     Using/UsedReverse/Refresh/Reset/Coded/CodedBody/SpecifierNodes,
     fxGenerateTag) need no wrap.
  9. **WC_CATCH rewrite** — old machine dispatched the catch Reference with
     stage still 0 (infinite re-dispatch when it parked) and skipped the
     statement after the Assign; rewritten with a stage bump before every
     dispatch, mirroring stock's fxCatchNodeCode including the using-protocol
     bookkeeping (exception/selector/catchTarget in frame slots).
  10. **fxScopeCodeUsingStatement is a whole-statement ENCODER** — its else
     branch dispatches the statement itself; calling it from a machine (plus
     the machine's own statement dispatch) double-emits. Must NOT be called
     from machines; stock folds the using protocol into the catch machine's
     stages.
  11. **WC_FORINFOROF stage bump** — same parked-loop hazard as WC_CATCH;
     Reference dispatch in stage 1 now bumps the stage first.
  12. **v-slot pointer rule** — with -Dmx32bitID=1 txInteger is 32-bit, so
     i-slots truncate pointers; frame slots holding node/target pointers MUST
     use the v-slots (txNodeWalkFrame splits int/pointer storage).
  13. **Root-entry shims** — fxParserCode calls (*dispatch->code)(root) DIRECTLY
     (bypasses fxNodeDispatchCode), so fxProgramNodeCode/fxModuleNodeCode carry
     XS_NR_ROOT_SHIM: if not running, push a root frame + drain + return;
     otherwise fall into the native body (harmless for nested dispatches).
  Module-side machines: WC_INCLUDE (single body dispatch), WC_IMPORT_CALL
  (expression + withExpression + importFlag/IMPORT), WC_MODULE (4-stage
  half-machine emitting prelude/main/epilogue with count + MODULE flag +
  SET_RESULT, using-context carried in slots), WC_PROGRAM (scopes + DefineNodes
  + body + returnTarget/RETURN). Park-list extended to all B5c kinds.
- Known limitation (pre-existing, documented): `fxParserCallClassFlag` writes
  `*theSymbol = parser->outSymbol` even for nested (pump-deferred) calls, where
  outSymbol is still the previous frame's value; the deferred frame's kind
  doesn't carry the target pointer. Not exercised by any current battery probe
  (out-param callers are top-level statement/export paths); fix = store the
  target pointer in an unused frame slot and write it in fxParserReturn.
- B5d hardening results (2026-10-01):
  - New probes t_bind/t_pbind/t_delg/t_thisv/t_cname/t_rgx cover every
    converted kind; all payload-exact + runtime-exact.
  - 269KB bundle (many.js): payload-hash parity + runtime parity.
  - Deep-AST compile survival (20k nested parens + 20k-term binary chain):
    fork parses/emits/runs OK; stock SEGFAULTS (C-stack recursion) — the R7
    deliverable, mirroring the R10 module-chain proof.
  - ASan battery (44 probes incl. bundle): ZERO AddressSanitizer reports on
    the fork; t_revive_deep crashes IDENTICALLY on both engines (known
    environmental); stock's deepast crash is its own.
- Gold standard (stronger than emit-trace): FNV hash of script->codeBuffer +
  symbolsBuffer at `return script;` in fxParserCode (spliced by
  /tmp/nrharness/emit_splice.py into BOTH fork and stock, stock built with
  matching -Dmx32bitID=1). **PROG 44/44 + MODULE 6/6 payload-exact**;
  RUN-MATCH 6/6 modules, 44/44 programs; parser regression suite 30/30;
  battery 449/456 (7 fails = the same deep-recursion abort class with rc
  parity: fork prints its ABORT banner, stock exits silently; deeper.js stock
  segfaults rc=139). t_revive_deep SIGBUSes on BOTH DISP engines identically
  (environmental; passes RUN-MATCH on plain builds).
- Remaining (optional): flip `gxCodeStepMachineEnabled` if such a master
  switch is ever wanted — the pump is now the only emission path, so there is
  nothing left to convert.
- Historical plan (kept for reference): convert emitters to stage machines
  body-by-body; the txCoder target/alias machinery is per-frame local state and
  moves into the frame verbatim.

### xsSyntaxical.c (verified clean)
- All fx* functions named like stock recursive-descent entry points (fxBody,
  fxStatement, fxCommaExpression, …) are thin wrappers calling
  fxParserCall* + pump. Dispatcher (748) is the only step-caller. All 6 pump
  helpers drain with `while (parser->curFrame != baseFrame)` and reset
  pumpRunning. JSON steps share the pump (K_JSON_*).
- Cleanup note: `fxParserCallParam` assigns `parser->pumpBase = baseFrame;`
  three times in a row (harmless; dedupe).

### xsMemory.c (verified converted)
- `fxMark` = worklist drain loop (752); `fxMarkWorklistPush/Grow` = heap array
  (c_realloc, lives in machine, freed in fxFree); `fxMarkInstance` =
  pointer-reversal traversal for prototype chains; marker bodies only push.

### xsArray.c / xsTree.c / xsRegExp.c / xsSourceMap.c (verified no live recursion)
- xsArray: `fxSortArrayItems` quicksort uses explicit `txSortPartition
  stack[30]` + insertion sort for small runs (GCC-qsort style).
- xsTree: `fxNodePrintTree` recursion is behind `#ifdef mxTreePrint` which is
  commented out (line 40) — dead in every build. `distribute` callbacks are flat.
- xsRegExp: `split_aux` already flattened to a loop; split host drives it.
- xsSourceMap: uses fxJSONValue pump wrapper (iterative).

---

## Findings

### xsre.c (11,897 lines — regex engine)

**R1. Grammar recursion — `fxDisjunctionParse` ↔ `fxSequenceParse` — INPUT-SCALED → **CONVERTED** (frame machine; see conversion notes below)**
- `fxDisjunctionParse` (1510): calls `fxSequenceParse`, then right-recurses once per `|`.
- `fxSequenceParse` (1674): loop body calls `fxDisjunctionParse` for every
  `(`-group: `(?:` (1753), `(?=` (1763), `(?!` (1773), `(?<=` (1782), `(?<!`
  (1792), `(?<name>` (1808), plain `(` (1820); and calls `fxModifiersParse` (via
  `(?i:` → 1595 → `fxDisjunctionParse(')')`).
- Depth ∝ pattern nesting (page-controlled). One C frame pair per nesting level.

**R2. v-mode charset recursion — `fxCharSetExpression` ↔ `fxCharSetOperand` — INPUT-SCALED → **CONVERTED** (same frame machine, P_CHARCLASS_EXPR/OPERAND kinds)**
- `fxCharSetExpression` (832) calls `fxCharSetOperand` (846/852/868/884/895);
  `fxCharSetOperand` (938) calls `fxCharSetExpression` on nested `[` (946).
- `fxCharSetParseList` (1064) → `fxCharSetParseItem` (1034) → `fxCharSetParseEscape`
  — all leaf-iterative, no recursion back. Only the Expression↔Operand pair recurses.
- Also entered from `fxSequenceParse` `[`-case (1835/1839) — part of the same graph.

**R3. `fxModifiersParse` (1583) — INPUT-SCALED (tail of R1) → **CONVERTED** (P_MODIFIERS frame kind)**
- Flag loops are iterative; calls `fxDisjunctionParse(parser, ')')` at 1595.
- Wrapped by sequence R-action "modifiers group"; same graph as R1.

**R4. Measure/Code walkers — CONVERTED (previous session), cleanup TODO**
- `fxRegExpMeasurePostOrder` (2064) / `fxRegExpCodePostOrder` (2380): heap
  `sxRegExpItem` worklists, two-phase PRE/POST, revisit marking. Correct.
- Cleanup items found while reading:
  - a) Dead native child-call branches remain in the `*Measure`/`*Code` bodies
    (`if (!parser->walkerRunning) { ...child dispatch... }`) — unreachable via the
    walkers but still present (and still call `fxPatternParserCheckStack`). Remove.
  - b) `walkerRunning` flag + re-entrancy wrappers (`fxSequenceMeasure`,
    `fxSequenceCode`, `fxModifiersMeasure` nested sub-walk) exist only to serve the
    dead branches. After (a), remove the flag entirely; integrate `(?i:)`
    modifiers-disjunction as a normal walker child (flags save/restore in the item).
  - c) Worklist items are freed only by the final drain in `fxCompileRegExp`; a
    nested modifiers sub-walk abandons the outer chain (leak), and the code walker's
    items are never freed inline either. Free each item on pop; also free
    `parser->walkerStack` on the longjmp path in `fxCompileRegExp`.

**Executor: NONE.** `fxMatchRegExp` (3037) is the stock threaded-code VM:
explicit `step = *pointer` chaining, heap `txStateData` backtrack stack
(`fxPushState`/`fxPopStates`). Layout requirement confirmed while reading: children
own disjoint bytecode ranges addressed via their own `step`/`completion`/`loop`
fields; the walker must allocate a node's range before its subtree's (post-order per
node) — current walker already does.

**Leaves verified iterative while reading:** `fxCaptureNameParse` (1446, while
loop), `fxCharSetStrings` (\q{}, two linear passes), `fxCharSetStringsDisjunction` /
`fxCharSetStringsSequence` (linear chain building), `fxQuantifierParseBrace` /
`fxQuantifierParseDigits` (linear), `fxPatternParserEscape` / `fxPatternParserNext`
/ `fxPatternParserDecimal` (leaf).

### Conversion plan for R1–R3 (approved approach — same pattern as xsScope.c walk)

- Add `sxPatternFrame` (heap chunk via `fxPatternParserCreateChunk` → freed by
  `fxPatternParserTerminate` on success AND longjmp paths — longjmp-safe by design)
  + `parser->parseStack` field in `sxPatternParser` (zeroed by Initialize).
- Frame kinds: `P_DISJUNCTION` (terminator char, named-capture splice state),
  `P_SEQUENCE` (terminator, sequence-chain state, resume-action code, saved
  captureIndex/current), `P_CHARCLASS_EXPR` (op mode, left/right/kind/result),
  `P_CHARCLASS_OPERAND` (nested-`[` resume), `P_MODIFIERS` (flags add/remove).
- Driver `fxPatternParseRun(parser, terminator)`: push initial frame; while stack:
  step top frame; finishing frame pops and delivers its result via
  `parser->lastResult` to its parent (mirrors fxNodeWalk pattern). `C` depth constant.
- `fxCompileRegExp` calls `fxPatternParseRun` instead of `fxDisjunctionParse`
  (both the plain and the N-flag re-parse).
- Semantics to preserve exactly (verified from bodies): stock builds sequence chains
  left-fold with `formerBranch` patching; disjunction named-capture splice order;
  quantifier index tags (`currentIndex`, `currentIndex-1` for captures); modifiers
  parse consumes its own `)`; charset `--`/`&&` right-fold loops; `-` range
  leftKind/rightKind guard; `[`-class expects `]` then Next.

---

### CONVERSION NOTES — R1/R2/R3 regex grammar frame machine (session of 2026-09-29)

All three grammar recursions now run on one explicit frame stack driven by
`fxPatternParseRun(parser, terminator)` (xsre.c). Design that SHIPPED (verified by
104-file battery + 63-pattern parity sweep vs stock + 502KB bundle parity):

- Frame kinds: `P_DISJUNCTION`, `P_SEQUENCE`, `P_MODIFIERS`, `P_CHARCLASS_EXPR`,
  `P_CHARCLASS_OPERAND`. `sxPatternFrame` carries kind/stage/character + all
  grammar state (result/left/former/currentBranch/currentIndex/action for
  sequences; leftNamedCaptureAddress/leftNamedCapture for the alternation splice;
  mode/not/leftSet/rightSet/leftKind/rightKind/string for class expressions).
- Frames are chunk-allocated via `fxPatternParserCreateChunk` → freed by
  `fxPatternParserTerminate` on BOTH success and longjmp paths;
  `fxPatternParseFreeStack` (called in the `fxCompileRegExp` longjmp branch) just
  resets `parseStack`/`parseResult` so nothing stale can resume.
- Stock call-order invariants reproduced exactly:
  - disjunction `|` fold stashes the sequence result in `f->result` BEFORE pushing
    the right child (the child clobbers `parser->parseResult`); named-capture
    splice happens AFTER building the txDisjunction (stock order);
  - group/capture/plain-`(?:` resume paths consume `)` and double-read
    `parseResult` through a local (fixed vs the folded-backup version);
  - capture groups quantifier `currentIndex-1`, others `currentIndex`;
  - lookahead `=`/`!` are consumed by the SEQUENCE step before pushing the child;
    assertions and modifiers groups get NO quantifier applied (stock errors on a
    trailing `?`/`+` after them — parity preserved);
  - class kinds travel via the child→parent `action` slot; `--`/`&&` folds store
    the running set in BOTH `leftSet` and `result` (the close path returns
    `result` — storing only leftSet segfaulted on `\d--\w`).
- `fxCharSetOperand` keeps its FULL stock body (incl. the nested-`[` branch, which
  recurses through `fxCharSetExpression` → `fxPatternParseRun`, still heap-
  bounded); the frame machine's `P_CHARCLASS_OPERAND` step uses it for leaves and
  handles `[` itself via an EXPR child. The Expr step NEVER parses operands
  itself — it always delegates to an OPERAND frame (this was the bug that made
  `[[a-z][0-9]]` fail).
- Files touched: xsre.c only (+ one latent INPUT-SCALED-severity bug fix in
  xsScope.c fxScopeLookup, see below). Harness trace instrumentation
  (XSRE_TRACE env push/pop log) was added and REMOVED after debugging.

### Latent bug found + fixed: xsScope.c fxScopeLookup (iterative rewrite)
The iterative closure-node chain built every FUNCTION scope's closure node
unconditionally; stock checks `access->declaration` after each recursion level and
stops the chain when a PROGRAM-level VAR/DEFINE resolution nulls it. Without the
`if (access->declaration)` guard, `function f(){ return f(); }` (self-reference
from an inner function) dereferenced NULL in the BIND phase → hard crash. Fixed
with the stock-equivalent guard inside the reversal loop.

### Regression damage recovered in the same session (context for compaction)
An earlier edit had silently deleted, from xsre.c, the top-of-file declaration
blocks (measure/code walker fwd decls, pattern-parser primitive decls), the
`mxCodeSize`/step-size defines + error enum + `gxErrors[]` table, the MATCH data
typedefs (txAssertionData/txCaptureData/txQuantifierData/sxStateData), the
`gxLineCharacters`/`gxWordCharacters` tables, `fxCharCaseCanonicalize` +
`fxCharCaseCompare` bodies, and the `walkerStack` struct field — plus the
machinery definitions themselves (ParseRun/FramePush/FramePop/FreeStack and the
disjunction/modifiers/operand steps). All were restored from the vendored stock
`Source/js/xs_moddable/sources/xsre.c` and the /tmp/old_sequence.txt backup, then
verified green. If similar compile errors appear, diff fork vs stock top-of-file
first (lines ~28-520).

---

## CONVERSION QUEUE (priority order — everything else verified clean)

| ID | Target | Why | Plan |
|----|--------|-----|------|
| R1+R3 | xsre.c fxDisjunctionParse/fxSequenceParse/fxModifiersParse | page-controlled pattern depth, run on game-task stack | **DONE** — heap frame stack + step machine (fxPatternParseRun); frames via fxPatternParserCreateChunk (freed by Terminate on success AND longjmp) |
| R2 | xsre.c fxCharSetExpression ↔ fxCharSetOperand | v-mode charset nesting, same stack | **DONE** — frame kinds in same parse stack |
| R4-c | xsre.c walker cleanup | dead native branches + leak on nested modifiers walk + free-on-pop | **DONE** — `!walkerRunning` branches removed, flag dropped, items freed on pop, walkerStack freed on longjmp path |
| R5 | xsJSON.c fxReviveStack exception safety + hook deregistration | reviver throw leaves stale frames + dangling the->jsonWalkFree | **DONE 2026-09-29**: mxTry/mxCatch around revive + parse phases, fxReviveFreeAll in catch, hooks cleared on both paths; per-invocation pump boundary (base = C local); 6/6 runtime parity vs stock |
| R11 | xsMarshall.c fx{Marshall,Measure,Demarshall}Slot | postMessage/structured transfer, UNGUARDED today — hard-crash risk | **DONE 2026-09-30** — heap frame-stack pumps (sxMarshallFrame ×3 pumps); exact stock parity incl. quirks; survives 64KB game-task stack where stock segfaults |
| R7 | xsCode.c ~80 native code emitters | biggest compile-phase item; currently bounded only by fxCheckParserStack (clean abort, not heap) | **CONVERTED 100% 2026-10-01 (B5d)** — ALL 79 gxCodeSteps rows (79 distinct WC_ kinds) in stage machines; zero native emitters left; 18 session bugs fixed total (B5d: 5 parser-pump bugs incl. arrow-default hang + array-rest desync + FromExpr stage-0 wiring, 1 machine tail-pop); gold standard = payload-hash PROG 44/44 + MODULE 6/6 EXACT, parser regression 30/30, battery 449/456 (7 deep-crash rc-parity); 269KB bundle parity; deep-AST 20k: fork OK / stock segfault; ASan clean. Remaining (optional): flip gxCodeStepMachineEnabled |
| R10/R10b | xsModule.c fxLinkCircularities + fxLinkTransfer | module-graph depth (export * from chains) | **DONE 2026-09-30** — run-stack pump (Circularities), tail-loop (LinkTransfer), worklist (fxOrderModule, R10c); exact parity; fork survives 2000-deep re-export chain at 64KB where stock segfaults |
| R9 | xsRun.c fxRunScript/fxRunForAwaitOf/fxRunUsed/fxRunEval | one C frame per native re-entry (eval-of-eval) | **DONE 2026-09-30** — fxCheckCStackSoft (catchable RangeError, 8KB margin before hard aborts) at all 4 entries; fork catchable @60KB real limit where stock aborts status=9; battery 321/322 (deeper.js known) |
| R6 | xsJSON.c fxStringifyJSONProperty | page-controlled object depth at runtime | **DONE 2026-09-30** — run-stack pump (HEAD/ARRAY/OBJ phases, separator-cell via pointer, LEVEL-flag guard); exact parity ×6+; 500-deep @64KB: stock segfaults, fork completes |

Note on ordering: R1–R3+R4-c are DONE and verified (104-file battery + 63-pattern
stock-parity sweep + 502KB bundle parity, 2026-09-29). R5 is DONE and verified
(6-test runtime suite at exact parity vs stock, incl. re-entrancy + deep +
throw-cycle stress; full battery 189/189; 502KB bundle parity holds). R11 is DONE
and verified (18/18 O1+O0+ASan parity vs stock; 301-file battery; 64KB-stack
deep-graph survival vs stock segfault; 2026-09-30). R10/R10b/R10c are DONE and
verified (module load/link harness; functional + deep-chain parity vs stock at
O1/O0/ASan; 2000-deep @64KB: stock segfaults, fork survives; battery 317/318;
2026-09-30). R9 is DONE and verified (soft guard: catchable RangeError at
real 60KB limit where stock hard-aborts; no-op-host parity 321/322; ASan
clean; 2026-09-30). R6 is DONE and verified (stringify run-stack pump;
6/6 exact parity incl. toJSON/replacer/indent/cycle quirks; 500-deep
@64KB: stock segfaults, fork completes; battery 330/331; ASan clean;
2026-09-30). R7 is CONVERTED 100% (B5d complete: ALL 79 gxCodeSteps rows
in stage machines; 269KB bundle parity; deep-AST 20k survival vs
stock segfault; ASan battery clean; PROG 44/44 + MODULE 6/6
payload-hash EXACT vs stock; battery 449/456 with all 7 fails in the
known deep-recursion abort class — rc parity holds; B5b fixed two
stock-parity parser bugs (class case-11 double base-NULL push;
computed property name never consuming `]`); B5d fixed five more
parser-pump bugs (arrow-default hang, array-rest `]` desync, two
object-binding flag/slot corruptions, FromExpr stage-0 wiring) plus
one machine tail-pop; remaining (optional): flip
gxCodeStepMachineEnabled).

R5 verification tooling note: the parse-only driver harness cannot run
revivers; use `/tmp/nrharness/xrun.c` (real machine + fxRunScript + main-frame
jump buffer) built exactly like stress_o2 but linking xrun.c. Runtime tests:
t_json, t_revive_throw, t_revive_reentrant, t_revive_syntaxerr, t_revive_deep,
t_revive_edge (all end in `throw "PASS ..."` since bare XS has no print).

---

## R12/R13 verification sweep (2026-10-01) — simulator + device, post-B5d

Scope: user-mandated sim+device proof (the R7/B5d proof ran only in the /tmp
harness). Full JS forced in the seam. Real-world target: the 502,716B webpack
bundle `main.44a5d502.js` actually served by bryanwandrych.com.

**R12 — SIMULATOR. Found and fixed two real fork bugs that the /tmp harness
could not see (both in the R7 xsCode.c machines, both only reachable through
the sim's bridge → device-facing machine sizing + prefix concatenation):**

1. **WC_SWITCH resume loop missing target creation** (`wcswitch_case_loop`,
   ~L1500): the stage-2 resume after a case-expression suspension never
   created `caseNode->target` for cases first visited after the suspension →
   `fxCoderAddBranch(..., NULL)` → EXC_BAD_ACCESS. Fix: create the target at
   the top of the resume loop (safe: the loop starts after the suspension
   point). Probes: t_switch.js + t_sw_a1–a4/b/c (a3/a4 crashed before the fix).
2. **WC_LABEL advance-rewire destroyed multi-label chains** (~L1000): stock
   `fxLabelNodeCode` REBINDS `self` while rewiring (`former->nextLabel = self;
   self = former;`), building anon→t→e; the machine wired every
   `former->nextLabel` to the fixed outermost node → `continue t` on
   `e:t:for(...)` failed label lookup ("invalid continue"). Fix: mirror
   stock's rebind walk with a local cursor. Minimal repro: /tmp/nrharness/
   t_dbl.js. (Also added a 64-guard cap to the first dup-label walk.)

Post-fix sim: bundle compiles + executes on engine=4; contained
"JavaScript stack overflow" = stock parity at the bridge sizing (abort
status 2 identical); ran=1 errs=1; heartbeats steady.

**R13 — DEVICE. Ten instrumented rounds (phase markers, pump/lexer/statement/
fromexpr ticks, byte-precise streamOffset, alloc-layer telemetry incl.
SDK-malloc brackets, auto-lock disable, 100KB input bisect). Result: NOT a
fork logic bug.** The fork lexer is byte-identical to stock (diff-verified);
pump/lexer ticks were healthy; the compile "hang" point drifted (131K→123K→
92K→67K input bytes) with ALLOCATION HISTORY, not input position. Root cause:
**platform memory wall** — the XS parser needs ~14B of parser memory per
source byte; compiling ~500KB alongside the live machine drives the SDK
allocator past its collapse point (crashlog heap 6.25–6.55MB, faults inside
malloc/stack-region BEFORE any engine error path could run). Stock XS's
recursive parser needs the same order of memory and dies identically.

Product fixes (kept, in the fork's own files):
- `fxNRParserTotalCap` (xsScript.c, fxNewParserChunk funnel; default 0 =
  unlimited; device bridge sets 768KB at engine boot) → oversized parses fail
  with the stock-contained `fxReportMemoryError` "script too large (parser
  memory cap)".
- Device pre-parse input cap (jsbridge_xs_nr.c, TARGET_PLAYDATE, 96KB): giant
  page scripts are skipped BEFORE any parse allocation, logged, page
  continues. Verified on hardware: contained skip, steady 30fps, empty logs.

Confounder documented: the app never disables Playdate auto-lock — unattended
parses hit OS auto-suspend at ~40s (silence that mimics a hang). PRODUCT NOTE:
consider `setAutoLockDisabled(1)` only while a page load is in flight.

Cleanup: all R13 TEMP instrumentation removed (residual grep clean; fork
xsLexical.c byte-identical to stock again; [xsnr-t] markers removed from the
final build). Post-everything verification: harness EMIT 54/54 payload-hash
EXACT vs stock, 9/9 runtime probes fork==stock (incl. both R12 crash cases),
bridge-sizing 502KB parse abort-parity, clean-release sim + device both clean
(device left running the release; pdex.bin MD5 746c6888…, 0 seam strings).

### R14 addendum (same day): why a 500KB bundle cannot run on-device, and the path that works

Measured with a heap probe at compile start (progressive OS-malloc test +
pluto_mem_dump_live): the OS could still hand out ONE contiguous 4MB block
(fragmentation refuted; funnel-tracked live ~2.0MB). The parse (~1.3MB) plus
the EMITTED BYTECODE (~2.5MB for this bundle) plus machine growth then drove
OS heap to 7,341,024B — genuine pool exhaustion. Conclusion: a ~500KB React
bundle cannot execute on-device in any engine; this is capacity, not the
fork. The fork's core promise held all day: C-stack peak 0B through the full
502KB compile+run on the sim, and with XS_STACK_COUNT 4096 the JS value-stack
overflow disappears (React then stops on a missing browser API — a shim gap,
not a stack failure). Proven path for the site: static HTML fallback inside
`<div id="root">` renders through the full Playdate pipeline on engine=4
while the giant script is containedly skipped. Site-side one-line action:
put the portfolio content in the root div of public/index.html.

## R15 (2026-10-01): streaming compile + OS-truth admission (device-verified)
- Disk-streaming parse: new run_script_stream engine hook; XS NR parses straight off a spill handle through a 1KB sliding-window getter (2-char lookahead safe; no fork engine changes — fxParseScript takes the getter as a parameter). 502KB bundle verified compiling from disk on the real site in sim: output parity with the RAM path.
- Admission: pluto_mem_probe_grantable (OS-truth malloc ladder) vs 14B/B + 512KB estimate (R13 measured rate). Fixed input caps removed. Denial defers (retry per frame); impossible scripts fail fast with measured numbers.
- Zero-copy fetch: identity-encoding script fetches + onSuccessSpill handle adoption (no RAM body at any point in the disk path).
- Device verdict recorded: 502KB parse tree ≈ 7.0MB > 6.5MB budget — not runnable on-device in any engine; contained fail-fast instead of allocator death. C-stack still 0B peak throughout.

## R16 (2026-10-02): chunked-parse prototype + fxNewParserChunk recursion fix
- ENGINE FIX (shipped, xsScript.c fxNewParserChunk): the R13 parser cap could recurse
  infinitely — fxReportMemoryError → fxNewParserString → fxNewParserChunk → cap fires
  again → stack overflow (SIGSEGV, uncontainable). Fix: fire the error only when
  parser->errorCount == 0; the error path's own message allocations bypass the cap so
  the longjmp completes. Device relevance: statement-heavy files (real 58–80B/B vs the
  14B/B admission estimate) passed R15 admission, then hit the 768KB cap mid-parse —
  OLD: hard crash; NEW: contained RangeError. Verified on the 502KB monolith at cap 4MB
  and 768KB: "RangeError: script too large (parser memory cap)", contained.
- Chunked-parse prototype (/tmp/nrharness chunk_scan.c + xrt3.c): streaming top-level
  statement splitter (byte-fed, O(1) state, 42/42 self-test incl. 1KB sliding windows)
  + segment-sequential compile/run on ONE machine. Engine facts proven: fxRunScript
  frees the script's codeBuffer on completion (xsRun.c:5125) and XS_CODE_CODE_* copies
  function bodies into machine chunks (xsRun.c:2943) — so peak parser memory = largest
  segment. many.js: 15.6MB (single) → 0.64MB (8KB segments), output parity. React
  bundle: one 502KB top-level IIFE statement — chunking cannot help it (16.25MB peak
  uncapped, 32B/B); needs lazy function-body compile (unscheduled).
- Hazards (probed): cross-segment use-before-declaration (function/var) throws
  ReferenceError in the consuming segment; strict-mode compile errors fragment (earlier
  segments run first). Parity holds for globals, closures, cross-segment let/const
  (XS NR persists the global lexical env across fxRunScript), templates, regex.
- R16 INTEGRATION (shipped, jsbridge_xs_nr.c): the harness splitter now lives in the
  bridge (xcs_* two-pass streaming scanner + planner). Device: when a streamed script's
  whole-file compile need is not grantable, the chunked path measures (pass 1), probes
  admission ATOMICALLY on the largest packed 8KB segment, then compiles+runs segments
  (pass 2), stopping containedly on the first failing segment. Sim-verified: 269KB
  statement-heavy file → 33 segments, exact semantic check (t=200989999), ran=1 errs=0,
  heap 1335KB vs 15699KB whole-file (92% less). Whole-file path unchanged.
  Integration bring-up found two memory-contract bugs beyond the harness logic:
  (1) xcs_peek behind the sliding window indexes window[negative] — segment/directive/
  prologue copies now use position-independent pluto_spill_read (xcs_copy_bytes);
  (2) fxParseScript(the, stream, fxStringCGetter) REQUIRES txStringCStream{buffer,
  offset,size} — passing a raw char* made the getter read the script TEXT as the struct
  (garbage pointer → SIGSEGV in fxStringCGetter, diagnosed from a crashpad minidump).
  The same latent bug existed in xs_run_script (RAM inline path) since before R15; both
  call sites now build the stream struct like xs_run_tiny always did. Lesson: the
  no-recursion fork's C-string source API is struct-typed, not pointer-typed — any new
  caller of fxStringCGetter must pass &txStringCStream.

## R17 (2026-10-02) — real-site CSR audit: bryanwandrych.com device wall quantified
- Live-site ladder (every engine, live fetches): muJS + Duktape fail PARSE (ES5 —
  modern syntax; vendored stock, not fixable in our code). QuickJS parses (502,716B →
  2,254,355B bytecode) but the device one-shot compile is walled by the OS game-task
  stack: the parser path is NOT stack-probed (CONFIG_STACK_CHECK covers the interpreter,
  not js_parse recursion), so the bridge's compile-safety scan is the only guard. XS NR
  is walled by the parser-memory cap (~8-10KB source per compile at the measured 80B/B).
- CRASH POST-MORTEM (device, 2026-10-02 15:10): admission depth 12→24 + Makefile
  STACK_SIZE 61800→131072 → bus fault mid-compile. STACK_SIZE is vestigial for pdex
  games (link_map.ld has no stack section; nothing consumes -D__STACK_SIZE — the OS
  scheduler owns the ~61.8KB game-task stack). Depth 24 ≈ 60KB parse demand > 61.8KB
  real stack ⇒ overrun. Reverted to 12/61800; lesson written at both sites. SW4's
  depth-19 crash was the same wall seen from the other side.
- What the wall means: a 500KB single-IIFE minified bundle parses at ~depth 24 — beyond
  one-shot device compile on EVERY engine. Statement-SEQUENCE bundles remain fully
  covered by the R16 chunked path (device, automatic). The general device route for
  monolith-class bundles is splitting at webpack's module-map boundaries (per-module
  function bodies, each ≤8KB → exactly the R16 chunk shape) — either bundler-side
  (splitChunks.maxSize=8000; chunks become ≤8KB statement sequences the R16 path
  already runs) or browser-side R18 (extract module-map entries on device during the
  chunked scan). No engine edits required on either route.
- The R17 browser-API surface (history/URL/anchor getters/getComputedStyle/DOM ctors/
  head-in-DOM/title setter) is QuickJS-bridge-side only; porting the same category list
  to the XS NR bridge is required for React-class sites that DO chunk on device.

## R18 audit (2026-10-02) — CORRECTION of the R17 stack claim

R17's note above ("the parser path is NOT stack-probed (CONFIG_STACK_CHECK
covers the interpreter...)") is WRONG for this vendored engine and was the
basis for keeping the depth-12 C-scanner gate on device. Verified from source
and by experiment (qexp, exact device geometry: 64KB OS thread, 40KB
JS_SetMaxStackSize):

- next_token calls js_check_stack_overflow per token (quickjs.c:22719) —
  PARSER recursion IS probed.
- Every interpreter call path probes (js_call_c_function:17606,
  JS_CallInternal:17863) — JS-to-JS recursion IS probed.
- The probe anchors to rt->stack_top captured at JS_NewRuntime2 INSIDE the
  game task (JS_UpdateStackTop), so stack_limit = task_sp - budget is a true
  device guarantee.

Experiment: at a 4KB budget every deep module of the real 502KB bundle fails
CONTAINEDLY ("SyntaxError: stack overflow", 95/95, zero crashes); at 40KB all
units plus the 253KB tail parse and run with peak heap ~3.94MB. The SW4
depth-24 bus fault is therefore re-attributed: the UNPROBED recursion was OUR
C compile-safety scanner (~2.5KB stack per level, jsbridge.c), not QuickJS.

Actions taken: the QuickJS device bridge no longer runs the C scanner
(engine probe suffices and is exact); the bundler-monolith splitter
(jsbridge_bundler.c, R18) removes the need for deep one-shot parses of
bundler output anyway; XS NR keeps its own parser-memory cap + R16/R18
segmentation. No engine files were modified.
