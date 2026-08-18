# Build system for the kernel-bypass feed handler.
#
#   make            build every benchmark and tool into build/bin
#   make bench      build benchmarks only
#   make tools      build host tools only
#   make run-bench  build and execute every benchmark in sequence
#   make clean      remove build artefacts
#
# Override the compiler or flags from the command line, e.g.
#   make CXX=clang++ OPT="-O2"

CXX      ?= g++
CXXSTD   := -std=c++20
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion
OPT      ?= -O3 -march=native -fno-omit-frame-pointer
INCLUDE  := -Iinclude
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(INCLUDE)
LDFLAGS  := -pthread

BUILD := build
BIN   := $(BUILD)/bin

BENCH_SRCS := $(wildcard bench/*.cpp)
TOOL_SRCS  := $(wildcard tools/*.cpp)
BENCH_BINS := $(patsubst bench/%.cpp,$(BIN)/%,$(BENCH_SRCS))
TOOL_BINS  := $(patsubst tools/%.cpp,$(BIN)/%,$(TOOL_SRCS))

.PHONY: all bench tools run-bench clean

all: bench tools

bench: $(BENCH_BINS)

tools: $(TOOL_BINS)

$(BIN)/%: bench/%.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BIN)/%: tools/%.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BIN):
	@mkdir -p $(BIN)

run-bench: bench
	@for b in $(BENCH_BINS); do \
		echo "--- $$b ---"; \
		$$b || exit 1; \
	done

clean:
	@rm -rf $(BUILD)
