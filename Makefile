# Matching engine — build with mingw32-make (Windows, MSYS2 UCRT64) or make (Linux).
#
#   make test     debug build (-Og, PX_DEBUG invariants, _GLIBCXX_ASSERTIONS); builds and runs every test
#   make bench    release build (-O3 -march=native -flto); builds and runs every benchmark
#   make latency  builds and runs only bench_matching_latency (the 1M-order headline number)
#   make repl     release build of the interactive REPL
#   make compare CANDIDATES=a,b   the standard experiment (docs/RUNNER.md); see "strategies of your own"
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
# Objects go in their own subtree, binaries at the top of DBG/REL. They have to
# be separated: object paths mirror the source tree, so without this the arena
# viewer's binary ($(REL)/arena) and its objects ($(REL)/arena/*.o) are the same
# path. On Windows the .exe suffix hides the clash; on Linux the directory wins
# and the viewer cannot be linked at all.
DOBJ := $(DBG)/obj
ROBJ := $(REL)/obj

LIB_SRC := src/matching_engine.cpp $(PLATFORM_SRC)
LIB_DBG := $(LIB_SRC:%.cpp=$(DOBJ)/%.o)
LIB_REL := $(LIB_SRC:%.cpp=$(ROBJ)/%.o)

TESTS   := test_containers test_spsc_ring test_engine fuzz_vs_reference test_alloc_audit test_determinism \
           test_arena
BENCHES := bench_engine bench_ring bench_matching_latency

TEST_BINS  := $(TESTS:%=$(DBG)/%$(EXE))
BENCH_BINS := $(BENCHES:%=$(REL)/%$(EXE))
REPL_BIN   := $(REL)/repl$(EXE)
# The arena: the headless experiment runner and the terminal viewer. Both are
# consumers of the same MatchingEngine the benchmarks measure, so neither can
# affect a latency number.
ARENA_BINS := $(REL)/sweep$(EXE) $(REL)/arena$(EXE) $(REL)/suite$(EXE)

.PHONY: all test bench latency repl arena sweep clean

all: test test-cli test-lab $(BENCH_BINS) $(REPL_BIN) $(ARENA_BINS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# The sweep runner's command line, checked the only way a command line can be:
# by running it. A bad field, flag or value must fail loudly (exit 2) and a
# configuration that could never terminate must be refused, not hang. These were
# all verified by hand once; this target is so they stay verified.
.PHONY: test-cli
test-cli: $(REL)/sweep$(EXE) $(REL)/suite$(EXE)
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
	! ./$< --values=1000 --seeds=1 --requests=500 --strategies=maker,nonsense --quiet > /dev/null 2>&1 \
	    || fail "an unknown participant name should fail"; \
	! ./$< --values=1000 --seeds=1 --requests=500 --strategies=maker,noise:x --quiet > /dev/null 2>&1 \
	    || fail "a non-numeric participant count should fail"; \
	! ./$< --values=1000 --seeds=1 --requests=500 --strategies=noise:2 --quiet > /dev/null 2>&1 \
	    || fail "a line-up with no maker should fail"; \
	./$< --values=1000 --seeds=1 --requests=2000 --strategies=maker,rival,informed,noise:2 --quiet > /dev/null \
	    || fail "a named line-up should succeed"; \
	./$< --values=1000 --seeds=1 --requests=2000 --quiet > /dev/null || fail "a normal sweep should succeed"; \
	echo "11/11 command-line checks passed"; \
	echo "== suite command line"; \
	S=./$(REL)/suite$(EXE); W=./$(REL)/sweep$(EXE); D=$(BUILD)/cli; \
	exits() { want=$$1; shift; rc=0; "$$@" > /dev/null 2>&1 || rc=$$?; [ $$rc -eq $$want ]; }; \
	files="suite.cfg sessions.csv participants.csv comparison.csv report.txt outcome.cfg"; \
	same() { for f in $$files; do cmp -s $$1/$$f $$2/$$f || return 1; done; }; \
	placebo42() { awk -F, 'NR==1{for(i=1;i<=NF;i++) c[$$i]=i; next} \
	    $$c["arm"]=="placebo" && $$c["seed"]==42 {print $$c["session_hash"]}' $$1/sessions.csv; }; \
	sweephash() { $$W "$$@" --quiet | sed -n 's/.*session hash at seed 42, per point: *//p'; }; \
	rm -rf $$D; mkdir -p $$D/keep; echo mine > $$D/keep/mine.txt; \
	$$S --help > /dev/null || fail "suite --help should succeed"; \
	exits 2 $$S || fail "no --candidates should exit 2"; \
	exits 2 $$S --candidates=nonsense || fail "an unknown candidate should exit 2"; \
	exits 2 $$S --candidates=informed || fail "the informed trader cannot sit in the seat"; \
	exits 2 $$S --candidates=placebo || fail "the placebo is not a candidate"; \
	exits 2 $$S --candidates=maker,maker || fail "a candidate named twice should exit 2"; \
	exits 2 $$S --candidates=maker --block=bogus || fail "an unknown block should exit 2"; \
	exits 2 $$S --candidates=maker --set=maker.quote_qty=1 || fail "the conditions cannot be changed"; \
	exits 2 $$S --block=smoke --candidates=maker --out=$$D/keep || fail "a foreign directory should be refused"; \
	[ "$$(ls $$D/keep)" = mine.txt ] && [ "$$(cat $$D/keep/mine.txt)" = mine ] \
	    || fail "a refused directory must be left untouched"; \
	$$S --block=smoke --candidates=maker,momentum --out=$$D/suite-a > /dev/null 2>&1 || fail "a smoke run should succeed"; \
	for f in $$files; do [ -f $$D/suite-a/$$f ] || fail "a run should write $$f"; done; \
	head -n1 $$D/suite-a/sessions.csv | grep -q '^suite,version,block,seed,arm,origin,' || fail "sessions.csv header"; \
	head -n1 $$D/suite-a/participants.csv | grep -q '^suite,version,block,seed,arm,index,' || fail "participants.csv header"; \
	grep -qx 'status=complete' $$D/suite-a/outcome.cfg || fail "outcome.cfg should say status=complete"; \
	$$S --block=smoke --candidates=maker,momentum --out=$$D/suite-b > /dev/null 2>&1 || fail "a second smoke run should succeed"; \
	same $$D/suite-a $$D/suite-b || fail "two runs should write byte-identical files"; \
	$$S --block=smoke --candidates=maker,momentum --out=$$D/suite-a > /dev/null 2>&1 || fail "a previous run should be replaced"; \
	same $$D/suite-a $$D/suite-b || fail "a replaced run should still match"; \
	ref=$$(placebo42 $$D/suite-a); \
	ev=$$(sweephash --values=10000 --seeds=1 --set=session.max_time_ns=25000000 \
	    --set=session.max_requests=0 --set=session.stall_limit_ns=0); \
	[ -n "$$ref" ] && [ "$$ref" = "$$ev" ] || fail "the placebo arm ($$ref) should be the sweep's market ($$ev)"; \
	bare=$$(sweephash --values=10000 --seeds=1 --requests=2000 --strategies=maker,rival,informed,noise:2,momentum); \
	seated=$$(sweephash --values=10000 --seeds=1 --requests=2000 --strategies=maker,rival,informed,noise:2,momentum,placebo); \
	[ -n "$$bare" ] && [ "$$bare" = "$$seated" ] || fail "an appended placebo should not move the sweep's hash"; \
	$$S --candidates=momentum --out=$$D/suite-std > /dev/null 2>&1 || fail "the standard block should run (exit 3: reference not admissible)"; \
	grep -qx 'reference_admissible=1' $$D/suite-std/outcome.cfg || fail "the reference should be admissible"; \
	! grep -nwE 'double|float' arena/suite.hpp || fail "arena/suite.hpp must stay integer-only"; \
	rm -rf $$D; \
	echo "  placebo arm at seed 42: $$ref, the sweep's market"; \
	echo "17/17 suite command-line checks passed"

bench: $(BENCH_BINS)
	@set -e; for b in $(BENCH_BINS); do echo "== $$b"; ./$$b; done

latency: $(REL)/bench_matching_latency$(EXE)
	./$<

repl: $(REPL_BIN)

arena: $(ARENA_BINS)

sweep: $(REL)/sweep$(EXE)
	./$<

$(DOBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $(DEPFLAGS) -c $< -o $@

$(ROBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BINS): $(DBG)/%$(EXE): $(DOBJ)/tests/%.o $(LIB_DBG)
	$(CXX) $(DEBUG_FLAGS) $^ -o $@ $(LDLIBS)

$(BENCH_BINS): $(REL)/%$(EXE): $(ROBJ)/bench/%.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(REPL_BIN): $(ROBJ)/tools/repl.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(ARENA_BINS): $(REL)/%$(EXE): $(ROBJ)/arena/%.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

# ---- strategies of your own ------------------------------------------------
#
#   make strategy STRATEGY=path/to/name.hpp   install it and build the lab binaries
#   make lab                                  rebuild the lab binaries
#   make test-lab                             check the whole path with the example
#
# A strategy is a header (see examples/strategies/imbalance.hpp for the
# convention). Installing copies it into arena/strategies/user/, which git
# ignores, and the manifest that wires every installed header into the registry
# is generated into the build tree. Only sweep_lab and arena_lab are compiled
# with PX_ARENA_USER_STRATEGIES, from their own object subtree, so sweep, arena
# and every test never contain an installed strategy: the published evidence
# and the pinned hashes cannot be touched by one. Nothing here changes the engine.
USER_STRATEGY_DIR := arena/strategies/user
GEN      := $(BUILD)/generated
MANIFEST := $(GEN)/px_user_strategies.hpp
LOBJ     := $(REL)/obj-lab
LAB_BINS := $(REL)/sweep_lab$(EXE) $(REL)/arena_lab$(EXE) $(REL)/suite_lab$(EXE)
LAB_FLAGS := -DPX_ARENA_USER_STRATEGIES -I$(GEN)
# Read from the registry itself, so an installed name can never shadow a built-in.
BUILTIN_STRATEGIES := $(shell sed -n 's/.*kStrategyNames = "\(.*\)";/\1/p' arena/strategies/registry.hpp)

.PHONY: strategy lab test-lab compare FORCE

strategy:
	@set -e; f='$(STRATEGY)'; \
	refuse() { echo "make strategy: $$1" >&2; exit 2; }; \
	[ -n "$$f" ] || refuse "say which header: make strategy STRATEGY=path/to/name.hpp"; \
	[ -f "$$f" ] || refuse "'$$f' is not a file"; \
	case "$$f" in *.hpp) ;; *) refuse "'$$f' is not a .hpp header";; esac; \
	n=$$(basename "$$f" .hpp); \
	echo "$$n" | grep -Eq '^[a-z][a-z0-9_]*$$' \
	    || refuse "'$$n' must be lowercase letters, digits and underscores, starting with a letter"; \
	for b in $(BUILTIN_STRATEGIES) registry script user; do \
	    [ "$$n" != "$$b" ] || refuse "'$$n' is already a built-in name; rename the file"; \
	done; \
	cp "$$f" $(USER_STRATEGY_DIR)/$$n.hpp; \
	echo "installed $$n -> $(USER_STRATEGY_DIR)/$$n.hpp"
	@$(MAKE) --no-print-directory lab
	@n=$$(basename '$(STRATEGY)' .hpp); \
	echo; echo "compare it: make compare CANDIDATES=$$n"; \
	echo "explore it: $(REL)/sweep_lab --strategies=maker,rival,informed,noise:2,momentum,$$n --seeds=8"; \
	echo "watch it:   $(REL)/arena_lab --strategies=maker,rival,informed,noise:2,momentum,$$n"

lab: $(LAB_BINS)

# The standard experiment (docs/RUNNER.md): every candidate in the same seat of
# the same market on the same named block of seeds, written to runs/.
#   make compare CANDIDATES=mine,other [BLOCK=smoke] [OUT=dir]
# BLOCK rather than SEEDS, because bench/run_arena_evidence.sh reads a numeric
# SEEDS from the environment and an exported one would leak in here.
compare: $(REL)/suite_lab$(EXE)
	@[ -n '$(CANDIDATES)' ] || { echo "make compare: say which strategies: make compare CANDIDATES=a[,b,...]" >&2; exit 2; }
	./$(REL)/suite_lab$(EXE) --candidates=$(CANDIDATES) $(if $(BLOCK),--block=$(BLOCK)) $(if $(OUT),--out=$(OUT))

# Regenerated on every lab build but only rewritten when its contents change, so
# an unchanged set of strategies does not relink anything.
$(MANIFEST): FORCE
	@mkdir -p $(GEN)
	@set -e; t=$@.tmp; names=; \
	{ echo "// Generated by make from $(USER_STRATEGY_DIR)/*.hpp. Do not edit; never committed."; \
	  echo "#pragma once"; echo; \
	  for h in $$(ls $(USER_STRATEGY_DIR)/*.hpp 2>/dev/null | sort); do echo "#include \"$$h\""; done; \
	  echo; echo "namespace px::arena {"; echo; \
	  echo "[[nodiscard]] inline bool add_user_strategy(std::string_view name, Roster& r) {"; \
	  for h in $$(ls $(USER_STRATEGY_DIR)/*.hpp 2>/dev/null | sort); do \
	    n=$$(basename $$h .hpp); t_=$$(echo $$n | sed -E 's/(^|_)([a-z0-9])/\U\2/g'); \
	    echo "    if (name == \"$$n\") {"; \
	    echo "        strategies::$$t_ s;"; \
	    echo "        s.name = \"$$n\";"; \
	    echo "        r.strategies.push_back(make_strategy(s));"; \
	    echo "        r.informed.push_back(false); // never informed: only the roster grants that"; \
	    echo "        return true;"; echo "    }"; \
	  done; \
	  echo "    (void)name; (void)r;"; echo "    return false;"; echo "}"; echo; \
	  names=$$(for h in $$(ls $(USER_STRATEGY_DIR)/*.hpp 2>/dev/null | sort); do basename $$h .hpp; done | tr '\n' ' '); \
	  echo "inline constexpr const char* kUserStrategyNames = \"$${names% }\";"; \
	  echo; echo "} // namespace px::arena"; } > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

FORCE:

$(LOBJ)/%.o: %.cpp $(MANIFEST)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(LAB_FLAGS) $(RELEASE_FLAGS) $(DEPFLAGS) -c $< -o $@

$(LAB_BINS): $(REL)/%_lab$(EXE): $(LOBJ)/arena/%.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

# The whole path, run the way a newcomer would: refusals first, then install the
# example, score it, and confirm the evidence binary cannot see it.
test-lab: $(REL)/sweep$(EXE) $(REL)/suite$(EXE)
	@set -e; \
	fail() { echo "FAIL: $$1"; exit 1; }; \
	echo "== strategy install"; \
	! $(MAKE) --no-print-directory strategy > /dev/null 2>&1 || fail "no STRATEGY should be refused"; \
	! $(MAKE) --no-print-directory strategy STRATEGY=no/such.hpp > /dev/null 2>&1 || fail "a missing file should be refused"; \
	! $(MAKE) --no-print-directory strategy STRATEGY=Makefile > /dev/null 2>&1 || fail "a non-header should be refused"; \
	! $(MAKE) --no-print-directory strategy STRATEGY=arena/strategies/fader.hpp > /dev/null 2>&1 \
	    || fail "a built-in name should be refused"; \
	$(MAKE) --no-print-directory strategy STRATEGY=examples/strategies/imbalance.hpp > /dev/null \
	    || fail "the example should install and build"; \
	out=$$(./$(REL)/sweep_lab --values=1000 --seeds=2 --requests=20000 --replay-check \
	    --strategies=maker,rival,informed,noise:2,imbalance --quiet) || fail "sweep_lab should score the example"; \
	echo "$$out" | grep -E '^ +[0-9]+ +\| imbalance ' > /dev/null || fail "the example should have a row"; \
	fills=$$(echo "$$out" | awk -F'|' '$$2 ~ /imbalance/ {gsub(/ /,"",$$5); print $$5}'); \
	[ "$$fills" -gt 0 ] || fail "the example should trade (fills: $$fills)"; \
	echo "$$out" | grep -q 'hashes identical' || fail "a scored run should replay identically"; \
	! ./$(REL)/sweep$(EXE) --values=1000 --seeds=1 --requests=2000 \
	    --strategies=maker,imbalance --quiet > /dev/null 2>&1 || fail "the evidence binary must not see it"; \
	echo "  imbalance: $$fills fills (median of 2 seeds)"; \
	echo "7/7 strategy-install checks passed"; \
	echo "== suite with an installed strategy"; \
	D=$(BUILD)/lab; rm -rf $$D; \
	fp() { sed -n 's/^fingerprint=//p' $$1/outcome.cfg; }; \
	./$(REL)/suite_lab$(EXE) --block=smoke --candidates=imbalance,maker --out=$$D/suite > /dev/null 2>&1 \
	    || fail "suite_lab should run the example"; \
	grep -q 'maker vs imbalance' $$D/suite/report.txt || fail "the report should pair maker against imbalance"; \
	awk -F, 'NR==1{for(i=1;i<=NF;i++) c[$$i]=i; next} $$c["arm"]=="imbalance" {n++; if ($$c["origin"]!="installed") bad++} \
	    END {exit !(n > 0 && bad == 0)}' $$D/suite/sessions.csv || fail "imbalance rows should say origin=installed"; \
	./$(REL)/suite$(EXE) --block=smoke --candidates=maker --out=$$D/evidence > /dev/null 2>&1 \
	    || fail "suite should run a built-in candidate"; \
	[ -n "$$(fp $$D/suite)" ] && [ "$$(fp $$D/suite)" = "$$(fp $$D/evidence)" ] \
	    || fail "suite_lab and suite should share one reference market"; \
	rc=0; ./$(REL)/suite$(EXE) --block=smoke --candidates=imbalance --out=$$D/no > /dev/null 2>&1 || rc=$$?; \
	[ $$rc -eq 2 ] || fail "the evidence binary's suite must not see it (exit $$rc, want 2)"; \
	$(MAKE) --no-print-directory compare CANDIDATES=imbalance BLOCK=smoke OUT=$$D/compare > /dev/null 2>&1 \
	    || fail "make compare should run it"; \
	echo "  fingerprint $$(fp $$D/suite), identical in suite and suite_lab"; \
	rm -rf $$D; \
	echo "4/4 suite checks passed"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(DOBJ)/*/*.d $(ROBJ)/*/*.d $(DOBJ)/*/*/*.d $(ROBJ)/*/*/*.d $(LOBJ)/*/*.d)
