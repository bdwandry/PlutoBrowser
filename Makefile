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
