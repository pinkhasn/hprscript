# hprscript — standalone Vectorscan-powered multi-pattern grep
#
# Vectorscan is an API/ABI-compatible fork of Intel Hyperscan with
# portable SIMD support (x86-64 SSE/AVX2, ARM NEON/SVE, POWER VSX).
#
# Static linking strategy:
#   - libhs.a               (Vectorscan, statically linked)
#   - libstdc++, libgcc     (Linux only — statically linked via
#                            -static-libstdc++ -static-libgcc)
#   - libc                  (kept dynamic — fully -static breaks NSS resolution
#                            on Linux; on macOS libSystem is always dynamic)
# Builds on Linux (x86-64, ARM64) and macOS (Apple Silicon / Intel). The
# resulting binary runs without extra packages on a matching host.

VECTORSCAN_PREFIX ?= /opt/vectorscan

# Version stamped into the binary (shown by `--version`). Derived from git
# tags so it tracks releases automatically; falls back for tarball builds
# with no git metadata. `--dirty` flags a build with uncommitted changes.
VERSION   := $(shell git describe --tags --always --dirty 2>/dev/null || echo v0.2.1)

CXX       ?= g++
CXXSTD    ?= -std=c++17
OPT       ?= -O2
WARN      ?= -Wall -Wextra -Wno-unused-parameter
CXXFLAGS  ?= $(CXXSTD) $(OPT) $(WARN) -pthread -Isrc -I$(VECTORSCAN_PREFIX)/include
CXXFLAGS  += -DHPRSCRIPT_VERSION='"$(VERSION)"'

UNAME_S := $(shell uname -s)

# Static-link Vectorscan (.a) and keep libc dynamic. Platform differences:
#   - Linux/GCC: also statically link libstdc++/libgcc so the binary runs on
#     hosts without a matching toolchain runtime.
#   - macOS/clang: libc++ ships with the OS and is ABI-stable, so it links
#     dynamically; Apple clang has no -static-libstdc++/-static-libgcc flags.
HS_STATIC = $(VECTORSCAN_PREFIX)/lib/libhs.a
ifeq ($(UNAME_S),Darwin)
  LDFLAGS ?= -pthread
  LDD     := otool -L
else
  LDFLAGS ?= -static-libstdc++ -static-libgcc -pthread
  LDD     := ldd
endif
LDLIBS   ?= $(HS_STATIC) -lm

BIN       = hprscript
SRC_DIR   = src
BUILD_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean test install

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	@echo
	@echo "Built $@. Dependencies (only libc/libm/libpthread should be dynamic):"
	@$(LDD) $@ || true

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# main.o embeds the git-derived VERSION; always rebuild it so the stamped
# version stays current between tags without requiring a full `make clean`.
$(BUILD_DIR)/main.o: .FORCE
.FORCE:
.PHONY: .FORCE

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN)

# Run the shell-based functional test suite. Builds the binary first
# (the test runner refuses to start without it).
test: $(BIN)
	@HPRSCRIPT_BIN=$(abspath $(BIN)) bash tests/run.sh
	@HPRSCRIPT_BIN=$(abspath $(BIN)) bash tests/accounting.sh
	@HPRSCRIPT_BIN=$(abspath $(BIN)) bash tests/edit_plan.sh
	@HPRSCRIPT_BIN=$(abspath $(BIN)) bash tests/investigate.sh
	@HPRSCRIPT_BIN=$(abspath $(BIN)) python3 tests/investigation_evidence.py
	@HPRSCRIPT_BIN=$(abspath $(BIN)) bash tests/query.sh
	@HPRSCRIPT_BIN=$(abspath $(BIN)) bash tests/golden_outputs.sh

install: $(BIN)
	install -d $(HOME)/.local/bin
	install -m 0755 $(BIN) $(HOME)/.local/bin/

-include $(DEPS)
