# PlutoBrowser — 100% C port of CometBrowser for Playdate
# All project content lives under Source/ (code, vendor, tools, logs, and
# pdc input: pdxinfo + assets). Only build/ stays at the top level.
# Build model follows the SDK convention (see $(SDK)/C_API/buildsupport/common.mk):
#   device:    arm-none-eabi-gcc -> pdex.elf  -> Source/
#   simulator: clang dylib       -> pdex.dylib -> Source/
#   package:   pdc Source PlutoBrowser.pdx

HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = PlutoBrowser.pdx

# Locate the SDK
SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config 2>/dev/null | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
	SDK = /Users/bwandrych/Developer/PlaydateSDK
endif

######
# Source folders (must be in VPATH for make to find them)
######
VPATH += Source:Source/core:Source/util:Source/html:Source/render:Source/render/decoders:Source/ui:Source/vendor/keyboard

# List C source files here
SRC = \
	Source/main.c \
	Source/core/constants.c \
	Source/core/logger.c \
	Source/core/url.c \
	Source/core/selftest_url.c \
	Source/core/storage.c \
	Source/core/cookie_jar.c \
	Source/core/selftest_storage.c \
	Source/core/tasks.c \
	Source/core/selftest_tasks.c \
	Source/core/encoding.c \
	Source/core/selftest_encoding.c \
	Source/core/internal_pages.c \
	Source/core/browser.c \
	Source/core/http_client.c \
 	Source/core/selftest_http.c \
 	Source/core/selftest_browser.c \
	Source/html/entities.c \
	Source/html/tokenizer.c \
	Source/html/dom.c \
	Source/html/document.c \
	Source/html/readability.c \
	Source/html/selftest_tokenizer.c \
	Source/html/selftest_dom.c \
	Source/html/selftest_document.c \
	Source/html/selftest_readability.c \
	Source/render/style.c \
	Source/render/selftest_style.c \
	Source/render/link_manager.c \
	Source/render/selftest_link_manager.c \
	Source/render/decoders/dither.c \
	Source/render/decoders/scale.c \
	Source/render/decoders/selftest_decoders.c \
	Source/render/decoders/inflate.c \
	Source/render/decoders/selftest_inflate.c \
	Source/render/decoders/png.c \
	Source/render/decoders/selftest_png.c \
	Source/render/decoders/bmp.c \
	Source/render/decoders/selftest_bmp.c \
	Source/render/decoders/gif.c \
	Source/render/decoders/selftest_gif.c \
	Source/render/decoders/jpeg.c \
	Source/render/decoders/selftest_jpeg.c \
	Source/render/decoders/selftest_jpeg_fixtures.c \
 	Source/render/decoders/webp_vp8_data.c \
 	Source/render/decoders/webp.c \
	Source/render/decoders/selftest_webp.c \
	Source/render/decoders/selftest_webp_fixtures.c \
	Source/render/decoders/ico.c \
	Source/render/decoders/selftest_ico.c \
	Source/render/decoders/selftest_ico_fixtures.c \
	Source/render/decoders/svg.c \
	Source/render/decoders/selftest_svg.c \
	Source/render/decoders/selftest_svg_fixtures.c \
	Source/render/image_decoder.c \
	Source/render/selftest_image_decoder.c \
	Source/render/cloud_layout.c \
	Source/render/layout.c \
	Source/render/selftest_layout.c \
	Source/render/selftest_cloud_layout.c \
	Source/ui/chrome.c \
	Source/ui/hud.c \
	Source/ui/selftest_ui.c \
	Source/ui/home_page.c \
	Source/ui/selftest_home_page.c \
	Source/ui/address_bar.c \
	Source/ui/selftest_address_bar.c \
	Source/ui/list_core.c \
	Source/ui/error_page.c \
	Source/ui/bookmarks_page.c \
	Source/ui/history_page.c \
	Source/ui/selftest_pages.c \
	Source/ui/settings_page.c \
	Source/ui/selftest_settings.c \
	Source/vendor/keyboard/keyboard.c \
	Source/util/mem.c \
	Source/util/strbuf.c \
	Source/util/dynarray.c \
	Source/util/strmap.c \
	Source/util/luapattern.c \
	Source/util/json.c \
	Source/util/luanum.c \
	Source/util/selftest_util.c \

# All user directories (for #include resolution)
# All user directories (for #include resolution)
UINCDIR = Source Source/vendor/keyboard

include $(SDK)/C_API/buildsupport/common.mk

# Bare-metal: satisfy newlib syscall refs pulled in by stdio (logger's
# vsnprintf etc.) with empty stubs — device code never touches host I/O.
LDFLAGS += --specs=nosys.specs

# Launch the freshly built .pdx in the Playdate Simulator
sim: all
	@echo "Launching $(PRODUCT) in Playdate Simulator..."
	open -a "$(SDK)/bin/Playdate Simulator.app" $(PRODUCT)

.PHONY: sim
