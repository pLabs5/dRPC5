# The shared build environment - payload SDK discovery, NixOS packaging quirks,
# host tools, manifest/version plumbing - comes from the pLabs5 SDK. This file
# keeps only what is dRPC5-specific: identity, sources, flags, and the package
# and deploy targets.

PLABS5_SDK_CANDIDATES := $(PLABS5_SDK) $(CURDIR)/plabs5-sdk /opt/plabs5-sdk
PLABS5_SDK_FILE := $(firstword $(foreach d,$(PLABS5_SDK_CANDIDATES),$(wildcard $(d)/toolchain/plabs5.mk)))
ifeq ($(PLABS5_SDK_FILE),)
$(error pLabs5 SDK not found: set PLABS5_SDK, or install it at ./plabs5-sdk or /opt/plabs5-sdk)
endif
PLABS5_SDK := $(patsubst %/toolchain/plabs5.mk,%,$(PLABS5_SDK_FILE))

TITLE_ID    := DRPC00001
CONTENT_ID  := UP9000-DRPC00001_00-DRPC5AAAAAAAAAAA
PKG_TITLE   := dRPC5

include $(PLABS5_SDK_FILE)

# dRPC5 stamps the minor component in tenths (0.5 -> 00.50). Keep that here
# until the next version bump moves this repo onto the SDK's canonical
# two-digit rule.
PKG_VERSION := $(shell printf '%s' '$(MANIFEST_VERSION)' | awk -F. '{printf "%02d.%02d", $$1+0, ($$2+0)*10}')

# -Os + section GC keeps the payload small; -g0 drops debug info, which was
# ~130KB of the deployed ELF. Override REL for a debug build: make REL=1
REL ?= 0
ifeq ($(REL),1)
OPT := -O0 -g
else
OPT := -Os -g0 -ffunction-sections -fdata-sections
endif

# The project's include/ (the <plabs5/paths.h> shim) and src/ must be searched
# before the SDK's include/ so the project's paths/messages win.
CFLAGS := -Wall -Werror $(OPT) -DTITLE_ID=\"$(TITLE_ID)\" -Iinclude -Isrc -I$(PLABS5_INCLUDE)
LDFLAGS := $(if $(REL),,-Wl,--gc-sections)
LDADD  := -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService -lpthread

# Layer 2 bootstrap behaviour: lowercase log prefix, and no version stamping -
# the tile is only (re)installed when it is absent.
PLABS5_TARGET_DEFS := -DPLABS5_LOG_PREFIX='"drpc5"'

PKGSRC   := $(BUILD)/bundled_tile_pkg.c
WEBSRC   := $(BUILD)/web_assets.c
CASRC    := $(BUILD)/bundled_ca.c
# Discovered, not listed: a hand-maintained list silently drops any new .c file
# from the link because the payload is built in one command. Discovery is
# recursive so modules can be nested as deep as they need to be. eboot.c is
# excluded: it is the standalone eboot entry point and supplies its own _start.
PAYLOAD_SRCS := $(filter-out src/eboot/eboot.c,$(sort $(shell find src -name '*.c')))
HEADERS  := $(sort $(shell find src -name '*.h'))

# Header dependency tracking. Note the SDK's prospero-clang wrapper passes
# --start-no-unused-arguments, which makes it drop -MMD, so no dRPC5.d is
# actually produced with this SDK. HEADERS below still forces a rebuild when a
# header changes, which is the correctness guarantee that matters here; the flag
# is kept because toolchains without that wrapper do emit the depfile.
DEPFLAGS := -MMD -MP
-include dRPC5.d

THIRD_PARTY := third_party
CURL_INC    := $(THIRD_PARTY)/curl/include
CURL_LIBS   := $(THIRD_PARTY)/curl/lib/libcurl.a \
               $(THIRD_PARTY)/curl/lib/libmbedtls.a \
               $(THIRD_PARTY)/curl/lib/libmbedx509.a \
               $(THIRD_PARTY)/curl/lib/libmbedcrypto.a
CACERT      := $(THIRD_PARTY)/cacert.pem

all: dRPC5.elf

dRPC5.elf: $(PAYLOAD_SRCS) $(HEADERS) $(PKGSRC) $(WEBSRC) $(CASRC) $(CURL_LIBS) $(PLABS5_LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(LDFLAGS) -I$(BUILD) -I$(CURL_INC) -isystem $(THIRD_PARTY)/mbedtls/include -o $@ $(PAYLOAD_SRCS) $(PLABS5_LIB) $(PKGSRC) $(WEBSRC) $(CASRC) $(CURL_LIBS) $(LDADD)

SEND_PAYLOAD := $(BUILD)/send-payload

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


$(BUILD)/tile-elf: $(BUILD)/tile-elf.o $(PLABS5_SDK)/target/eboot.x
	$(LD) --static -T $(PLABS5_SDK)/target/eboot.x -o $@ $(BUILD)/tile-elf.o

$(BUILD)/tile-elf.o: src/eboot/eboot.c
	mkdir -p $(BUILD)
	$(CC) -c $(CFLAGS) -o $@ $<

tile: $(DIST)/drpc5-tile.pkg

deploy: dRPC5.elf $(SEND_PAYLOAD)
	$(SEND_PAYLOAD) -h $(PS5_HOST) -p $(PS5_PORT) $<

send-payload: $(SEND_PAYLOAD)

clean:
	rm -rf $(BUILD) $(DIST) dRPC5.elf

.PHONY: all tile deploy send-payload clean
