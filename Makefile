SDK_CANDIDATES := $(PS5_PAYLOAD_SDK) $(CURDIR)/ps5-payload-sdk /opt/ps5-payload-sdk
SDK_MAKEFILE   := $(firstword $(foreach d,$(SDK_CANDIDATES),$(wildcard $(d)/toolchain/prospero.mk)))

ifeq ($(SDK_MAKEFILE),)
$(error PS5 payload SDK not found: set PS5_PAYLOAD_SDK, or install it at ./ps5-payload-sdk or /opt/ps5-payload-sdk)
endif

PS5_PAYLOAD_SDK := $(patsubst %/toolchain/prospero.mk,%,$(SDK_MAKEFILE))
include $(SDK_MAKEFILE)

ifneq ($(wildcard /etc/NIXOS),)
LLVM_CONFIG ?= $(abspath $(CURDIR)/tools/nixos/llvm-config)
export LLVM_CONFIG
# libprosperopkg.so is a .NET assembly; it must load an OpenSSL runtime matching the
# one it was built against (3.5.x). Newer store paths exist but .NET rejects them
# ("No usable version of libssl was found"), so prefer 3.5 explicitly.
NIX_OPENSSL_LIB := $(firstword $(wildcard /nix/store/*openssl-3.5*/lib))
ifeq ($(NIX_OPENSSL_LIB),)
NIX_OPENSSL_LIB := $(firstword $(wildcard /nix/store/*openssl-3*/lib))
endif
ifneq ($(NIX_OPENSSL_LIB),)
LPP_ENV := DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 LD_LIBRARY_PATH=$(NIX_OPENSSL_LIB)
endif
endif

PS5_HOST ?= ps5
PS5_PORT ?= 9021

PYTHON  ?= python3
LPP_LIB ?= tools/lib/libprosperopkg.so
LPP_ENV ?= DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

TITLE_ID    := DRPC00001
CONTENT_ID  := UP9000-DRPC00001_00-DRPC5AAAAAAAAAAA
PKG_TITLE   := dRPC5
PKG_VERSION := 01.00

BUILD := build
DIST  := dist

CFLAGS := -Wall -Werror -g -DTITLE_ID=\"$(TITLE_ID)\"
LDADD  := -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService -lpthread

PKGSRC   := $(BUILD)/bundled_tile_pkg.c
WEBSRC   := $(BUILD)/web_assets.c
CURL_LIB := $(BUILD)/lib/libcurl.so
PAYLOAD_SRCS := src/payload.c src/discord.c src/gateway.c src/presence.c

all: dRPC5.elf

dRPC5.elf: $(PAYLOAD_SRCS) src/bundled_tile_pkg.h src/curl_api.h src/discord.h src/gateway.h src/paths.h src/presence.h src/web.h $(PKGSRC) $(WEBSRC) $(CURL_LIB)
	$(CC) $(CFLAGS) -I$(BUILD) -o $@ $(PAYLOAD_SRCS) $(PKGSRC) $(WEBSRC) -L$(BUILD)/lib -lcurl $(LDADD)

$(CURL_LIB): src/libcurl_stubs.c
	mkdir -p $(BUILD)/lib
	$(CC) $(CFLAGS) -fPIC -c -o $(BUILD)/libcurl_stubs.o $<
	$(LD) --shared -soname libcurl.sprx -o $@ $(BUILD)/libcurl_stubs.o

$(WEBSRC): web/index.html web/vendor/qrcode.js tools/gen_web.py
	$(PYTHON) tools/gen_web.py --html web/index.html --qrcode web/vendor/qrcode.js --out $@

$(PKGSRC): $(DIST)/drpc5-tile.pkg tools/gen_tile_pkg.py
	$(PYTHON) tools/gen_tile_pkg.py --pkg $< --out $@

$(DIST)/drpc5-tile.pkg: $(BUILD)/homebrew/eboot.bin tile/sce_sys/param.json tile/sce_sys/icon0.png tools/mk_tile_pkg.py
	$(LPP_ENV) $(PYTHON) tools/mk_tile_pkg.py \
		--lib $(LPP_LIB) \
		--homebrew $(BUILD)/homebrew \
		--out-pkg $@ \
		--content-id $(CONTENT_ID) \
		--title $(PKG_TITLE) \
		--version $(PKG_VERSION)

$(BUILD)/homebrew/eboot.bin: $(BUILD)/tile-elf tile/sce_sys/param.json tile/sce_sys/icon0.png
	mkdir -p $(BUILD)/homebrew/sce_sys
	cp $(BUILD)/tile-elf $@
	cp tile/sce_sys/param.json $(BUILD)/homebrew/sce_sys/param.json
	cp tile/sce_sys/icon0.png $(BUILD)/homebrew/sce_sys/icon0.png

tile/sce_sys/icon0.png: tools/gen_icon.py
	$(PYTHON) tools/gen_icon.py --out $@

$(BUILD)/tile-elf: $(BUILD)/tile-elf.o src/eboot.x
	$(LD) --static -T src/eboot.x -o $@ $(BUILD)/tile-elf.o

$(BUILD)/tile-elf.o: src/eboot.c
	mkdir -p $(BUILD)
	$(CC) -c $(CFLAGS) -o $@ $<

tile: $(DIST)/drpc5-tile.pkg

deploy: dRPC5.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

clean:
	rm -rf $(BUILD) $(DIST) dRPC5.elf

.PHONY: all tile deploy clean
