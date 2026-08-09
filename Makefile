# prediction-markets — build via mingw32-make (Windows/MSYS2 UCRT64) or make (Linux).
# No CMake/Ninja in this environment by design — see the plan doc for why.
#
# Targets:
#   make test         debug build (PX_DEBUG invariants + _GLIBCXX_ASSERTIONS), builds &
#                      runs the dependency-free unit/fuzz tests: core engine (Stage 1),
#                      MarketState/Strategy unit tests, the Brain/ShadowBook
#                      adversarial-input fuzz test (Stage 3), and Stage 5's hot-path
#                      allocation audit (test_alloc_audit)
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
#                      the gateway's dry-run structural guarantee, replay-pipeline
#                      determinism, full pipeline E2E (real threads/rings), hostile-event
#                      fuzzing (duplicate/reordered messages, races, reconnect backoff),
#                      market lifecycle (halt/resolution) gating, the Stage 5 workload
#                      generator's own correctness (test_workload_generator), and Stage 5
#                      reconnect/resync/Gateway-failure-path recovery testing
#                      (test_reconnect_recovery)
#   make bench-live   builds & runs bench_sign (RSA-PSS signing latency — the measured
#                      justification for keeping crypto off T2's thread),
#                      bench_pipeline_e2e (Stage 4: the full WS-frame -> ... ->
#                      OrderIntent decision path's real, measured latency), and
#                      bench_pipeline_stages (Stage 5: that same path broken into
#                      6 independently-attributed per-stage numbers)
#   make profile      Stage 5: gprof flat profile + call graph of bench_pipeline_stages,
#                      built with -pg (no LTO) — corroborates the hand-timed per-stage
#                      breakdown with a sampling/instrumenting profiler; output in
#                      perf/results/gprof_flat.txt and gprof_callgraph.txt
#   make bench-perf   Stage 5, Sections 4/5/6: builds bench_workload_throughput (CLI-
#                      driven throughput/saturation/backpressure/scaling tool — see
#                      perf/run_sweeps.py to actually drive it across sweep points) and
#                      bench_dispatch_scaling (Section 10: isolates
#                      dispatch_market_triggered's per-strategy-count cost)
#   make perf-regression  Stage 5, Section 7: re-runs bench_engine/bench_ring/
#                      bench_strategy with --csv=, diffs each against perf/baseline/'s
#                      frozen snapshot via perf/compare_baseline.py (tolerance-banded,
#                      not a hard pass/fail on tiny noise — see that script's own
#                      header comment)
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
TEST_BINS := $(OBJDIR_DEBUG)/test_book$(EXE) $(OBJDIR_DEBUG)/test_engine$(EXE) $(OBJDIR_DEBUG)/fuzz_vs_reference$(EXE) $(OBJDIR_DEBUG)/test_risk$(EXE) $(OBJDIR_DEBUG)/test_market_state$(EXE) $(OBJDIR_DEBUG)/test_strategies$(EXE) $(OBJDIR_DEBUG)/test_replay_fuzz$(EXE) $(OBJDIR_DEBUG)/test_alloc_audit$(EXE) $(OBJDIR_DEBUG)/test_eval_framework$(EXE)

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

# test_alloc_audit.cpp (Stage 5, Section 9): header-only, same as
# test_market_state.cpp/test_strategies.cpp above — no object link needed.
$(OBJDIR_DEBUG)/test_alloc_audit$(EXE): tests/test_alloc_audit.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $< -o $@

# test_eval_framework.cpp (Stage 6, Part B.3): eval/backtest_runner.hpp drives a
# real Brain synchronously (no threads/rings) — needs the same debug-flavored
# pipeline/brain.o test_replay_fuzz.cpp already links against, for the same reason
# (PX_DEBUG=1 makes ShadowBook::check_invariants() exist/assert). No OpenSSL.
$(OBJDIR_DEBUG)/test_eval_framework$(EXE): tests/test_eval_framework.cpp $(OBJDIR_DEBUG)/pipeline/brain.o
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
	@echo "== test_alloc_audit ==" && $(OBJDIR_DEBUG)/test_alloc_audit$(EXE)
	@echo "== test_eval_framework ==" && $(OBJDIR_DEBUG)/test_eval_framework$(EXE)

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

# ---- run_backtest (Stage 6, Part B.3/H — eval/ framework CLI, no OpenSSL) ---
# Only needs pipeline/brain.o (release-flavored, for speed across a real sweep of
# events) — same minimal-linking precedent tests/test_replay_fuzz.cpp already
# established for its debug-flavored equivalent: cpu_relax() is header-only
# (px/platform.hpp), and eval/ never touches OrderBook/MatchingEngine, so neither
# $(RELEASE_OBJS) nor a platform .o is needed.
$(OBJDIR_RELEASE)/run_backtest$(EXE): tools/run_backtest.cpp $(OBJDIR_RELEASE)/pipeline/brain.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(OBJDIR_RELEASE)/pipeline/brain.o -o $@

.PHONY: run_backtest
run_backtest: $(OBJDIR_RELEASE)/run_backtest$(EXE)

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

# test_pipeline_e2e.cpp (Stage 4): the full Ingress->Brain->Strategy->Risk->
# OrderIntent->Gateway->(simulated)REST->ExecReport->Brain loop, through real
# threads/rings — needs the same OpenSSL-linked objects test_replay_determinism
# does (Gateway/kalshi_rest), so it lives here rather than in the
# dependency-free `test` target.
$(OBJDIR_RELEASE)/test_pipeline_e2e$(EXE): tests/test_pipeline_e2e.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

# test_hostile_events.cpp (Stage 4): duplicate/reordered feed messages,
# partial fills, cancel/fill races, connection failure, risk rejection, an
# unrecognized message type — the same real-threads/real-rings methodology as
# test_pipeline_e2e.cpp, aimed at adversarial input instead of the happy path.
$(OBJDIR_RELEASE)/test_hostile_events$(EXE): tests/test_hostile_events.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

# test_lifecycle.cpp (Stage 4, second pass): market_lifecycle_v2 decoding,
# the event_type -> MarketStatus mapping, and Brain's halt/resolution gate.
$(OBJDIR_RELEASE)/test_lifecycle$(EXE): tests/test_lifecycle.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

# test_workload_generator.cpp (Stage 5, Section 3): proves workload::Generator's
# output actually parses and drives a real Ingress/Brain pair without hanging
# or crashing, across all three profiles.
$(OBJDIR_RELEASE)/test_workload_generator$(EXE): tests/test_workload_generator.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

# test_reconnect_recovery.cpp (Stage 5, Section 8): resync correctness after a
# reconnect-triggered snapshot, no premature strategy resumption, no duplicate
# orders across a resync, risk state untouched by market-data-only events, and
# the Gateway failure-path gap (orphaned pending orders) demonstrated directly.
$(OBJDIR_RELEASE)/test_reconnect_recovery$(EXE): tests/test_reconnect_recovery.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: test-live
test-live: $(OBJDIR_RELEASE)/test_feed_parse$(EXE) $(OBJDIR_RELEASE)/test_gateway_dry_run$(EXE) $(OBJDIR_RELEASE)/test_replay_determinism$(EXE) $(OBJDIR_RELEASE)/test_pipeline_e2e$(EXE) $(OBJDIR_RELEASE)/test_hostile_events$(EXE) $(OBJDIR_RELEASE)/test_lifecycle$(EXE) $(OBJDIR_RELEASE)/test_workload_generator$(EXE) $(OBJDIR_RELEASE)/test_reconnect_recovery$(EXE)
	@echo "== test_feed_parse ==" && $(OBJDIR_RELEASE)/test_feed_parse$(EXE)
	@echo "== test_gateway_dry_run ==" && $(OBJDIR_RELEASE)/test_gateway_dry_run$(EXE)
	@echo "== test_replay_determinism ==" && $(OBJDIR_RELEASE)/test_replay_determinism$(EXE)
	@echo "== test_pipeline_e2e ==" && $(OBJDIR_RELEASE)/test_pipeline_e2e$(EXE)
	@echo "== test_workload_generator ==" && $(OBJDIR_RELEASE)/test_workload_generator$(EXE)
	@echo "== test_reconnect_recovery ==" && $(OBJDIR_RELEASE)/test_reconnect_recovery$(EXE)
	@echo "== test_hostile_events ==" && $(OBJDIR_RELEASE)/test_hostile_events$(EXE)
	@echo "== test_lifecycle ==" && $(OBJDIR_RELEASE)/test_lifecycle$(EXE)

# ---- bench-live (OpenSSL-dependent benchmarks) ------------------------------
# bench_sign only needs kalshi_auth.o directly (no Ingress/Brain involved).
$(OBJDIR_RELEASE)/bench_sign$(EXE): bench/bench_sign.cpp $(OBJDIR_RELEASE)/live/kalshi_auth.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(OBJDIR_RELEASE)/live/kalshi_auth.o -o $@ $(LIVE_LIBS)

# bench_pipeline_e2e.cpp (Stage 4): the full WS-frame -> parse -> shadow-book
# -> features -> strategy -> risk -> OrderIntent path, through a real
# Ingress/Brain pair — needs $(LIVE_OBJS)/$(PIPELINE_OBJS) (defined above, in
# the ---- live ---- section), which is why this whole block sits after
# test-live rather than immediately after the dependency-free `bench` target.
$(OBJDIR_RELEASE)/bench_pipeline_e2e$(EXE): bench/bench_pipeline_e2e.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

# bench_pipeline_stages.cpp (Stage 5, Section 2): the same logical path as
# bench_pipeline_e2e, broken into 6 independently-attributed stage numbers
# instead of one end-to-end one — see the file's own header comment for why
# that's done single-threaded rather than by timestamping across the real
# Ingress/Brain thread boundary.
$(OBJDIR_RELEASE)/bench_pipeline_stages$(EXE): bench/bench_pipeline_stages.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: bench-live
bench-live: $(OBJDIR_RELEASE)/bench_sign$(EXE) $(OBJDIR_RELEASE)/bench_pipeline_e2e$(EXE) $(OBJDIR_RELEASE)/bench_pipeline_stages$(EXE)
	@echo "== bench_sign ==" && $(OBJDIR_RELEASE)/bench_sign$(EXE)
	@echo "== bench_pipeline_e2e ==" && $(OBJDIR_RELEASE)/bench_pipeline_e2e$(EXE)
	@echo "== bench_pipeline_stages ==" && $(OBJDIR_RELEASE)/bench_pipeline_stages$(EXE)

# ---- bench-perf (Stage 5, Sections 4/5/6: throughput/saturation, backpressure,
# multi-market scaling — one CLI-configurable tool, driven many times over by
# perf/run_sweeps.py rather than run once like the bench-live targets above) --
$(OBJDIR_RELEASE)/bench_workload_throughput$(EXE): bench/bench_workload_throughput.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

$(OBJDIR_RELEASE)/bench_dispatch_scaling$(EXE): bench/bench_dispatch_scaling.cpp $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $< $(RELEASE_OBJS) $(LIVE_OBJS) $(PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: bench-perf
bench-perf: $(OBJDIR_RELEASE)/bench_workload_throughput$(EXE) $(OBJDIR_RELEASE)/bench_dispatch_scaling$(EXE)
	@echo "built $(OBJDIR_RELEASE)/bench_workload_throughput$(EXE) -- see perf/run_sweeps.py to drive it"
	@echo "== bench_dispatch_scaling ==" && $(OBJDIR_RELEASE)/bench_dispatch_scaling$(EXE)

# ---- profile (Stage 5, Section 2: gprof, corroborating the hand-timed
# per-stage breakdown above with a sampling/instrumenting profiler instead of
# hand-rolled TSC brackets) --------------------------------------------------
# No -flto here deliberately: LTO and gprof instrumentation don't reliably
# cooperate (symbols/call-graph edges get folded away), so this is its own
# object set, not a reuse of $(OBJDIR_RELEASE). -O2, not -O3/-O0: representative
# of real optimized code (unlike -O0) while still leaving enough of the call
# graph intact for gprof to attribute (unlike -flto's aggressive inlining).
OBJDIR_PROFILE := build/profile
PROFILE_FLAGS = -O2 -g -pg -fno-omit-frame-pointer -DNDEBUG

$(OBJDIR_PROFILE)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(PROFILE_FLAGS) -c $< -o $@

PROFILE_CORE_OBJS := $(patsubst %.cpp,$(OBJDIR_PROFILE)/%.o,$(CORE_SRC))
PROFILE_LIVE_OBJS := $(patsubst %.cpp,$(OBJDIR_PROFILE)/%.o,$(LIVE_SRC))
PROFILE_PIPELINE_OBJS := $(patsubst %.cpp,$(OBJDIR_PROFILE)/%.o,$(PIPELINE_SRC))

$(OBJDIR_PROFILE)/bench_pipeline_stages$(EXE): bench/bench_pipeline_stages.cpp $(PROFILE_CORE_OBJS) $(PROFILE_LIVE_OBJS) $(PROFILE_PIPELINE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(PROFILE_FLAGS) -pg $< $(PROFILE_CORE_OBJS) $(PROFILE_LIVE_OBJS) $(PROFILE_PIPELINE_OBJS) -o $@ $(LIVE_LIBS)

.PHONY: profile
profile: $(OBJDIR_PROFILE)/bench_pipeline_stages$(EXE)
	@mkdir -p perf/results
	cd $(OBJDIR_PROFILE) && ./bench_pipeline_stages$(EXE)
	gprof $(OBJDIR_PROFILE)/bench_pipeline_stages$(EXE) $(OBJDIR_PROFILE)/gmon.out --flat-profile > perf/results/gprof_flat.txt
	gprof $(OBJDIR_PROFILE)/bench_pipeline_stages$(EXE) $(OBJDIR_PROFILE)/gmon.out --graph > perf/results/gprof_callgraph.txt
	@echo "gprof flat profile:    perf/results/gprof_flat.txt"
	@echo "gprof call graph:      perf/results/gprof_callgraph.txt"

# ---- perf-regression (Stage 5, Section 7) -----------------------------------
# Re-runs the three dependency-free benches with --csv pointed at
# perf/results/, then diffs each against its frozen perf/baseline/ snapshot
# via perf/compare_baseline.py. Needs Python (any python3 on PATH); not a
# hard CI gate by itself -- see compare_baseline.py's own header comment for
# why a flagged regression doesn't fail the build outright.
.PHONY: perf-regression
perf-regression: $(BENCH_BINS)
	@mkdir -p perf/results
	$(OBJDIR_RELEASE)/bench_engine$(EXE) --csv=perf/results/current_bench_engine.csv > /dev/null
	$(OBJDIR_RELEASE)/bench_ring$(EXE) --csv=perf/results/current_bench_ring.csv > /dev/null
	$(OBJDIR_RELEASE)/bench_strategy$(EXE) --csv=perf/results/current_bench_strategy.csv > /dev/null
	python perf/compare_baseline.py perf/baseline/bench_engine.csv perf/results/current_bench_engine.csv
	python perf/compare_baseline.py perf/baseline/bench_ring.csv perf/results/current_bench_ring.csv
	python perf/compare_baseline.py perf/baseline/bench_strategy.csv perf/results/current_bench_strategy.csv

.PHONY: all
all: test bench repl strategy_demo

.PHONY: clean
clean:
	rm -rf build
