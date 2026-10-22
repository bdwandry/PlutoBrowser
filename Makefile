HEAP_SIZE      = 16777216
# NOTE (R17): STACK_SIZE does NOT resize the device game-task stack for a
# pdex game — the OS fixes it (~61.8KB; no .S in the link script, nothing
# consumes -D__STACK_SIZE). Kept at the SDK default; deep-parse admission
# is handled by the compile-safety guards instead.
STACK_SIZE     = 61800

PRODUCT = PlutoBrowser.pdx

# Locate the SDK
SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config 2>/dev/null | head -n 1 | cut -c9-)
endif

ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

######
# IMPORTANT: You must add your source folders to VPATH for make to find them
######
VPATH += Source:Source/core:Source/util:Source/html:Source/render:Source/render/decoders:Source/ui:Source/keyboard:Source/js/muJS:Source/js/duktape:Source/js/QuickJS:Source/js/xs_moddable/sources:Source/js/xs_moddable/platforms:Source/js/xs_moddable_no_recursion/sources:Source/js/xs_moddable_no_recursion/platforms

# List C source files here (grows as phases land)
SRC_DEVICE = \
	Source/main.c \
	Source/core/logger.c \
	Source/core/constants.c \
	Source/core/url.c \
	Source/core/netmon.c \
	Source/core/encoding.c \
	Source/core/cookie_jar.c \
	Source/core/storage.c \
	Source/core/http_client.c \
	Source/core/pluto_mem.c \
	Source/core/pluto_spill.c \
	Source/core/pluto_snap.c \
	Source/core/pluto_page.c \
	Source/render/style.c \
	Source/render/link_manager.c \
	Source/render/image_decoder.c \
	Source/render/layout.c \
	Source/render/cloud_layout.c \
	Source/util/json.c \
	Source/render/decoders/scale.c \
	Source/render/decoders/dither.c \
	Source/render/decoders/inflate.c \
	Source/render/decoders/bmp.c \
	Source/render/decoders/gif.c \
	Source/render/decoders/ico.c \
	Source/render/decoders/png.c \
	Source/render/decoders/jpeg.c \
	Source/render/decoders/svg.c \
	Source/render/decoders/tif.c \
	Source/render/decoders/tga.c \
	Source/render/decoders/psd.c \
	Source/render/decoders/sgi.c \
	Source/render/decoders/xbm.c \
	Source/render/decoders/pdfimg.c \
	Source/render/decoders/webp.c \
	Source/render/decoders/webp_vp8_data.c \
	Source/render/decoders/webp_vp8.c \
	Source/render/decoders/webp_container.c \
	Source/render/decoders/avif.c \
	Source/render/decoders/dav1d/src/cdef_apply_tmpl.c \
	Source/render/decoders/dav1d/src/cdef_tmpl.c \
	Source/render/decoders/dav1d/src/cdf.c \
	Source/render/decoders/dav1d/src/cpu.c \
	Source/render/decoders/dav1d/src/ctx.c \
	Source/render/decoders/dav1d/src/data.c \
	Source/render/decoders/dav1d/src/decode.c \
	Source/render/decoders/dav1d/src/dequant_tables.c \
	Source/render/decoders/dav1d/src/fg_apply_tmpl.c \
	Source/render/decoders/dav1d/src/filmgrain_tmpl.c \
	Source/render/decoders/dav1d/src/getbits.c \
	Source/render/decoders/dav1d/src/intra_edge.c \
	Source/render/decoders/dav1d/src/ipred_prepare_tmpl.c \
	Source/render/decoders/dav1d/src/ipred_tmpl.c \
	Source/render/decoders/dav1d/src/itx_1d.c \
	Source/render/decoders/dav1d/src/itx_tmpl.c \
	Source/render/decoders/dav1d/src/lf_apply_tmpl.c \
	Source/render/decoders/dav1d/src/lf_mask.c \
	Source/render/decoders/dav1d/src/lib.c \
	Source/render/decoders/dav1d/src/log.c \
	Source/render/decoders/dav1d/src/loopfilter_tmpl.c \
	Source/render/decoders/dav1d/src/looprestoration_tmpl.c \
	Source/render/decoders/dav1d/src/lr_apply_tmpl.c \
	Source/render/decoders/dav1d/src/mc_tmpl.c \
	Source/render/decoders/dav1d/src/mem.c \
	Source/render/decoders/dav1d/src/msac.c \
	Source/render/decoders/dav1d/src/obu.c \
	Source/render/decoders/dav1d/src/pal.c \
	Source/render/decoders/dav1d/src/picture.c \
	Source/render/decoders/dav1d/src/qm.c \
	Source/render/decoders/dav1d/src/recon_tmpl.c \
	Source/render/decoders/dav1d/src/ref.c \
	Source/render/decoders/dav1d/src/refmvs.c \
	Source/render/decoders/dav1d/src/scan.c \
	Source/render/decoders/dav1d/src/tables.c \
	Source/render/decoders/dav1d/src/thread_task.c \
	Source/render/decoders/dav1d/src/warpmv.c \
	Source/render/decoders/dav1d/src/wedge.c \
	Source/ui/chrome.c \
	Source/ui/hud.c \
	Source/ui/error_page.c \
	Source/ui/home_page.c \
	Source/ui/bookmarks_page.c \
	Source/ui/history_page.c \
	Source/ui/settings_page.c \
	Source/ui/address_bar.c \
	Source/html/entities.c \
	Source/html/tokenizer.c \
	Source/html/dom.c \
	Source/html/document.c \
	Source/html/readability.c \
	Source/html/jsbridge.c \
	Source/html/jsbridge_bundler.c \
	Source/html/jsbridge_mujs.c \
	Source/html/jsbridge_duktape.c \
	Source/html/jsbridge_quickjs.c \
	Source/html/webstorage.c \
	Source/html/jsext.c \
	Source/html/css.c \
	Source/keyboard/keyboard.c \
	Source/core/tasks.c \
	Source/util/strbuf.c \
	Source/util/strutil.c \
	Source/util/pdtimer.c \
	Source/js/muJS/jsarray.c \
	Source/js/muJS/jsboolean.c \
	Source/js/muJS/jsbuiltin.c \
	Source/js/muJS/jscompile.c \
	Source/js/muJS/jsdate.c \
	Source/js/muJS/jsdtoa.c \
	Source/js/muJS/jserror.c \
	Source/js/muJS/jsfunction.c \
	Source/js/muJS/jsgc.c \
	Source/js/muJS/jsintern.c \
	Source/js/muJS/jslex.c \
	Source/js/muJS/jsmath.c \
	Source/js/muJS/jsnumber.c \
	Source/js/muJS/jsobject.c \
	Source/js/muJS/json.c \
	Source/js/muJS/jsparse.c \
	Source/js/muJS/jsproperty.c \
	Source/js/muJS/jsregexp.c \
	Source/js/muJS/jsrepr.c \
	Source/js/muJS/jsrun.c \
	Source/js/muJS/jsstate.c \
	Source/js/muJS/jsstring.c \
	Source/js/muJS/jsvalue.c \
	Source/js/muJS/regexp.c \
	Source/js/muJS/utf.c \
	Source/js/duktape/duktape.c \
	Source/html/qjs_shim_quickjs.c \
	Source/js/xs_moddable/sources/xsAll.c \
	Source/js/xs_moddable/sources/xsAPI.c \
	Source/js/xs_moddable/sources/xsArguments.c \
	Source/js/xs_moddable/sources/xsArray.c \
	Source/js/xs_moddable/sources/xsAtomics.c \
	Source/js/xs_moddable/sources/xsBigInt.c \
	Source/js/xs_moddable/sources/xsBoolean.c \
	Source/js/xs_moddable/sources/xsCode.c \
	Source/js/xs_moddable/sources/xsCommon.c \
	Source/js/xs_moddable/sources/xsDataView.c \
	Source/js/xs_moddable/sources/xsDate.c \
	Source/js/xs_moddable/sources/xsDebug.c \
	Source/js/xs_moddable/sources/xsDefaults.c \
	Source/js/xs_moddable/sources/xsError.c \
	Source/js/xs_moddable/sources/xsFunction.c \
	Source/js/xs_moddable/sources/xsGenerator.c \
	Source/js/xs_moddable/sources/xsGlobal.c \
	Source/js/xs_moddable/sources/xsJSON.c \
	Source/js/xs_moddable/sources/xsLexical.c \
	Source/js/xs_moddable/sources/xsLockdown.c \
	Source/js/xs_moddable/sources/xsMapSet.c \
	Source/js/xs_moddable/sources/xsMarshall.c \
	Source/js/xs_moddable/sources/xsMath.c \
	Source/js/xs_moddable/sources/xsMemory.c \
	Source/js/xs_moddable/sources/xsModule.c \
	Source/js/xs_moddable/sources/xsNumber.c \
	Source/js/xs_moddable/sources/xsObject.c \
	Source/js/xs_moddable/sources/xsPlatforms.c \
	Source/js/xs_moddable/sources/xsProfile.c \
	Source/js/xs_moddable/sources/xsPromise.c \
	Source/js/xs_moddable/sources/xsProperty.c \
	Source/js/xs_moddable/sources/xsProxy.c \
	Source/js/xs_moddable/sources/xsRegExp.c \
	Source/js/xs_moddable/sources/xsRun.c \
	Source/js/xs_moddable/sources/xsScope.c \
	Source/js/xs_moddable/sources/xsScript.c \
	Source/js/xs_moddable/sources/xsSourceMap.c \
	Source/js/xs_moddable/sources/xsString.c \
	Source/js/xs_moddable/sources/xsSymbol.c \
	Source/js/xs_moddable/sources/xsSyntaxical.c \
	Source/js/xs_moddable/sources/xsTree.c \
	Source/js/xs_moddable/sources/xsType.c \
	Source/js/xs_moddable/sources/xsdtoa.c \
	Source/js/xs_moddable/sources/xsre.c \
	Source/js/xs_moddable/sources/xsmc.c \
	Source/html/qjs_shim_libregexp.c \
	Source/html/qjs_shim_libunicode.c \
	Source/html/qjs_shim_cutils.c \
	Source/html/qjs_shim_dtoa.c \
	Source/html/qjs_pthread_stubs.c \

# The simulator compiles all engines in ONE gcc invocation, so per-object -D
# separation is impossible. The sim therefore builds the FORK tree only
# (isolation = fork's xs_nr_rename.h first-include; stock objects are NOT in
# the dylib). The device build compiles per-object with per-object CPFLAGS
# and swaps in the stock tree via SRC_DEVICE (below).
# (SRC_DEVICE above lists the STOCK tree plus the sim-only extras — the fork
# tree is DERIVED from it by the patsubst below, so the two lists can never
# drift apart. jsbridge_xs_nr.c must NOT be patsubst-mapped; it is added to
# the sim SRC separately, and the device build filters it out.)
SRC = $(patsubst Source/js/xs_moddable/%,Source/js/xs_moddable_no_recursion/%,$(SRC_DEVICE)) Source/html/jsbridge_xs_nr.c

# List all user directories here
UINCDIR = Source Source/core Source/util Source/html Source/render Source/render/decoders Source/render/decoders/dav1d Source/render/decoders/dav1d/include Source/ui Source/keyboard Source/js/muJS Source/js/duktape Source/js/QuickJS Source/js/xs_moddable/sources Source/js/xs_moddable/platforms Source/js/xs_moddable_no_recursion/sources Source/js/xs_moddable_no_recursion/platforms

# List all user C define here, like -D_DEBUG=1
UDEFS =

# QuickJS 2026-06-04 (vendored stock under Source/js/QuickJS — never
# modified): CONFIG_VERSION is defined by the build, not the engine. The
# device game-task stack is 61.8KB, so the CONFIG_STACK_CHECK probe bound
# via JS_SetMaxStackSize (jsbridge_quickjs.c) is what keeps deep JS
# recursion from blowing the task stack — never raise it above ~32KB.
#
# SYMBOL COEXISTENCE: QuickJS's public allocator helpers (js_malloc/js_free/
# js_realloc/js_strdup via cutils.h) collide with muJS's internal allocator
# wrappers of the same names. Resolved WITHOUT touching either vendored
# engine: the Source/html/qjs_shim_*.c adapter TUs #define the rename and
# then #include the stock engine sources, so ONLY QuickJS's translation
# units get distinct pluto_qjs_* symbols while muJS keeps its own. Do not
# compile Source/js/QuickJS/*.c directly — always through the shims.
UDEFS += -DCONFIG_VERSION="\"2026-06-04\""

# R26g DEVICE-CRITICAL (2026-10-05): force JS_PTR64 so quickjs.h takes the
# portable struct-JSValue path on the 32-bit ARM target instead of the
# forced JS_NAN_BOXING (quickjs.h:63). The boxing path is the device
# hard-crash mechanism: UNDEFINSTR wild jump during QuickJS's COMPILE of
# bundle spans ≥~1KB (deterministic across 3 runs; spans ≤498B ran fine;
# a 64KB-stack host thread with the funnel compiled identical bytes
# containedly, and JS_NAN_BOXING forced on arm64 segfaults `1+1` — the
# boxing representation itself is broken for this content). JS_PTR64's
# only other consumer is map_hash_pointer's hash variant. Cost: JSValue is
# 16B instead of 8B — engine heap grows ~1.5x, hence the raised
# QJS_MEM_LIMIT in jsbridge_quickjs.c.
UDEFS += -DJS_PTR64

# Route QuickJS's pthread mutex/condvar calls (its Atomics intrinsics + a
# JS_NewClassID guard) to no-op/safe-fail primitives for the single-threaded
# Playdate (Source/html/qjs_pthread_stubs.c). The rename is applied to the
# QuickJS shims AND the stubs TU — never to other code. Atomics.wait needs a
# SharedArrayBuffer (QuickJS exposes no way to create one), so the condvar
# paths can never be entered in practice; they fail safely if ever reached.
UDEFS += -Dpthread_mutex_lock=pluto_qjs_pthread_mutex_lock \
         -Dpthread_mutex_unlock=pluto_qjs_pthread_mutex_unlock \
         -Dpthread_cond_init=pluto_qjs_pthread_cond_init \
         -Dpthread_cond_destroy=pluto_qjs_pthread_cond_destroy \
         -Dpthread_cond_signal=pluto_qjs_pthread_cond_signal \
         -Dpthread_cond_wait=pluto_qjs_pthread_cond_wait \
         -Dpthread_cond_timedwait=pluto_qjs_pthread_cond_timedwait \
         -Dclock_gettime=pluto_qjs_clock_gettime

# ── XS (Moddable) 9.5.0 (vendored stock under Source/js/xs_moddable — never
# modified): compiled per-file exactly like upstream's xst tool
# (xs/makefiles/lin/xst.mk), with Source/html/xs_platform.h routed through
# XS's OWN platform hook (-DINCLUDE_XSPLATFORM -DXSPLATFORM=...). No engine
# source is touched: all adaptation (allocator, abort containment, C-stack
# guard, metering) lives in Source/html/xs_platform.h + jsbridge_xs.c.
#
# No threads (no mxUsePOSIXThreads/mxUseGCCAtomics): single-threaded like
# the device, Atomics.wait throws instead of blocking. mxMetering bounds
# runaway scripts; mx32bitID matches the 32-bit device target.
UDEFS += -DINCLUDE_XSPLATFORM '-DXSPLATFORM="xs_platform.h"'

# ── muJS resource limits (device stack safety) ────────────────────────────────
# The vendored muJS 1.3.10 ships with limits sized for servers, not for a
# 61.8KB game-task stack. Every one of these is guarded by #ifndef in
# Source/js, so they can be tightened WITHOUT touching the vendored engine
# (user rule: Source/js/muJS, Source/js/duktape AND Source/js/QuickJS are
# immutable). The simulator never reproduces the
# overflow (8MB host stack), so these are applied to BOTH sim + device via
# UDEFS — shared build flags are the only way the sim validates the same
# code the device runs.
#
# Device evidence (errorlog 2026-09-16 16:09–16:13, build b2db9147):
# 4x "stack overflow in task gameTask" while loading google.com with
# jsEnabled=2 (Full). Worst chains (measured, arm-none-eabi-gcc -O2 .su):
#   - js_regcompx embeds Reclass cclass[128] (REG_MAXCLASS) ON ITS STACK:
#     33,536B frame for ANY script containing a regex literal; 16 regex
#     literals on google.com = 536KB total demand.
#   - jsparse AST recursion: JS_ASTLIMIT 400 levels x ~1.4KB worst chain
#     (expression ~600B + statement 596B) ≈ 560KB.
#   - regex parse recursion: REG_MAXREC 4096 deep x ~112B parseatom chain.
#   - JS runtime call recursion is bounded ONLY by heap growth (envstack
#     spills to heap) — an unbounded OOM spiral instead of a clean error.
# With the compile-safety gate in jsbridge.c this is layered defense:
# gate first (our code), engine limits second (vendored #ifndef knobs).
UDEFS += -DJS_ASTLIMIT=48 \
         -DJS_ENVLIMIT=64 \
         -DJS_TRYLIMIT=8 \
         -DREG_MAXREC=48 \
         -DREG_MAXCLASS=16

# muJS value-stack: 4096 entries x 16B = 64KB > the whole device stack.
# The engine allocates it from the heap at js_newstate, so only the SIZE
# needs bounding (deep JS recursion spills envstack here; the recursion
# itself is stopped by JS_ENVLIMIT above).
UDEFS += -DJS_STACKSIZE=2048

# ── dav1d 1.5.1 (vendored C-only subset under Source/render/decoders/dav1d
# — upstream files never modified except the hand-written config.h, which
# stands in for the Meson-generated file): the 13 *_tmpl.c DSP files each
# compile for ONE bitdepth, selected by BITDEPTH (upstream builds them 4x;
# our monolithic sim link and flat device SRC list compile them 1x).
# NOTHING else in the tree reads BITDEPTH (verified), so this global
# define is contained to dav1d. 8-bit stills only; 10/12-bit AVIFs are
# refused cleanly by the wrapper (NULL → placeholder card).
UDEFS += -DBITDEPTH=8

# List the user directory to look for the libraries here
ULIBDIR =

# List all user libraries here
ULIBS =

override PDCFLAGS += -k -s

# Device builds: per-object compilation lets BOTH XS engines coexist (the
# fork's xs_nr_rename.h renames every engine symbol to its _nr twin and the
# two bridge TUs keep their helpers static), so the device links the fork
# tree as-is PLUS the stock tree and both bridge TUs. Settings can then
# select XS (Moddable) or XS (No Recursion) on device, matching the sim.
#
# This MUST run before the common.mk include below: the pdex.elf rule's
# prerequisite list ($(OBJS) <- $(SRC)) is expanded at include time, while
# the link recipe expands it lazily — a later SRC mutation would leave the
# prerequisite graph missing the appended stock objects (link fails with
# "No such file" for objects make never planned to compile).
# Goal-aware: the device parts ride for `make device` AND for plain `make`
# (goal-less = the default = the shippable bundle; see the `all` override
# after the common.mk include — the SDK's all either fails the device link
# with undefined fxAbort or assembles no PDX at all).
ifeq ($(MAKECMDGOALS),)
PLUTO_DEVICE_BUILD := 1
else ifneq (,$(filter device all,$(MAKECMDGOALS)))
PLUTO_DEVICE_BUILD := 1
else
PLUTO_DEVICE_BUILD :=
endif
ifeq ($(PLUTO_DEVICE_BUILD),1)
SRC := $(SRC) $(patsubst Source/js/xs_moddable_no_recursion/%,Source/js/xs_moddable/%,$(filter Source/js/xs_moddable_no_recursion/%,$(SRC))) Source/html/jsbridge_xs.c
# dav1d single-thread pthread stubs (device-only: the sim links macOS
# pthreads; a shared TU would collide at link). See dav1d_pthread_shim.c.
SRC := $(SRC) Source/render/decoders/dav1d_pthread_shim.c
# newlib clock() syscall stub (device-only): TEMP CSS instrumentation uses
# clock(); firmware lacks _times. See pluto_times_stub.c.
SRC := $(SRC) Source/core/pluto_times_stub.c
endif

include $(SDK)/C_API/buildsupport/common.mk

# Optional extra flags for the simulator dylib (e.g. make SIMDEFS=-DPLUTO_JS_AUTOTEST)
# Must come after the include: common.mk assigns DYLIB_FLAGS with =.
ifdef SIMDEFS
DYLIB_FLAGS += $(SIMDEFS)
endif

# The simulator's monolithic compile rule doesn't consume UDEFS (the device
# per-object rule does). Mirror ALL of UDEFS to the sim — engine-compat
# renames AND the muJS limits — so both targets compile the exact same code
# (the sim validates device behavior only if the flags match).
DYLIB_FLAGS += $(UDEFS)

# The sim's single-pass dylib cannot hold both XS engines (per-file -D is
# impossible there), so it links the no-recursion fork only. This macro
# drops the stock-XS vtable reference from jsbridge.c in sim builds; the
# device build keeps the stock engine fully linked.
ifndef DEVICEDEFS
ifeq ($(MAKECMDGOALS),simulator)
DYLIB_FLAGS += -DPLUTO_NO_STOCK_XS
endif
endif

# Duktape's stock duk_config.h probes for math functions (fmin/fmax/fmod)
# that macOS's libSystem does not export as separate symbols — link libm
# explicitly for the simulator dylib (the device toolchain links newlib's
# libm by default).
DYLIB_FLAGS += -lm

# Homebrew clang ≥16 turns -Wincompatible-pointer-types into a default
# ERROR, which breaks the sim build inside third-party C (XS-NR fork's
# xsScope.c) without any behavior change. Demote it for the sim dylib
# only — the device gcc merely warns.
DYLIB_FLAGS += -Wno-incompatible-pointer-types

# dav1d aligned allocation, sim side (device side is -DHAVE_MEMALIGN=1 in
# DAV1D_CPFLAGS): macOS has no memalign(3)/<malloc.h> but does have
# posix_memalign. See Source/render/decoders/dav1d/config.h.
DYLIB_FLAGS += -DHAVE_POSIX_MEMALIGN=1

# Optional extra flags for the device binary (mirrors SIMDEFS), e.g.
# make device DEVICEDEFS="-DPLUTO_JSEXT_AUTOTEST" for a self-testing deploy.
ifdef DEVICEDEFS
CPFLAGS += $(DEVICEDEFS)
endif

# Per-function stack-usage reports (build/*.su) — device game-task stack is
# small; P18's watchdog crash was an aggregate eventHandler frame overflow.
# Audit with: sort -t, -k2 -rn build/*.su | head
CPFLAGS += -fstack-usage

# The SDK compile flags emit a per-file assembly listing (-fverbose-asm
# plus -Wa,-ahlms=...) which costs 10+ minutes on the ~60K-line QuickJS
# shim TU (Source/html/qjs_shim_quickjs.c). Override the flags for the
# QuickJS shim objects ONLY: same -O2/-g levels and limits, minus the
# listing emission (the .su stack-usage reports are kept — those are what
# the device stack audits read). Dependency tracking (-MD/-MP/-MF) is
# re-added per-target so incremental builds stay correct.
QJS_CPFLAGS = $(MCFLAGS) $(OPT) -gdwarf-2 -Wall -Wno-unused -Wstrict-prototypes -Wno-unknown-pragmas -Wdouble-promotion -mword-relocations -fno-common -Wstack-usage=8192 -Walloca-larger-than=8192 -ffunction-sections -fdata-sections $(DEFS) -fstack-usage
build/Source/html/qjs_shim_%.o: CPFLAGS = $(QJS_CPFLAGS) -MD -MP -MF $(DEPDIR)/$(@F).d

# Same treatment for the ~100K-line XS engine (Source/js/xs_moddable): skip
# the per-file assembly listing emission (10+ min penalty) but keep the .su
# stack-usage reports for the device stack audits, plus warning noise
# suppression so real diagnostics stay visible. Dependency tracking is
# re-added per-target so incremental builds stay correct.
XS_CPFLAGS = $(MCFLAGS) $(OPT) -gdwarf-2 -Wall -Wno-unused -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-misleading-indentation -Wno-implicit-fallthrough -Wstrict-prototypes -Wno-unknown-pragmas -Wdouble-promotion -mword-relocations -fno-common -Wstack-usage=8192 -Walloca-larger-than=8192 -ffunction-sections -fdata-sections $(DEFS) -fstack-usage
build/Source/js/xs_moddable/sources/%.o: CPFLAGS = $(XS_CPFLAGS) -MD -MP -MF $(DEPDIR)/$(@F).d

# Same treatment for the no-recursion fork (Source/js/xs_moddable_no_recursion):
# identical flags to the stock engine, isolated by the fork's xs_nr_rename.h
# (every engine symbol compiles to its _nr twin, verified zero overlap).
XS_NR_CPFLAGS = $(XS_CPFLAGS)
build/Source/js/xs_moddable_no_recursion/sources/%.o: CPFLAGS = $(XS_NR_CPFLAGS) -MD -MP -MF $(DEPDIR)/$(@F).d

# Same treatment for vendored dav1d (Source/render/decoders/dav1d/src).
# Two dav1d-only additions: -D_POSIX_THREADS exposes newlib's full pthread
# declarations (types + functions) for these objects only — bare metal
# leaves them guarded out by default. Bodies come from
# Source/render/decoders/dav1d_pthread_shim.c (also device-only): real
# pthread_once/attr, link-only noops for everything the n_threads=1 path
# never touches. No other TU is affected, and the simulator build never
# uses this rule (monolithic dylib keeps macOS pthreads). Dependency
# tracking re-added so incremental builds stay correct.
DAV1D_CPFLAGS = $(MCFLAGS) $(OPT) -gdwarf-2 -Wall -Wno-unused -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-misleading-indentation -Wno-implicit-fallthrough -Wstrict-prototypes -Wno-unknown-pragmas -Wdouble-promotion -mword-relocations -fno-common -Wstack-usage=8192 -Walloca-larger-than=8192 -ffunction-sections -fdata-sections $(DEFS) -fstack-usage -D_POSIX_THREADS -DHAVE_MEMALIGN=1 -Dpthread_once=pluto_dav1d_pthread_once -Dpthread_attr_init=pluto_dav1d_pthread_attr_init -Dpthread_attr_destroy=pluto_dav1d_pthread_attr_destroy -Dpthread_attr_setstacksize=pluto_dav1d_pthread_attr_setstacksize -Dpthread_mutex_init=pluto_dav1d_pthread_mutex_init -Dpthread_mutex_destroy=pluto_dav1d_pthread_mutex_destroy -Dpthread_mutex_lock=pluto_dav1d_pthread_mutex_lock -Dpthread_mutex_unlock=pluto_dav1d_pthread_mutex_unlock -Dpthread_cond_init=pluto_dav1d_pthread_cond_init -Dpthread_cond_destroy=pluto_dav1d_pthread_cond_destroy -Dpthread_cond_wait=pluto_dav1d_pthread_cond_wait -Dpthread_cond_signal=pluto_dav1d_pthread_cond_signal -Dpthread_cond_broadcast=pluto_dav1d_pthread_cond_broadcast -Dpthread_create=pluto_dav1d_pthread_create -Dpthread_join=pluto_dav1d_pthread_join
build/Source/render/decoders/dav1d/src/%.o: CPFLAGS = $(DAV1D_CPFLAGS) -MD -MP -MF $(DEPDIR)/$(@F).d
build/Source/render/decoders/dav1d_pthread_shim.o: CPFLAGS = $(DAV1D_CPFLAGS) -MD -MP -MF $(DEPDIR)/$(@F).d
# AV1 DSP loops are compute-bound on the M7 (a 640px still needs ~6s at
# -O2, flirting with the 10s run-loop watchdog under heap pressure); -O3
# buys back margin. Stack frames are unaffected (data arrays dominate, not
# code motion). dav1d sources only — the rest of the tree stays -O2.
build/Source/render/decoders/dav1d/src/%.o: OPT = -O3


# Plain `make` (and explicit `make all`) must end in the SHIPPABLE bundle —
# device binary + simulator dylib in ONE PlutoBrowser.pdx. The goal-less
# default pulled the SDK's `all`, which built both binaries under the
# sim-only source list — the device link died there (undefined fxAbort)
# before assembly could ever run. With the goal-aware SRC append above,
# plain `make` compiles the same blessed source list `make device` uses for
# BOTH binaries (both XS engines ride — exactly the configuration the
# device receipted), then assembles once.
.PHONY: all
all: device_bin simulator_bin
	@$(PDC) $(PDCFLAGS) Source $(PRODUCT)

# `make device` ships a DEVICE-ONLY bundle: after device_bin stages the
# fresh ARM binary, the recipe removes BOTH stale carriers before packing:
# (1) a simulator dylib staged in Source/ by an earlier `make` /
# `make simulator` — the device goal itself never stages one, so a leftover
# from a dual-build WOULD otherwise be packed right back in, and (2) the
# whole product directory — pdc MERGES into an existing PDX and never
# deletes stale files from it, so a previously packed dylib would survive
# inside the bundle (both lanes receipted 2026-10-21). Result: pdex.bin +
# assets and NOTHING the Playdate Simulator can launch. Plain `make` /
# `make all` still stage a fresh dylib and assemble the dual-target
# bundle; `make simulator` keeps the sim-only lab build. Recipe override,
# last wins — same pattern as the `all` override above (the "overriding
# commands" warning is cosmetic).
# R-25a: device optimization knob. The SDK's common.mk bakes OPT = -O2 …
# for the whole tree; these target-scoped assignments override it for the
# `device` and `all` goals and their prerequisites (same pattern as the
# SDK's own `debug: OPT = -O0`). R25 default: -O3 for the device binary —
# the full -O3 device battery receipted clean 2026-10-22 (real site +
# 5-page internal battery, zero crashlog, stack-usage audit ≤ +48B/frame).
# Roll back per build with `make device DEVICEOPT=-O2`. The simulator
# build never reads OPT (clang builds $(SRC) directly), so sim binaries
# are identical either way.
DEVICEOPT ?= -O3
device: OPT = $(DEVICEOPT) -falign-functions=16 -fomit-frame-pointer
all: OPT = $(DEVICEOPT) -falign-functions=16 -fomit-frame-pointer

.PHONY: device
device: device_bin
	rm -f Source/pdex.dylib
	rm -rf $(PRODUCT)
	$(PDC) $(PDCFLAGS) Source $(PRODUCT)
