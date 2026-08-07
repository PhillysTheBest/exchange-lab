# prediction-markets — build via mingw32-make (Windows/MSYS2 UCRT64) or make (Linux).
# No CMake/Ninja in this environment by design — see the plan doc for why.
#
# Targets:
#   make test      debug build (asan/ubsan + PX_DEBUG invariants), builds & runs all tests
#   make bench     release build (-O3 -march=native -flto), builds & runs all benchmarks
#   make repl      release build of the interactive REPL
#   make live      release build of the Kalshi connectivity tools (needs OpenSSL)
#   make all       test + bench + repl
#   make clean

CXX      ?= g++
INCLUDES  = -Iinclude -Ilive -I.
CXXSTD    = -std=c++20
WARN      = -Wall -Wextra -Wpedantic

# NOTE: this MSYS2/mingw-w64 g++ toolchain ships no ASan/UBSan/TSan runtime
# (-lasan/-lubsan/-ltsan are not installed), so debug builds lean on
# _GLIBCXX_ASSERTIONS (libstdc++ container/iterator bounds checks) plus the
# PX_DEBUG invariant checks in order_book.cpp instead of a sanitizer. On a Linux
# box with sanitizer runtimes installed, add -fsanitize=address,undefined here.
DEBUG_FLAGS   = -O0 -g -DPX_DEBUG=1 -D_GLIBCXX_ASSERTIONS -fno-omit-frame-pointer
RELEASE_FLAGS = -O3 -march=native -flto -DNDEBUG

ifeq ($(OS),Windows_NT)
  PLATFORM_SRC := src/platform_win32.cpp
  LIBS_PLATFORM := -lws2_32
  EXE := .exe
else
  PLATFORM_SRC := src/platform_linux.cpp
  LIBS_PLATFORM := -lpthread
  EXE :=
endif

CORE_SRC := src/order_book.cpp src/matching_engine.cpp $(PLATFORM_SRC)
LIVE_SRC := live/tls_socket.cpp live/win_root_certs.cpp live/ws_client.cpp live/kalshi_auth.cpp live/kalshi_feed.cpp live/kalshi_rest.cpp
ifeq ($(OS),Windows_NT)
  LIVE_LIBS := -lssl -lcrypto -lcrypt32 $(LIBS_PLATFORM)
else
  LIVE_LIBS := -lssl -lcrypto $(LIBS_PLATFORM)
endif

OBJDIR_DEBUG   := build/debug
OBJDIR_RELEASE := build/release

# ---- debug objects (tests link against these) ------------------------------
DEBUG_OBJS := $(patsubst %.cpp,$(OBJDIR_DEBUG)/%.o,$(CORE_SRC))

# ---- release objects (bench/repl/live link against these) -----------------
RELEASE_OBJS := $(patsubst %.cpp,$(OBJDIR_RELEASE)/%.o,$(CORE_SRC))

$(OBJDIR_DEBUG)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) -c $< -o $@

$(OBJDIR_RELEASE)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) -c $< -o $@

# ---- tests ------------------------------------------------------------------
TEST_BINS := $(OBJDIR_DEBUG)/test_book$(EXE) $(OBJDIR_DEBUG)/test_engine$(EXE) $(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE)

$(OBJDIR_DEBUG)/test_book$(EXE): tests/test_book.cpp $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(DEBUG_OBJS) -o $@

$(OBJDIR_DEBUG)/test_engine$(EXE): tests/test_engine.cpp $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(DEBUG_OBJS) -o $@

$(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE): tests/fuzz_vs_reference.cpp $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(DEBUG_OBJS) -o $@

.PHONY: test
test: $(TEST_BINS)
	@echo "== test_book ==" && $(OBJDIR_DEBUG)/test_book$(EXE)
	@echo "== test_engine ==" && $(OBJDIR_DEBUG)/test_engine$(EXE)
	@echo "== fuzz_vs_reference ==" && $(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE)

# ---- benchmarks (release, no sanitizers, pinned) ----------------------------
BENCH_BINS := $(OBJDIR_RELEASE)/bench_engine$(EXE) $(OBJDIR_RELEASE)/bench_ring$(EXE)

$(OBJDIR_RELEASE)/bench_engine$(EXE): bench/bench_engine.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

$(OBJDIR_RELEASE)/bench_ring$(EXE): bench/bench_ring.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

.PHONY: bench
bench: $(BENCH_BINS)
	@echo "== bench_engine ==" && $(OBJDIR_RELEASE)/bench_engine$(EXE)
	@echo "== bench_ring ==" && $(OBJDIR_RELEASE)/bench_ring$(EXE)

# ---- repl --------------------------------------------------------------------
$(OBJDIR_RELEASE)/repl$(EXE): tools/repl.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

.PHONY: repl
repl: $(OBJDIR_RELEASE)/repl$(EXE)

# ---- strategy demo (Phase 4 — oracle feed -> signal -> engine, all local) --
$(OBJDIR_RELEASE)/strategy_demo$(EXE): tools/strategy_demo.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

.PHONY: strategy_demo
strategy_demo: $(OBJDIR_RELEASE)/strategy_demo$(EXE)

# ---- live (Kalshi connectivity — Phase 3, needs OpenSSL) --------------------
$(OBJDIR_RELEASE)/live/%.o: live/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) -c $< -o $@

LIVE_OBJS := $(patsubst %.cpp,$(OBJDIR_RELEASE)/%.o,$(LIVE_SRC))

$(OBJDIR_RELEASE)/kalshi_shadow$(EXE): tools/kalshi_shadow.cpp $(RELEASE_OBJS) $(LIVE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: live
live: $(OBJDIR_RELEASE)/kalshi_shadow$(EXE)

.PHONY: all
all: test bench repl strategy_demo

.PHONY: clean
clean:
	rm -rf build
