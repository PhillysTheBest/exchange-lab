# Matching engine — build with mingw32-make (Windows, MSYS2 UCRT64) or make (Linux).
#
#   make test     debug build (-Og, PX_DEBUG invariants, _GLIBCXX_ASSERTIONS); builds and runs every test
#   make bench    release build (-O3 -march=native -flto); builds and runs every benchmark
#   make latency  builds and runs only bench_matching_latency (the 1M-order headline number)
#   make repl     release build of the interactive REPL
#   make all      test, then build the benchmarks and the REPL
#   make clean
#
# Every object is compiled with -MMD -MP, so editing a header rebuilds exactly
# the objects that include it.
#
# Variants: BUILD picks the output directory, SAN adds sanitizer flags to the
# debug (test) build. The Linux sanitizer run used for this project:
#   make BUILD=build-asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all' test

CXX      ?= g++
CXXSTD   := -std=c++20
WARN     := -Wall -Wextra -Wpedantic
INCLUDES := -Iinclude -I.
DEPFLAGS := -MMD -MP

# No sanitizer runtimes ship with MSYS2's mingw-w64 g++, so Windows debug builds
# rely on _GLIBCXX_ASSERTIONS plus the engine's own PX_DEBUG checks; sanitizers
# run on Linux via SAN (see above).
# -fno-tree-loop-distribute-patterns: stop GCC from rewriting the hand-written
# shift loops in sorted_levels.hpp into memmove calls (see that file for why).
# It is in both flag sets because with -flto code is generated at link time.
CODEGEN       := -fno-tree-loop-distribute-patterns
SAN          ?=
DEBUG_FLAGS   := -Og -g -DPX_DEBUG=1 -D_GLIBCXX_ASSERTIONS -fno-omit-frame-pointer $(CODEGEN) $(SAN)
RELEASE_FLAGS := -O3 -march=native -flto -DNDEBUG $(CODEGEN)

ifeq ($(OS),Windows_NT)
  PLATFORM_SRC := src/platform_win32.cpp
  # Static: no dependence on whichever libstdc++/winpthread DLL wins the PATH search.
  LDLIBS := -static
  EXE := .exe
else
  PLATFORM_SRC := src/platform_linux.cpp
  LDLIBS := -pthread
  EXE :=
endif

BUILD ?= build
DBG := $(BUILD)/debug
REL := $(BUILD)/release

LIB_SRC := src/matching_engine.cpp $(PLATFORM_SRC)
LIB_DBG := $(LIB_SRC:%.cpp=$(DBG)/%.o)
LIB_REL := $(LIB_SRC:%.cpp=$(REL)/%.o)

TESTS   := test_containers test_spsc_ring test_engine fuzz_vs_reference test_alloc_audit test_determinism \
           test_arena
BENCHES := bench_engine bench_ring bench_matching_latency

TEST_BINS  := $(TESTS:%=$(DBG)/%$(EXE))
BENCH_BINS := $(BENCHES:%=$(REL)/%$(EXE))
REPL_BIN   := $(REL)/repl$(EXE)
# The arena: the headless experiment runner and the terminal viewer. Both are
# consumers of the same MatchingEngine the benchmarks measure, so neither can
# affect a latency number.
ARENA_BINS := $(REL)/sweep$(EXE) $(REL)/arena$(EXE)

.PHONY: all test bench latency repl arena sweep clean

all: test test-cli $(BENCH_BINS) $(REPL_BIN) $(ARENA_BINS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# The sweep runner's command line, checked the only way a command line can be:
# by running it. A bad field, flag or value must fail loudly (exit 2) and a
# configuration that could never terminate must be refused, not hang. These were
# all verified by hand once; this target is so they stay verified.
.PHONY: test-cli
test-cli: $(REL)/sweep$(EXE)
	@set -e; \
	fail() { echo "FAIL: $$1"; exit 1; }; \
	echo "== sweep command line"; \
	./$< --help > /dev/null || fail "--help should succeed"; \
	! ./$< --bogus > /dev/null 2>&1 || fail "unknown flag should fail"; \
	! ./$< --values=abc > /dev/null 2>&1 || fail "unparseable --values should fail"; \
	! ./$< --param=nonsense.field --values=1 --seeds=1 --requests=500 --quiet > /dev/null 2>&1 \
	    || fail "unknown field should fail"; \
	! ./$< --values=1000 --seeds=1 --requests=500 --set=session.stall_limit_ns=0 \
	    --set=session.max_time_ns=0 --quiet > /dev/null 2>&1 \
	    || fail "a session with no bound should be refused"; \
	! ./$< --values=1000 --seeds=1 --requests=500 --set=noise.count=100 --quiet > /dev/null 2>&1 \
	    || fail "a roster past the participant ceiling should be refused"; \
	./$< --values=1000 --seeds=1 --requests=2000 --quiet > /dev/null || fail "a normal sweep should succeed"; \
	echo "7/7 command-line checks passed"

bench: $(BENCH_BINS)
	@set -e; for b in $(BENCH_BINS); do echo "== $$b"; ./$$b; done

latency: $(REL)/bench_matching_latency$(EXE)
	./$<

repl: $(REPL_BIN)

arena: $(ARENA_BINS)

sweep: $(REL)/sweep$(EXE)
	./$<

$(DBG)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $(DEPFLAGS) -c $< -o $@

$(REL)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BINS): $(DBG)/%$(EXE): $(DBG)/tests/%.o $(LIB_DBG)
	$(CXX) $(DEBUG_FLAGS) $^ -o $@ $(LDLIBS)

$(BENCH_BINS): $(REL)/%$(EXE): $(REL)/bench/%.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(REPL_BIN): $(REL)/tools/repl.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(ARENA_BINS): $(REL)/%$(EXE): $(REL)/arena/%.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(DBG)/*/*.d $(REL)/*/*.d $(DBG)/*/*/*.d $(REL)/*/*/*.d)
