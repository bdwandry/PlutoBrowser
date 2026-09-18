HEAP_SIZE      = 16777216
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
VPATH += Source:Source/core:Source/util:Source/html:Source/render:Source/render/decoders:Source/ui:Source/keyboard:Source/js/muJS:Source/js/duktape:Source/js/QuickJS:Source/js/xs_moddable/sources:Source/js/xs_moddable/platforms

# List C source files here (grows as phases land)
SRC = \
	Source/main.c \
	Source/core/logger.c \
	Source/core/constants.c \
	Source/core/url.c \
	Source/core/encoding.c \
	Source/core/cookie_jar.c \
	Source/core/storage.c \
	Source/core/http_client.c \
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
	Source/html/jsbridge_mujs.c \
	Source/html/jsbridge_duktape.c \
	Source/html/jsbridge_quickjs.c \
	Source/html/jsext.c \
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
	Source/html/jsbridge_xs.c \
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
	Source/html/qjs_pthread_stubs.c

# List all user directories here
UINCDIR = Source Source/core Source/util Source/html Source/render Source/render/decoders Source/ui Source/keyboard Source/js/muJS Source/js/duktape Source/js/QuickJS Source/js/xs_moddable/sources Source/js/xs_moddable/platforms

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

# List the user directory to look for the libraries here
ULIBDIR =

# List all user libraries here
ULIBS =

override PDCFLAGS += -k -s

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

# Duktape's stock duk_config.h probes for math functions (fmin/fmax/fmod)
# that macOS's libSystem does not export as separate symbols — link libm
# explicitly for the simulator dylib (the device toolchain links newlib's
# libm by default).
DYLIB_FLAGS += -lm

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
