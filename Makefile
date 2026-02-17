# ──────────────────────────────────────────────────────────────
# Shortcuts Pak — Build System
# ──────────────────────────────────────────────────────────────

APP_NAME   := shortcuts
PAK_NAME   := Shortcuts
MODULE     := github.com/Helaas/nextui-shortcuts-pak
GABAGOOL   := github.com/BrandonKowalski/gabagool/v2
DOCKER_IMG := ghcr.io/brandonkowalski/quasimodo:latest

BUILD_DIR  := build
CACHE_DIR  := .cache
TOOLCHAIN_CACHE := $(CACHE_DIR)/go-toolchain

# ── Platform auto-detection ──────────────────────────────────

ifdef PLATFORM
ifeq ($(PLATFORM),tg5040)
all: tg5040
else ifeq ($(PLATFORM),tg5050)
all: tg5050
else
all: mac
endif
else ifeq ($(shell uname -s),Darwin)
all: mac
else
all: tg5040
endif

# ── Native macOS build ───────────────────────────────────────

mac:
	@mkdir -p $(BUILD_DIR)
	CGO_ENABLED=1 go build -mod=vendor -o $(BUILD_DIR)/$(APP_NAME) .

# ── Docker ARM64 builds ──────────────────────────────────────

tg5040:
	@mkdir -p $(BUILD_DIR)/tg5040/lib $(CACHE_DIR)/go-mod $(CACHE_DIR)/go-build $(TOOLCHAIN_CACHE)
	docker run --rm --platform linux/arm64 \
		-v "$(CURDIR)":/build \
		-v "$(CURDIR)/$(CACHE_DIR)/go-mod":/root/go/pkg/mod \
		-v "$(CURDIR)/$(CACHE_DIR)/go-build":/root/.cache/go-build \
		-v "$(CURDIR)/$(TOOLCHAIN_CACHE)":/root/.cache/go-toolchain \
		-w /build \
		$(DOCKER_IMG) \
		sh -c 'GOTOOLCHAINCACHE=/root/.cache/go-toolchain CGO_ENABLED=1 GOOS=linux GOARCH=arm64 go build -mod=vendor -o $(BUILD_DIR)/tg5040/$(APP_NAME) . && \
		       cp /usr/lib/aarch64-linux-gnu/libSDL2_gfx-1.0.so.0.0.2 $(BUILD_DIR)/tg5040/lib/libSDL2_gfx-1.0.so.0 && \
		       cp /usr/lib/aarch64-linux-gnu/libSDL2_gfx-1.0.so.0.0.2 $(BUILD_DIR)/tg5040/lib/libSDL2_gfx-1.0.so.0.0.2'

tg5050:
	@mkdir -p $(BUILD_DIR)/tg5050/lib $(CACHE_DIR)/go-mod $(CACHE_DIR)/go-build $(TOOLCHAIN_CACHE)
	docker run --rm --platform linux/arm64 \
		-v "$(CURDIR)":/build \
		-v "$(CURDIR)/$(CACHE_DIR)/go-mod":/root/go/pkg/mod \
		-v "$(CURDIR)/$(CACHE_DIR)/go-build":/root/.cache/go-build \
		-v "$(CURDIR)/$(TOOLCHAIN_CACHE)":/root/.cache/go-toolchain \
		-w /build \
		$(DOCKER_IMG) \
		sh -c 'GOTOOLCHAINCACHE=/root/.cache/go-toolchain CGO_ENABLED=1 GOOS=linux GOARCH=arm64 go build -mod=vendor -o $(BUILD_DIR)/tg5050/$(APP_NAME) . && \
		       cp /usr/lib/aarch64-linux-gnu/libSDL2_gfx-1.0.so.0.0.2 $(BUILD_DIR)/tg5050/lib/libSDL2_gfx-1.0.so.0 && \
		       cp /usr/lib/aarch64-linux-gnu/libSDL2_gfx-1.0.so.0.0.2 $(BUILD_DIR)/tg5050/lib/libSDL2_gfx-1.0.so.0.0.2'

embedded: tg5040 tg5050

# ── Vendor patches ────────────────────────────────────────────
# Gabagool hardcodes /dev/input/event1 for the power button.
# TG5050 uses /dev/input/event2. This target applies the fix.

GABAGOOL_INIT := vendor/github.com/BrandonKowalski/gabagool/v2/pkg/gabagool/init.go
GABAGOOL_NEXTVAL := vendor/github.com/BrandonKowalski/gabagool/v2/pkg/gabagool/platform/nextui/theming.go

patch-vendor:
	@if [ -f "$(GABAGOOL_INIT)" ] && grep -q 'DevicePath:.*"/dev/input/event1"' "$(GABAGOOL_INIT)"; then \
		echo "Patching Gabagool power button for TG5050 support..."; \
		cp patches/gabagool-power-button-tg5050.patch /tmp/_gaba_patch.patch; \
		cd "$(CURDIR)" && git apply --whitespace=nowarn patches/gabagool-power-button-tg5050.patch 2>/dev/null || \
			patch -p1 < patches/gabagool-power-button-tg5050.patch; \
		echo "Patch applied."; \
	else \
		echo "Gabagool power button patch already applied (or vendor not present)."; \
	fi
	@if [ -f "$(GABAGOOL_NEXTVAL)" ] && ! grep -q 'platformEnv := strings.ToLower' "$(GABAGOOL_NEXTVAL)"; then \
		echo "Patching Gabagool nextval path for TG5050 support..."; \
		cp patches/gabagool-nextval-path-tg5050.patch /tmp/_gaba_nextval_patch.patch; \
		cd "$(CURDIR)" && git apply --whitespace=nowarn patches/gabagool-nextval-path-tg5050.patch 2>/dev/null || \
			patch -p1 < patches/gabagool-nextval-path-tg5050.patch; \
		echo "Patch applied."; \
	else \
		echo "Gabagool nextval path patch already applied (or vendor not present)."; \
	fi

# ── Dependency management ────────────────────────────────────

deps:
	go get $(GABAGOOL)@latest
	go mod tidy
	go mod vendor
	$(MAKE) patch-vendor

# ── Packaging ────────────────────────────────────────────────

package-tg5040: tg5040
	@rm -rf $(BUILD_DIR)/pak-stage
	@mkdir -p $(BUILD_DIR)/pak-stage/resources/lib
	@mkdir -p $(BUILD_DIR)/release/tg5040
	cp $(BUILD_DIR)/tg5040/$(APP_NAME) $(BUILD_DIR)/pak-stage/
	cp launch.sh $(BUILD_DIR)/pak-stage/
	cp pak.json $(BUILD_DIR)/pak-stage/
	cp LICENSE $(BUILD_DIR)/pak-stage/ 2>/dev/null || true
	cp $(BUILD_DIR)/tg5040/lib/* $(BUILD_DIR)/pak-stage/resources/lib/
	cd $(BUILD_DIR)/pak-stage && zip -r "$(CURDIR)/$(BUILD_DIR)/release/tg5040/$(PAK_NAME).pak.zip" . -x '.*'
	@rm -rf $(BUILD_DIR)/pak-stage

package-tg5050: tg5050
	@rm -rf $(BUILD_DIR)/pak-stage
	@mkdir -p $(BUILD_DIR)/pak-stage/resources/lib
	@mkdir -p $(BUILD_DIR)/release/tg5050
	cp $(BUILD_DIR)/tg5050/$(APP_NAME) $(BUILD_DIR)/pak-stage/
	cp launch.sh $(BUILD_DIR)/pak-stage/
	cp pak.json $(BUILD_DIR)/pak-stage/
	cp LICENSE $(BUILD_DIR)/pak-stage/ 2>/dev/null || true
	cp $(BUILD_DIR)/tg5050/lib/* $(BUILD_DIR)/pak-stage/resources/lib/
	cd $(BUILD_DIR)/pak-stage && zip -r "$(CURDIR)/$(BUILD_DIR)/release/tg5050/$(PAK_NAME).pak.zip" . -x '.*'
	@rm -rf $(BUILD_DIR)/pak-stage

package: package-tg5040 package-tg5050

# ── TrimUI .pakz export ──────────────────────────────────────

export-trimui: embedded
	@rm -rf $(BUILD_DIR)/trimui-stage
	@mkdir -p $(BUILD_DIR)/trimui-stage/Tools/tg5040/$(PAK_NAME).pak/resources/lib
	@mkdir -p $(BUILD_DIR)/trimui-stage/Tools/tg5050/$(PAK_NAME).pak/resources/lib
	@mkdir -p $(BUILD_DIR)/release/trimui
	cp $(BUILD_DIR)/tg5040/$(APP_NAME) $(BUILD_DIR)/trimui-stage/Tools/tg5040/$(PAK_NAME).pak/
	cp launch.sh $(BUILD_DIR)/trimui-stage/Tools/tg5040/$(PAK_NAME).pak/
	cp pak.json $(BUILD_DIR)/trimui-stage/Tools/tg5040/$(PAK_NAME).pak/
	cp LICENSE $(BUILD_DIR)/trimui-stage/Tools/tg5040/$(PAK_NAME).pak/ 2>/dev/null || true
	cp $(BUILD_DIR)/tg5040/lib/* $(BUILD_DIR)/trimui-stage/Tools/tg5040/$(PAK_NAME).pak/resources/lib/
	cp $(BUILD_DIR)/tg5050/$(APP_NAME) $(BUILD_DIR)/trimui-stage/Tools/tg5050/$(PAK_NAME).pak/
	cp launch.sh $(BUILD_DIR)/trimui-stage/Tools/tg5050/$(PAK_NAME).pak/
	cp pak.json $(BUILD_DIR)/trimui-stage/Tools/tg5050/$(PAK_NAME).pak/
	cp LICENSE $(BUILD_DIR)/trimui-stage/Tools/tg5050/$(PAK_NAME).pak/ 2>/dev/null || true
	cp $(BUILD_DIR)/tg5050/lib/* $(BUILD_DIR)/trimui-stage/Tools/tg5050/$(PAK_NAME).pak/resources/lib/
	@# Include SHORTCUT.pak bridge emu for tool shortcuts
	@mkdir -p $(BUILD_DIR)/trimui-stage/Emus/tg5040/SHORTCUT.pak
	@mkdir -p $(BUILD_DIR)/trimui-stage/Emus/tg5050/SHORTCUT.pak
	cp resources/SHORTCUT.pak/launch.sh $(BUILD_DIR)/trimui-stage/Emus/tg5040/SHORTCUT.pak/
	cp resources/SHORTCUT.pak/launch.sh $(BUILD_DIR)/trimui-stage/Emus/tg5050/SHORTCUT.pak/
	cd $(BUILD_DIR)/trimui-stage && zip -9 -r "$(CURDIR)/$(BUILD_DIR)/release/trimui/$(PAK_NAME).pakz" . -x '.*'
	@rm -rf $(BUILD_DIR)/trimui-stage

# ── Cleanup ───────────────────────────────────────────────────

clean:
	rm -rf $(BUILD_DIR)

clean-all: clean
	rm -rf $(CACHE_DIR)

# ── Help ──────────────────────────────────────────────────────

help:
	@echo "Targets:"
	@echo "  all           Auto-detect platform and build"
	@echo "  mac           Build for macOS (native)"
	@echo "  tg5040        Build for TG5040 (Docker ARM64)"
	@echo "  tg5050        Build for TG5050 (Docker ARM64)"
	@echo "  embedded      Build all embedded platforms"
	@echo "  deps          Update Go dependencies + apply patches"
	@echo "  patch-vendor  Apply vendor patches (TG5050 power button)"
	@echo "  package       Package both platforms (.pak.zip)"
	@echo "  export-trimui Create .pakz for TrimUI Tools"
	@echo "  clean         Remove build artifacts"
	@echo "  clean-all     Remove build + cache"

.PHONY: all mac tg5040 tg5050 embedded deps patch-vendor package package-tg5040 package-tg5050 export-trimui clean clean-all help
