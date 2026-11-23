# glassbox-dvp-sim: short commands for building, testing and running.
#
#   make                     build the simulator (Debug)
#   make list                list the scenarios
#   make watch S=S9          watch it in slow motion (SPEED=slow|normal|fast)
#   make story S=S9          the same explanation as plain text
#   make run S=S9 SEED=1     print a scenario's full trace at once
#   make test                build and run every test (Debug)
#   make test-all            the merge gate: Debug, Release and UBSan
#   make test-asan           ASan + UBSan (hangs on some macOS toolchains)
#   make clean               delete the build directories made by this file
#
# Compiler: Homebrew clang when installed, otherwise c++. Override with
#   make CXX_BIN=/path/to/clang++   (run `make clean` after switching)

LLVM_CXX := /opt/homebrew/opt/llvm/bin/clang++
CXX_BIN ?= $(if $(wildcard $(LLVM_CXX)),$(LLVM_CXX),c++)

# Some Command Line Tools installs ship a compiler without the C++ standard
# headers. Probe once; if <vector> is missing, use the SDK's copy.
HEADERS_OK := $(shell echo 'int main(){}' | $(CXX_BIN) -x c++ -std=c++20 \
                -include vector -fsyntax-only - >/dev/null 2>&1 && echo yes)
ifeq ($(HEADERS_OK),yes)
STDLIB_FIX :=
else
STDLIB_FIX := -nostdinc++ -isystem $(shell xcrun --show-sdk-path)/usr/include/c++/v1
endif

# -gdwarf-4: Apple's linker cannot read the newer debug format of recent clang.
CXX_FLAGS := -Werror -gdwarf-4 $(STDLIB_FIX)
CMAKE_COMMON = -DCMAKE_CXX_COMPILER=$(CXX_BIN) "-DCMAKE_CXX_FLAGS=$(CXX_FLAGS)"

BUILD := build/make
BIN := $(BUILD)/debug/glassbox-dvp-sim
S ?=
SEED ?= 1
SPEED ?= normal

.PHONY: all build list run watch story test test-all test-asan clean help

all: build

help:
	@sed -n '2,14p' Makefile | sed 's/^# \{0,1\}//'

$(BUILD)/debug/CMakeCache.txt:
	cmake -S . -B $(BUILD)/debug -DCMAKE_BUILD_TYPE=Debug $(CMAKE_COMMON)

$(BUILD)/release/CMakeCache.txt:
	cmake -S . -B $(BUILD)/release -DCMAKE_BUILD_TYPE=Release $(CMAKE_COMMON)

$(BUILD)/ubsan/CMakeCache.txt:
	cmake -S . -B $(BUILD)/ubsan -DCMAKE_BUILD_TYPE=Debug \
	  -DGLASSBOX_ENABLE_UBSAN=ON $(CMAKE_COMMON)

$(BUILD)/asan/CMakeCache.txt:
	cmake -S . -B $(BUILD)/asan -DCMAKE_BUILD_TYPE=Debug \
	  -DGLASSBOX_ENABLE_ASAN=ON -DGLASSBOX_ENABLE_UBSAN=ON $(CMAKE_COMMON)

# Quiet: the compiler log is shown only when the build fails.
build: $(BUILD)/debug/CMakeCache.txt
	@cmake --build $(BUILD)/debug -j --target glassbox-dvp-sim \
	  > $(BUILD)/last-build.log 2>&1 \
	  || { cat $(BUILD)/last-build.log; exit 1; }
	@echo "built $(BIN)" >&2

list: build
	@$(BIN) --list

run: build
	@if [ -z "$(S)" ]; then echo "usage: make run S=S1 [SEED=1]"; exit 2; fi
	@$(BIN) --scenario $(S) --seed $(SEED)

watch: build
	@if [ -z "$(S)" ]; then echo "usage: make watch S=S1 [SPEED=slow|normal|fast] [SEED=1]"; exit 2; fi
	@$(BIN) --scenario $(S) --seed $(SEED) --watch --speed $(SPEED)

story: build
	@if [ -z "$(S)" ]; then echo "usage: make story S=S1 [SEED=1]"; exit 2; fi
	@$(BIN) --scenario $(S) --seed $(SEED) --story

# One build directory: build everything, then run its tests.
define run_suite
	cmake --build $(BUILD)/$(1) -j
	cd $(BUILD)/$(1) && ctest --timeout 300 --output-on-failure
endef

test: $(BUILD)/debug/CMakeCache.txt
	$(call run_suite,debug)

test-all: $(BUILD)/debug/CMakeCache.txt $(BUILD)/release/CMakeCache.txt \
          $(BUILD)/ubsan/CMakeCache.txt
	$(call run_suite,debug)
	$(call run_suite,release)
	$(call run_suite,ubsan)

# Short per-test timeout: on the affected toolchains every ASan binary hangs.
test-asan: $(BUILD)/asan/CMakeCache.txt
	cmake --build $(BUILD)/asan -j
	cd $(BUILD)/asan && ctest --timeout 30 --output-on-failure

clean:
	rm -rf $(BUILD)
