SDK_CANDIDATES := $(PS5_PAYLOAD_SDK) $(CURDIR)/ps5-payload-sdk /opt/ps5-payload-sdk
SDK_MAKEFILE   := $(firstword $(foreach d,$(SDK_CANDIDATES),$(wildcard $(d)/toolchain/prospero.mk)))

ifeq ($(SDK_MAKEFILE),)
# send-payload is a plain host binary and needs no SDK, so only the targets that
# actually cross-compile require one.
ifneq ($(filter send-payload,$(MAKECMDGOALS)),)
$(warning PS5 payload SDK not found: set PS5_PAYLOAD_SDK, or install it at ./ps5-payload-sdk or /opt/ps5-payload-sdk)
else
$(error PS5 payload SDK not found: set PS5_PAYLOAD_SDK, or install it at ./ps5-payload-sdk or /opt/ps5-payload-sdk)
endif
endif

PS5_PAYLOAD_SDK := $(patsubst %/toolchain/prospero.mk,%,$(SDK_MAKEFILE))
include $(SDK_MAKEFILE)

ifneq ($(wildcard /etc/NIXOS),)
LLVM_CONFIG ?= $(abspath $(CURDIR)/tools/nixos/llvm-config)
export LLVM_CONFIG
# libprosperopkg.so is a .NET assembly; it must load an OpenSSL runtime matching the
# one it was built against (3.5.x). Newer store paths exist but .NET rejects them
# ("No usable version of libssl was found"), so prefer 3.5 explicitly.
# The wildcard also matches -dev outputs, which ship headers and no libssl.so, and
# those sort first. Keep only directories that actually contain the runtime library.
NIX_OPENSSL_DIRS := $(wildcard /nix/store/*openssl-3.5*/lib)
ifeq ($(NIX_OPENSSL_DIRS),)
NIX_OPENSSL_DIRS := $(wildcard /nix/store/*openssl-3*/lib)
endif
NIX_OPENSSL_LIB := $(firstword $(foreach d,$(NIX_OPENSSL_DIRS),$(if $(wildcard $(d)/libssl.so*),$(d))))
ifneq ($(NIX_OPENSSL_LIB),)
LPP_ENV := DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 LD_LIBRARY_PATH=$(NIX_OPENSSL_LIB)
endif
endif

PS5_HOST ?= ps5
PS5_PORT ?= 9021

HOSTCC     ?= cc
HOSTCFLAGS ?= -O2 -Wall -Wextra -Werror
LPP_LIB ?= tools/lib/libprosperopkg.so
LPP_ENV ?= DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

TITLE_ID    := DRPC00001
CONTENT_ID  := UP9000-DRPC00001_00-DRPC5AAAAAAAAAAA
PKG_TITLE   := dRPC5
PKG_VERSION := 01.00

BUILD := build
DIST  := dist

# -Os + section GC keeps the payload small; -g0 drops debug info, which was
# ~130KB of the deployed ELF. Override REL for a debug build: make REL=1
REL ?= 0
ifeq ($(REL),1)
OPT := -O0 -g
else
OPT := -Os -g0 -ffunction-sections -fdata-sections
endif

CFLAGS := -Wall -Werror $(OPT) -DTITLE_ID=\"$(TITLE_ID)\" -Isrc
LDFLAGS := $(if $(REL),,-Wl,--gc-sections)
LDADD  := -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService -lpthread

PKGSRC   := $(BUILD)/bundled_tile_pkg.c
WEBSRC   := $(BUILD)/web_assets.c
CASRC    := $(BUILD)/bundled_ca.c
# Discovered, not listed: a hand-maintained list silently drops any new .c file
# from the link because the payload is built in one command. eboot.c is excluded:
# it is the standalone eboot entry point and supplies its own _start.
PAYLOAD_SRCS := $(filter-out src/eboot.c, \
                 $(sort $(wildcard src/main.c src/install.c src/*.c src/*/*.c)))
HEADERS  := $(sort $(wildcard src/*.h src/*/*.h))

# Header dependency tracking. Note the SDK's prospero-clang wrapper passes
# --start-no-unused-arguments, which makes it drop -MMD, so no dRPC5.d is
# actually produced with this SDK. HEADERS below still forces a rebuild when a
# header changes, which is the correctness guarantee that matters here; the flag
# is kept because toolchains without that wrapper do emit the depfile.
DEPFLAGS := -MMD -MP
.DELETE_ON_ERROR:
-include dRPC5.d

THIRD_PARTY := third_party
CURL_INC    := $(THIRD_PARTY)/curl/include
CURL_LIBS   := $(THIRD_PARTY)/curl/lib/libcurl.a \
               $(THIRD_PARTY)/curl/lib/libmbedtls.a \
               $(THIRD_PARTY)/curl/lib/libmbedx509.a \
               $(THIRD_PARTY)/curl/lib/libmbedcrypto.a
CACERT      := $(THIRD_PARTY)/cacert.pem

all: dRPC5.elf

dRPC5.elf: $(PAYLOAD_SRCS) $(HEADERS) $(PKGSRC) $(WEBSRC) $(CASRC) $(CURL_LIBS)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(LDFLAGS) -I$(BUILD) -I$(CURL_INC) -isystem $(THIRD_PARTY)/mbedtls/include -o $@ $(PAYLOAD_SRCS) $(PKGSRC) $(WEBSRC) $(CASRC) $(CURL_LIBS) $(LDADD)

$(BUILD)/embed: tools/embed.c
	mkdir -p $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(BUILD)/mk_tile_pkg: tools/mk_tile_pkg.c
	mkdir -p $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $< -ldl

SEND_PAYLOAD := tools/send-payload

$(SEND_PAYLOAD): tools/deploy/send-payload.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(CASRC): $(CACERT) $(BUILD)/embed
	$(BUILD)/embed --out $@ --dec kCaPem=$(CACERT):kCaPemSize

WEBSRCS := web/index.html web/pc.html web/style.css web/vendor/qrcode.js \
	web/js/util.js web/js/app.js web/js/remote.js web/js/signin.js

$(WEBSRC): $(WEBSRCS) $(BUILD)/embed
	$(BUILD)/embed --out $@ --hex --include stddef.h --check-nul \
		kIndexHtml=web/index.html:kIndexHtmlLen \
		kPcHtml=web/pc.html:kPcHtmlLen \
		kStyleCss=web/style.css:kStyleCssLen \
		kQrcodeJs=web/vendor/qrcode.js:kQrcodeJsLen \
		kJsUtil=web/js/util.js:kJsUtilLen \
		kJsApp=web/js/app.js:kJsAppLen \
		kJsRemote=web/js/remote.js:kJsRemoteLen \
		kJsSignin=web/js/signin.js:kJsSigninLen

$(PKGSRC): $(DIST)/drpc5-tile.pkg $(BUILD)/embed
	$(BUILD)/embed --out $@ --dec kTilePkg=$(DIST)/drpc5-tile.pkg:kTilePkgSize:kTilePkgRawSize

$(DIST)/drpc5-tile.pkg: $(BUILD)/homebrew/eboot.bin tile/sce_sys/param.json tile/sce_sys/icon0.png $(BUILD)/mk_tile_pkg
	$(LPP_ENV) $(BUILD)/mk_tile_pkg \
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


$(BUILD)/tile-elf: $(BUILD)/tile-elf.o src/eboot.x
	$(LD) --static -T src/eboot.x -o $@ $(BUILD)/tile-elf.o

$(BUILD)/tile-elf.o: src/eboot.c
	mkdir -p $(BUILD)
	$(CC) -c $(CFLAGS) -o $@ $<

tile: $(DIST)/drpc5-tile.pkg

deploy: dRPC5.elf $(SEND_PAYLOAD)
	$(SEND_PAYLOAD) -h $(PS5_HOST) -p $(PS5_PORT) $<

send-payload: $(SEND_PAYLOAD)

clean:
	rm -rf $(BUILD) $(DIST) dRPC5.elf

.PHONY: all tile deploy send-payload clean
