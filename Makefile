BINARY  := timeout
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
DIST_DIR := dist
TEST_DIR := $(DIST_DIR)/test

CC   := clang
SRCS := $(wildcard src/*.c)
HDRS := $(wildcard src/*.h)
# The pure modules the unit tests link (everything but main.c).
LIB_SRCS := $(filter-out src/main.c,$(SRCS))
UNIT_SRCS := $(wildcard tests/unit/*.c)

# macOS 12 is the floor: every API used (kqueue, posix signals) is far older.
TARGET_FLAGS := -arch arm64 -mmacosx-version-min=12.0

# The C safety layers (docs/en/adr/0001-c-and-memory-safety-rules.md).
# Layer 1: warnings are errors in every build.
WARN_FLAGS := -std=c17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
	-Wshadow -Wvla -Wformat=2 -Wimplicit-fallthrough -Wuninitialized \
	-Wsometimes-uninitialized -Wreturn-stack-address
# Layer 3: bounds checking, in both the shipped and the test build.
BOUNDS_FLAGS := -fbounds-safety
# Layer 4, shipped build. Apple's SDK treats any _FORTIFY_SOURCE above 2 as 2.
RELEASE_FLAGS := -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong
# Layer 4, test build. Without -fno-sanitize-recover UBSan reports and exits 0.
TEST_SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -ftrivial-auto-var-init=pattern
# ASan installs SEGV/BUS/FPE handlers before main, which would hide the
# dispositions timeout inherited (ADR-0002).
TEST_ASAN_OPTIONS := handle_segv=0:handle_sigbus=0:handle_sigfpe=0

VERSION_FLAG := -DTIMEOUT_VERSION='"$(VERSION)"'

# macOS Developer ID signing / notarization (see nlink-jp/.github
# CONVENTIONS.md §Code Signing). Defaults match any Developer ID
# Application cert in the keychain and the org-standard notary
# profile. Builds without these fall back to ad-hoc / un-notarized
# with a one-line warning — see scripts/codesign-darwin.sh.
CODESIGN_IDENTITY ?= Developer ID Application
NOTARY_PROFILE    ?= nlink-jp-notary

# darwin/arm64 only: Linux has GNU timeout, and Windows has no signals.
PLATFORMS := darwin/arm64

.PHONY: build build-all build-test lint controls unit test package verify-release clean help

## build: Build the shipped binary (dist/timeout)
build: $(SRCS) $(HDRS)
	@mkdir -p $(DIST_DIR)
	$(CC) $(TARGET_FLAGS) $(WARN_FLAGS) $(BOUNDS_FLAGS) $(RELEASE_FLAGS) $(VERSION_FLAG) \
		$(SRCS) -o $(DIST_DIR)/$(BINARY)
	@scripts/codesign-darwin.sh $(DIST_DIR)/$(BINARY) "$(CODESIGN_IDENTITY)"

## build-all: Build every release platform (darwin/arm64 only)
build-all: $(SRCS) $(HDRS)
	@mkdir -p $(DIST_DIR)
	$(CC) $(TARGET_FLAGS) $(WARN_FLAGS) $(BOUNDS_FLAGS) $(RELEASE_FLAGS) $(VERSION_FLAG) \
		$(SRCS) -o $(DIST_DIR)/$(BINARY)-darwin-arm64
	@scripts/codesign-darwin.sh $(DIST_DIR)/$(BINARY)-darwin-arm64 "$(CODESIGN_IDENTITY)" "$(BINARY)"

## build-test: Build the sanitizer binary the tests also run (dist/test/timeout)
build-test: $(SRCS) $(HDRS)
	@mkdir -p $(TEST_DIR)
	$(CC) $(TARGET_FLAGS) $(WARN_FLAGS) $(BOUNDS_FLAGS) $(TEST_SAN_FLAGS) $(VERSION_FLAG) \
		$(SRCS) -o $(TEST_DIR)/$(BINARY)

## lint: Static analysis; any finding fails (layer 2)
lint:
	@for f in $(SRCS) $(UNIT_SRCS); do \
		$(CC) --analyze -Xanalyzer -analyzer-werror $(TARGET_FLAGS) $(WARN_FLAGS) $(BOUNDS_FLAGS) \
			"$$f" -o /dev/null || exit 1; \
	done
	@echo "lint: ok"

## controls: Prove each safety layer fails on a planted bug
controls:
	@CC="$(CC)" WARN_FLAGS="$(WARN_FLAGS)" BOUNDS_FLAGS="$(BOUNDS_FLAGS)" \
		TEST_SAN_FLAGS="$(TEST_SAN_FLAGS)" OUT="$(TEST_DIR)/controls" \
		sh tests/controls/run.sh

## unit: Unit tests and input stress for the pure modules (layer 5), test flags
unit: $(LIB_SRCS) $(HDRS) $(UNIT_SRCS)
	@mkdir -p $(TEST_DIR)
	$(CC) $(TARGET_FLAGS) $(WARN_FLAGS) $(BOUNDS_FLAGS) $(TEST_SAN_FLAGS) \
		$(LIB_SRCS) $(UNIT_SRCS) -o $(TEST_DIR)/unit
	ASAN_OPTIONS="$(TEST_ASAN_OPTIONS)" $(TEST_DIR)/unit

## test: controls, lint, unit, then the e2e tests against the test and shipped builds
test: controls lint unit build-test build
	TIMEOUT_BIN="$(abspath $(TEST_DIR)/$(BINARY))" ASAN_OPTIONS="$(TEST_ASAN_OPTIONS)" \
		go test -count=1 ./...
	TIMEOUT_BIN="$(abspath $(DIST_DIR)/$(BINARY))" go test -count=1 ./...

## package: Build, archive with version suffix, and notarize. Asset naming
## follows the org Release Archive Standard (timeout-vX.Y.Z-darwin-arm64.zip).
package: build-all
	@cd $(DIST_DIR) && for p in $(PLATFORMS); do os=$${p%/*}; arch=$${p#*/}; \
		stage=_pkg; rm -rf $$stage; mkdir -p $$stage; \
		cp "$(BINARY)-$$os-$$arch" "$$stage/$(BINARY)"; \
		cp ../README.md ../LICENSE $$stage/; \
		base="$(BINARY)-$(VERSION)-$$os-$$arch"; \
		( cd $$stage && zip -q "../$$base.zip" * ); \
		rm -rf $$stage; \
	done
	@scripts/notarize-darwin.sh $(DIST_DIR)/$(BINARY)-$(VERSION)-darwin-arm64.zip "$(NOTARY_PROFILE)"

## verify-release: refuse to release a zip that is un-notarized, stale, does
## not unpack, does not run, or holds a build from another tag.
## Every step fails closed; only the spctl line is informational.
verify-release:
	@test -f "$(DIST_DIR)/$(BINARY)-$(VERSION)-darwin-arm64.zip.notarized" || { \
		echo "verify-release: FAIL — $(BINARY)-$(VERSION)-darwin-arm64.zip has no notarization marker."; \
		echo "  make package must end with '[notarize] ...: Accepted'. Do not upload this zip."; \
		exit 1; }
	@test "$(DIST_DIR)/$(BINARY)-$(VERSION)-darwin-arm64.zip.notarized" -nt "$(DIST_DIR)/$(BINARY)-$(VERSION)-darwin-arm64.zip" || { \
		echo "verify-release: FAIL — the zip was rebuilt after its marker (re-run make package)."; \
		exit 1; }
	@tmp=$$(mktemp -d); rc=0; \
		if ! unzip -oq "$(DIST_DIR)/$(BINARY)-$(VERSION)-darwin-arm64.zip" -d "$$tmp"; then \
			echo "verify-release: FAIL — the zip does not unpack. Do not upload it."; rc=1; \
		elif ! out=$$("$$tmp/$(BINARY)" --version 2>&1); then \
			echo "verify-release: FAIL — the packaged binary does not run:"; \
			echo "  $$out"; rc=1; \
		elif ! printf '%s\n' "$$out" | grep -qF "$(VERSION)"; then \
			echo "verify-release: FAIL — the packaged binary reports \"$$out\", not $(VERSION)."; \
			echo "  The zip holds a build from another tag (re-run make package)."; rc=1; \
		else \
			echo "  $$out"; \
			spctl -a -vv -t install "$$tmp/$(BINARY)" 2>&1 | head -2 || true; \
		fi; \
		rm -rf "$$tmp"; \
		exit $$rc
	@echo "verify-release: OK ($(VERSION), notarized, unpacks, runs, reports its version)"

## clean: Remove build artifacts
clean:
	rm -rf $(DIST_DIR)/

## help: Show this help
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/## //'

# Homebrew tap generation (see scripts/release-brew.mk). After `make package`,
# `make brew` generates this formula from the built darwin-arm64 zip into the
# local nlink-jp/homebrew-tap checkout. The package target is unchanged.
BREW_KIND := formula
BREW_DESC := Run a command with a time limit (POSIX and GNU compatible)
include scripts/release-brew.mk
