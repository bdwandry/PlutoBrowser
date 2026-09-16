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
VPATH += Source:Source/core:Source/util:Source/html:Source/render:Source/render/decoders:Source/ui:Source/keyboard:Source/js

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
	Source/html/jsext.c \
	Source/keyboard/keyboard.c \
	Source/core/tasks.c \
	Source/util/strbuf.c \
	Source/util/strutil.c \
	Source/util/pdtimer.c \
	Source/js/jsarray.c \
	Source/js/jsboolean.c \
	Source/js/jsbuiltin.c \
	Source/js/jscompile.c \
	Source/js/jsdate.c \
	Source/js/jsdtoa.c \
	Source/js/jserror.c \
	Source/js/jsfunction.c \
	Source/js/jsgc.c \
	Source/js/jsintern.c \
	Source/js/jslex.c \
	Source/js/jsmath.c \
	Source/js/jsnumber.c \
	Source/js/jsobject.c \
	Source/js/json.c \
	Source/js/jsparse.c \
	Source/js/jsproperty.c \
	Source/js/jsregexp.c \
	Source/js/jsrepr.c \
	Source/js/jsrun.c \
	Source/js/jsstate.c \
	Source/js/jsstring.c \
	Source/js/jsvalue.c \
	Source/js/regexp.c \
	Source/js/utf.c

# List all user directories here
UINCDIR = Source Source/core Source/util Source/html Source/render Source/render/decoders Source/ui Source/keyboard Source/js

# List all user C define here, like -D_DEBUG=1
UDEFS =

# ── muJS resource limits (device stack safety) ────────────────────────────────
# The vendored muJS 1.3.10 ships with limits sized for servers, not for a
# 61.8KB game-task stack. Every one of these is guarded by #ifndef in
# Source/js, so they can be tightened WITHOUT touching the vendored engine
# (user rule: Source/js is immutable). The simulator never reproduces the
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

# Optional extra flags for the device binary (mirrors SIMDEFS), e.g.
# make device DEVICEDEFS="-DPLUTO_JSEXT_AUTOTEST" for a self-testing deploy.
ifdef DEVICEDEFS
CPFLAGS += $(DEVICEDEFS)
endif

# Per-function stack-usage reports (build/*.su) — device game-task stack is
# small; P18's watchdog crash was an aggregate eventHandler frame overflow.
# Audit with: sort -t, -k2 -rn build/*.su | head
CPFLAGS += -fstack-usage
