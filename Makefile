# prediction-markets — build via mingw32-make (Windows/MSYS2 UCRT64) or make (Linux).
# No CMake/Ninja in this environment by design — see the plan doc for why.
#
# Targets:
#   make test         debug build (PX_DEBUG invariants + _GLIBCXX_ASSERTIONS), builds &
#                      runs the dependency-free unit/fuzz tests: core engine (Stage 1),
#                      MarketState/Strategy unit tests, and the Brain/ShadowBook
#                      adversarial-input fuzz test (Stage 3)
#   make bench        release build (-O3 -march=native -flto), builds & runs the
#                      dependency-free benchmarks: core engine (Stage 1) and
#                      Strategy::on_event/extract_features latency (Stage 3)
#   make repl         release build of the interactive REPL
#   make strategy_demo  release build of the local oracle->strategy->engine demo (Phase 4,
#                      ported onto the Strategy interface in Stage 3)
#   make live         release build of every Kalshi connectivity tool (needs OpenSSL):
#                      kalshi_shadow (single-market shadow book) and kalshi_pipeline
#                      (the full Stage 2 3-thread/3-ring live pipeline, now dispatching
#                      to registered Strategy objects — see pipeline/brain.hpp)
#   make test-live    builds & runs the OpenSSL-dependent tests: feed schema parsing,
#                      the gateway's dry-run structural guarantee, and the replay
#                      pipeline's determinism property
#   make bench-live   builds & runs bench_sign (RSA-PSS signing latency — the measured
#                      justification for keeping crypto off T2's thread)
#   make all          test + bench + repl + strategy_demo (the dependency-free set)
#   make clean

CXX      ?= g++
INCLUDES  = -Iinclude -Ilive -Irisk -Ipipeline -I.
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
LIVE_SRC := live/tls_socket.cpp live/win_root_certs.cpp live/ws_client.cpp live/kalshi_auth.cpp live/kalshi_feed.cpp live/kalshi_rest.cpp live/kalshi_gateway.cpp
PIPELINE_SRC := pipeline/ingress.cpp pipeline/brain.cpp
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
TEST_BINS := $(OBJDIR_DEBUG)/test_book$(EXE) $(OBJDIR_DEBUG)/test_engine$(EXE) $(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE) $(OBJDIR_DEBUG)/test_risk$(EXE) $(OBJDIR_DEBUG)/test_market_state$(EXE) $(OBJDIR_DEBUG)/test_strategies$(EXE) $(OBJDIR_DEBUG)/test_replay_fuzz$(EXE)

$(OBJDIR_DEBUG)/test_book$(EXE): tests/test_book.cpp $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(DEBUG_OBJS) -o $@

$(OBJDIR_DEBUG)/test_engine$(EXE): tests/test_engine.cpp $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(DEBUG_OBJS) -o $@

$(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE): tests/fuzz_vs_reference.cpp $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(DEBUG_OBJS) -o $@

# test_risk.cpp only needs risk/risk_engine.hpp (header-only, no engine
# dependency) — no $(DEBUG_OBJS) link needed.
$(OBJDIR_DEBUG)/test_risk$(EXE): tests/test_risk.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< -o $@

# test_market_state.cpp / test_strategies.cpp (Stage 3): ShadowBook +
# strategy/*.hpp are all header-only (no strategy/*.cpp exists) — no object
# link needed, same as test_risk above.
$(OBJDIR_DEBUG)/test_market_state$(EXE): tests/test_market_state.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< -o $@

$(OBJDIR_DEBUG)/test_strategies$(EXE): tests/test_strategies.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< -o $@

# test_replay_fuzz.cpp (Stage 3) needs a real Brain — pipeline/brain.cpp, debug-
# flavored so PX_DEBUG=1 makes ShadowBook::check_invariants() exist/assert (the
# release-flavored $(OBJDIR_RELEASE)/pipeline/%.o rule below, used by
# test-live, builds with -DNDEBUG instead, which would compile it out). Brain
# itself needs no OpenSSL/CORE_SRC — pipeline/ingress.hpp is pulled in only for
# the RingA type alias, never instantiated — so this stays in the
# dependency-free `test` target, not test-live.
$(OBJDIR_DEBUG)/pipeline/%.o: pipeline/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) -c $< -o $@

$(OBJDIR_DEBUG)/test_replay_fuzz$(EXE): tests/test_replay_fuzz.cpp $(OBJDIR_DEBUG)/pipeline/brain.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< $(OBJDIR_DEBUG)/pipeline/brain.o -o $@

.PHONY: test
test: $(TEST_BINS)
	@echo "== test_book ==" && $(OBJDIR_DEBUG)/test_book$(EXE)
	@echo "== test_engine ==" && $(OBJDIR_DEBUG)/test_engine$(EXE)
	@echo "== fuzz_vs_reference ==" && $(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE)
	@echo "== test_risk ==" && $(OBJDIR_DEBUG)/test_risk$(EXE)
	@echo "== test_market_state ==" && $(OBJDIR_DEBUG)/test_market_state$(EXE)
	@echo "== test_strategies ==" && $(OBJDIR_DEBUG)/test_strategies$(EXE)
	@echo "== test_replay_fuzz ==" && $(OBJDIR_DEBUG)/test_replay_fuzz$(EXE)

# ---- benchmarks (release, no sanitizers, pinned) ----------------------------
BENCH_BINS := $(OBJDIR_RELEASE)/bench_engine$(EXE) $(OBJDIR_RELEASE)/bench_ring$(EXE) $(OBJDIR_RELEASE)/bench_strategy$(EXE)

$(OBJDIR_RELEASE)/bench_engine$(EXE): bench/bench_engine.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

# bench_strategy.cpp (Stage 3) only needs px/platform.hpp's pin_current_thread_to
# (implemented in $(PLATFORM_SRC)) — strategy/*.hpp is all header-only, no
# OrderBook/MatchingEngine involved, so this links against $(RELEASE_OBJS)
# only for that one platform object (the rest of $(RELEASE_OBJS) goes unused
# but costs nothing to link in, same as repl/strategy_demo already do).
$(OBJDIR_RELEASE)/bench_strategy$(EXE): bench/bench_strategy.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

$(OBJDIR_RELEASE)/bench_ring$(EXE): bench/bench_ring.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

.PHONY: bench
bench: $(BENCH_BINS)
	@echo "== bench_engine ==" && $(OBJDIR_RELEASE)/bench_engine$(EXE)
	@echo "== bench_ring ==" && $(OBJDIR_RELEASE)/bench_ring$(EXE)
	@echo "== bench_strategy ==" && $(OBJDIR_RELEASE)/bench_strategy$(EXE)

# bench_sign needs OpenSSL (kalshi_auth.cpp) — kept out of the dependency-free
# `bench` target above, alongside `live`/`test-live`.
$(OBJDIR_RELEASE)/bench_sign$(EXE): bench/bench_sign.cpp $(OBJDIR_RELEASE)/live/kalshi_auth.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(OBJDIR_RELEASE)/live/kalshi_auth.o -o $@ $(LIVE_LIBS)

.PHONY: bench-live
bench-live: $(OBJDIR_RELEASE)/bench_sign$(EXE)
	@echo "== bench_sign ==" && $(OBJDIR_RELEASE)/bench_sign$(EXE)

# ---- repl --------------------------------------------------------------------
$(OBJDIR_RELEASE)/repl$(EXE): tools/repl.cpp $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) -o $@ $(LIBS_PLATFORM)

.PHONY: repl
repl: $(OBJDIR_RELEASE)/repl$(EXE)

# ---- strategy demo (Phase 4 — oracle feed -> strategy -> engine, all local) --
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

$(OBJDIR_RELEASE)/pipeline/%.o: pipeline/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) -c $< -o $@

PIPELINE_OBJS := $(patsubst %.cpp,$(OBJDIR_RELEASE)/%.o,$(PIPELINE_SRC))

$(OBJDIR_RELEASE)/kalshi_shadow$(EXE): tools/kalshi_shadow.cpp $(RELEASE_OBJS) $(LIVE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) -o $@ $(LIVE_LIBS)

$(OBJDIR_RELEASE)/kalshi_pipeline$(EXE): tools/kalshi_pipeline.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: live
live: $(OBJDIR_RELEASE)/kalshi_shadow$(EXE) $(OBJDIR_RELEASE)/kalshi_pipeline$(EXE)

# ---- live tests (need OpenSSL/Winsock, so separate from the dependency-free
# `test` target above; parse_feed_message/SeqTracker themselves touch neither
# a socket nor OpenSSL, but they share a translation unit with code that does) -
$(OBJDIR_RELEASE)/test_feed_parse$(EXE): tests/test_feed_parse.cpp $(RELEASE_OBJS) $(LIVE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) -o $@ $(LIVE_LIBS)

$(OBJDIR_RELEASE)/test_gateway_dry_run$(EXE): tests/test_gateway_dry_run.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

$(OBJDIR_RELEASE)/test_replay_determinism$(EXE): tests/test_replay_determinism.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: test-live
test-live: $(OBJDIR_RELEASE)/test_feed_parse$(EXE) $(OBJDIR_RELEASE)/test_gateway_dry_run$(EXE) $(OBJDIR_RELEASE)/test_replay_determinism$(EXE)
	@echo "== test_feed_parse ==" && $(OBJDIR_RELEASE)/test_feed_parse$(EXE)
	@echo "== test_gateway_dry_run ==" && $(OBJDIR_RELEASE)/test_gateway_dry_run$(EXE)
	@echo "== test_replay_determinism ==" && $(OBJDIR_RELEASE)/test_replay_determinism$(EXE)

.PHONY: all
all: test bench repl strategy_demo

.PHONY: clean
clean:
	rm -rf build
