# PlutoBrowser — 100% C port of CometBrowser for Playdate
# C sources live in src/, pdc input (pdxinfo + assets + built binaries) in Source/.
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
VPATH += src:src/core:src/util:src/html:src/render:src/render/decoders:src/ui

# List C source files here
SRC = \
	src/main.c \
	src/core/constants.c \
	src/core/logger.c \
	src/core/url.c \
	src/core/selftest_url.c \
	src/core/storage.c \
	src/core/cookie_jar.c \
	src/core/selftest_storage.c \
	src/core/tasks.c \
	src/core/selftest_tasks.c \
	src/core/encoding.c \
	src/core/selftest_encoding.c \
	src/core/internal_pages.c \
	src/core/http_client.c \
	src/core/selftest_http.c \
	src/html/entities.c \
	src/html/tokenizer.c \
	src/html/dom.c \
	src/html/document.c \
	src/html/readability.c \
	src/html/selftest_tokenizer.c \
	src/html/selftest_dom.c \
	src/html/selftest_document.c \
	src/html/selftest_readability.c \
	src/render/style.c \
	src/render/selftest_style.c \
	src/render/link_manager.c \
	src/render/selftest_link_manager.c \
	src/render/decoders/dither.c \
	src/render/decoders/scale.c \
	src/render/decoders/selftest_decoders.c \
	src/render/decoders/inflate.c \
	src/render/decoders/selftest_inflate.c \
	src/render/decoders/png.c \
	src/render/decoders/selftest_png.c \
	src/render/decoders/bmp.c \
	src/render/decoders/selftest_bmp.c \
	src/render/decoders/gif.c \
	src/render/decoders/selftest_gif.c \
	src/render/decoders/jpeg.c \
	src/render/decoders/selftest_jpeg.c \
	src/render/decoders/selftest_jpeg_fixtures.c \
 	src/render/decoders/webp_vp8_data.c \
 	src/render/decoders/webp.c \
	src/render/decoders/selftest_webp.c \
	src/render/decoders/selftest_webp_fixtures.c \
	src/render/decoders/ico.c \
	src/render/decoders/selftest_ico.c \
	src/render/decoders/selftest_ico_fixtures.c \
	src/render/decoders/svg.c \
	src/render/decoders/selftest_svg.c \
	src/render/decoders/selftest_svg_fixtures.c \
	src/render/image_decoder.c \
	src/render/selftest_image_decoder.c \
	src/render/cloud_layout.c \
	src/render/selftest_cloud_layout.c \
	src/ui/chrome.c \
	src/ui/hud.c \
	src/ui/selftest_ui.c \
	src/ui/home_page.c \
	src/ui/selftest_home_page.c \
	src/ui/address_bar.c \
	src/ui/selftest_address_bar.c \
	vendor/keyboard/keyboard.c \
	src/util/mem.c \
	src/util/strbuf.c \
	src/util/dynarray.c \
	src/util/strmap.c \
	src/util/luapattern.c \
	src/util/json.c \
	src/util/luanum.c \
	src/util/selftest_util.c \

# All user directories (for #include resolution)
UINCDIR = src vendor/keyboard

include $(SDK)/C_API/buildsupport/common.mk

# Bare-metal: satisfy newlib syscall refs pulled in by stdio (logger's
# vsnprintf etc.) with empty stubs — device code never touches host I/O.
LDFLAGS += --specs=nosys.specs

# Launch the freshly built .pdx in the Playdate Simulator
sim: all
	@echo "Launching $(PRODUCT) in Playdate Simulator..."
	open -a "$(SDK)/bin/Playdate Simulator.app" $(PRODUCT)

.PHONY: sim
