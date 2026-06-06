# hprscript — standalone Vectorscan-powered multi-pattern grep
#
# Vectorscan is an API/ABI-compatible fork of Intel Hyperscan with
# portable SIMD support (x86-64 SSE/AVX2, ARM NEON/SVE, POWER VSX).
#
# Static linking strategy:
#   - libhs.a               (Vectorscan, statically linked)
#   - libstdc++, libgcc     (statically linked via -static-libstdc++ -static-libgcc)
#   - glibc                 (kept dynamic — fully -static breaks NSS resolution)
# Resulting binary runs on any modern Linux host without any extra packages.

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

# Static-link Vectorscan (.a) and libstdc++/libgcc; keep libc dynamic.
HS_STATIC = $(VECTORSCAN_PREFIX)/lib/libhs.a
LDFLAGS  ?= -static-libstdc++ -static-libgcc -pthread
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
	@ldd $@ || true

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

install: $(BIN)
	install -d $(HOME)/.local/bin
	install -m 0755 $(BIN) $(HOME)/.local/bin/

-include $(DEPS)
