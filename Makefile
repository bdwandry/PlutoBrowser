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
	src/util/mem.c \
	src/util/strbuf.c \
	src/util/dynarray.c \
	src/util/strmap.c \
	src/util/luapattern.c \
	src/util/json.c \
	src/util/luanum.c \
	src/util/selftest_util.c \

# All user directories (for #include resolution)
UINCDIR = src

include $(SDK)/C_API/buildsupport/common.mk

# Launch the freshly built .pdx in the Playdate Simulator
sim: all
	@echo "Launching $(PRODUCT) in Playdate Simulator..."
	open -a "$(SDK)/bin/Playdate Simulator.app" $(PRODUCT)

.PHONY: sim
