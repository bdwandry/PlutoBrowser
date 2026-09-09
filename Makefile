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
VPATH += Source:Source/core:Source/util:Source/html:Source/render:Source/render/decoders:Source/ui:Source/keyboard

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
	Source/keyboard/keyboard.c \
	Source/core/tasks.c \
	Source/util/strbuf.c \
	Source/util/strutil.c \
	Source/util/pdtimer.c

# List all user directories here
UINCDIR = Source Source/core Source/util Source/html Source/render Source/render/decoders Source/ui Source/keyboard

# List all user C define here, like -D_DEBUG=1
UDEFS =

# List the user directory to look for the libraries here
ULIBDIR =

# List all user libraries here
ULIBS =

override PDCFLAGS += -k -s

include $(SDK)/C_API/buildsupport/common.mk

# Per-function stack-usage reports (build/*.su) — device game-task stack is
# small; P18's watchdog crash was an aggregate eventHandler frame overflow.
# Audit with: sort -t, -k2 -rn build/*.su | head
CPFLAGS += -fstack-usage
