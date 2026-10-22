# Matching engine — build with mingw32-make (Windows, MSYS2 UCRT64) or make (Linux).
#
#   make app      the terminal app, build/release/exchange-lab, and every program it runs (docs/APP.md)
#   make test     debug build (-Og, PX_DEBUG invariants, _GLIBCXX_ASSERTIONS); builds and runs every test
#   make bench    release build (-O3 -march=native -flto); builds and runs every benchmark
#   make latency  builds and runs only bench_matching_latency (the 1M-order headline number)
#   make repl     release build of the interactive REPL
#   make compare CANDIDATES=a,b   the standard experiment (docs/RUNNER.md); see "strategies of your own"
#   make reproduce RUN=runs/x     re-run a run from its suite.cfg and compare every file (docs/REPRODUCE.md)
#   make controls                 the evaluator's own controls on the standard block (docs/CONTROLS.md)
#   make test-app the app driven the way a person would, through its commands
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
           test_arena test_app
BENCHES := bench_engine bench_ring bench_matching_latency

TEST_BINS  := $(TESTS:%=$(DBG)/%$(EXE))
BENCH_BINS := $(BENCHES:%=$(REL)/%$(EXE))
REPL_BIN   := $(REL)/repl$(EXE)
# The arena: the headless experiment runner and the terminal viewer. Both are
# consumers of the same MatchingEngine the benchmarks measure, so neither can
# affect a latency number.
ARENA_BINS := $(REL)/sweep$(EXE) $(REL)/arena$(EXE) $(REL)/suite$(EXE) $(REL)/controls$(EXE)

.PHONY: all test bench latency repl arena sweep controls reproduce clean

all: test test-cli test-lab test-app $(BENCH_BINS) $(REPL_BIN) $(ARENA_BINS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# The sweep runner's command line, checked the only way a command line can be:
# by running it. A bad field, flag or value must fail loudly (exit 2) and a
# configuration that could never terminate must be refused, not hang. These were
# all verified by hand once; this target is so they stay verified.
.PHONY: test-cli
test-cli: $(REL)/sweep$(EXE) $(REL)/suite$(EXE) $(REL)/controls$(EXE)
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
	files="suite.cfg sessions.csv participants.csv comparison.csv evaluation.csv attribution.csv report.txt outcome.cfg"; \
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
	exits 2 $$S --candidates=maker --suite=bogus || fail "an unknown suite should exit 2"; \
	exits 2 $$S --candidates=maker --suite=null || fail "the null family is not shipped (its market fails the gate)"; \
	[ "$$(ls $$D/keep)" = mine.txt ] && [ "$$(cat $$D/keep/mine.txt)" = mine ] \
	    || fail "a refused directory must be left untouched"; \
	$$S --block=smoke --candidates=maker,momentum --out=$$D/suite-a > /dev/null 2>&1 || fail "a smoke run should succeed"; \
	for f in $$files; do [ -f $$D/suite-a/$$f ] || fail "a run should write $$f"; done; \
	head -n1 $$D/suite-a/sessions.csv | grep -q '^suite,version,block,seed,arm,origin,' || fail "sessions.csv header"; \
	head -n1 $$D/suite-a/participants.csv | grep -q '^suite,version,block,seed,arm,index,' || fail "participants.csv header"; \
	grep -qx 'status=complete' $$D/suite-a/outcome.cfg || fail "outcome.cfg should say status=complete"; \
	head -n1 $$D/suite-a/evaluation.csv | grep -q '^kind,baseline,arm,metric,' || fail "evaluation.csv header"; \
	grep -q '^paired,maker,momentum,spread_earned,' $$D/suite-a/evaluation.csv || fail "evaluation.csv should pair the candidates"; \
	grep -q 'sum to minus the seat.s value exactly' $$D/suite-a/report.txt || fail "the attribution identity should hold"; \
	$$S --block=smoke --suite=all --candidates=maker --out=$$D/fam > /dev/null 2>&1 || fail "--suite=all should run every family"; \
	for f in standard wide busy; do grep -qx 'status=complete' $$D/fam/$$f/outcome.cfg || fail "family $$f should complete"; done; \
	grep -q 'not pooled' $$D/fam/families.txt || fail "families.txt should hold the cross-family table"; \
	[ "$$(sed -n 's/^fingerprint=//p' $$D/fam/standard/outcome.cfg)" = d461c30ab2446acf ] \
	    || fail "the standard family should be the pinned standard suite"; \
	$$S --block=smoke --candidates=maker,momentum --out=$$D/suite-b > /dev/null 2>&1 || fail "a second smoke run should succeed"; \
	same $$D/suite-a $$D/suite-b || fail "two runs should write byte-identical files"; \
	$$S --block=smoke --candidates=maker,momentum --out=$$D/suite-a > /dev/null 2>&1 || fail "a previous run should be replaced"; \
	same $$D/suite-a $$D/suite-b || fail "a replaced run should still match"; \
	grep -q '^build.commit=' $$D/suite-a/suite.cfg || fail "suite.cfg should record the build"; \
	$$S --reproduce=$$D/suite-a --out=$$D/re-a 2>/dev/null | grep -q '^  REPRODUCED' || fail "a run should reproduce"; \
	exits 2 $$S --reproduce=$$D/suite-a --candidates=maker || fail "--reproduce takes nothing but --out"; \
	exits 2 $$S --reproduce=$$D/keep || fail "--reproduce of a directory with no run should exit 2"; \
	cp -r $$D/suite-a $$D/cond; sed -i.bak 's/^maker.quote_qty=50$$/maker.quote_qty=40/' $$D/cond/suite.cfg; \
	exits 2 $$S --reproduce=$$D/cond --out=$$D/re-cond || fail "changed conditions should be refused before running"; \
	[ ! -e $$D/re-cond ] || fail "a refused reproduction should write nothing"; \
	cp -r $$D/suite-a $$D/raw; sed -i.bak '2s/,maker,/,maker ,/' $$D/raw/participants.csv; \
	exits 1 $$S --reproduce=$$D/raw --out=$$D/re-raw || fail "a run whose raw files changed should not reproduce"; \
	$$S --reproduce=$$D/fam --out=$$D/re-fam > /dev/null 2>&1 || fail "a --suite=all run should reproduce family by family"; \
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
	echo "34/34 suite command-line checks passed"; \
	echo "== the evaluator's controls"; \
	C=./$(REL)/controls$(EXE); \
	$$C --help > /dev/null || fail "controls --help should succeed"; \
	exits 2 $$C --bogus || fail "controls: an unknown flag should exit 2"; \
	exits 2 $$C --block=bogus || fail "controls: an unknown block should exit 2"; \
	out=$$($$C 2>/dev/null) || fail "every control should hold on the standard block"; \
	echo "$$out" | grep -qx '8/8 controls held' || fail "controls should report 8/8"; \
	echo "$$out" | grep -A1 'noise floor' | sed -n 's/^ *measured: /  noise floor: /p'; \
	echo "4/4 controls checks passed"

bench: $(BENCH_BINS)
	@set -e; for b in $(BENCH_BINS); do echo "== $$b"; ./$$b; done

latency: $(REL)/bench_matching_latency$(EXE)
	./$<

repl: $(REPL_BIN)

arena: $(ARENA_BINS)

sweep: $(REL)/sweep$(EXE)
	./$<

# The evaluator's own controls on the standard block (docs/CONTROLS.md); fails if one does not hold.
controls: $(REL)/controls$(EXE)
	./$<

# The app passes ANNOUNCE=1, so a command that has to rebuild says so rather than
# pausing silently (a pull or a commit changes the build info, which rebuilds suite_lab).
announce = $(if $(ANNOUNCE),@echo "  $(1) $(2)")

$(DOBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(DEBUG_FLAGS) $(DEPFLAGS) -c $< -o $@

$(ROBJ)/%.o: %.cpp
	$(call announce,compiling,$<)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(RELEASE_FLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BINS): $(DBG)/%$(EXE): $(DOBJ)/tests/%.o $(LIB_DBG)
	$(CXX) $(DEBUG_FLAGS) $^ -o $@ $(LDLIBS)

$(BENCH_BINS): $(REL)/%$(EXE): $(ROBJ)/bench/%.o $(LIB_REL)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(REPL_BIN): $(ROBJ)/tools/repl.o $(LIB_REL)
	$(call announce,linking,$@)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(ARENA_BINS): $(REL)/%$(EXE): $(ROBJ)/arena/%.o $(LIB_REL)
	$(call announce,linking,$@)
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
	./$(REL)/suite_lab$(EXE) --candidates=$(CANDIDATES) $(if $(BLOCK),--block=$(BLOCK)) $(if $(SUITE),--suite=$(SUITE)) $(if $(OUT),--out=$(OUT))

# Re-run a run directory from its own suite.cfg and compare every file (docs/REPRODUCE.md).
reproduce: $(REL)/suite_lab$(EXE)
	@[ -n '$(RUN)' ] || { echo "make reproduce: say which run: make reproduce RUN=runs/<dir>" >&2; exit 2; }
	./$(REL)/suite_lab$(EXE) --reproduce=$(RUN) $(if $(OUT),--out=$(OUT))

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
	  echo; echo "// Each installed header's cksum CRC: which version of a strategy ran."; \
	  echo "[[nodiscard]] inline const char* user_strategy_digest(std::string_view name) {"; \
	  for h in $$(ls $(USER_STRATEGY_DIR)/*.hpp 2>/dev/null | sort); do \
	    echo "    if (name == \"$$(basename $$h .hpp)\") return \"$$(cksum < $$h | cut -d' ' -f1)\";"; \
	  done; \
	  echo "    (void)name;"; echo "    return \"\";"; echo "}"; \
	  echo; echo "} // namespace px::arena"; } > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

FORCE:

# Which build ran a suite, recorded in every suite.cfg (docs/REPRODUCE.md).
# Regenerated on every build, rewritten only when it changes.
BUILD_INFO := $(GEN)/px_build_info.hpp
$(BUILD_INFO): FORCE
	@mkdir -p $(GEN)
	@set -e; t=$@.tmp; \
	commit=$$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown); \
	dirty=$$(git status --porcelain --untracked-files=no 2>/dev/null | head -n 1 | wc -l | tr -d ' '); \
	{ echo "// Generated by make. Do not edit; never committed."; echo "#pragma once"; \
	  echo "inline constexpr const char* kBuildCommit = \"$$commit\";"; \
	  echo "inline constexpr bool kBuildDirty = $$( [ "$$dirty" = 0 ] && echo false || echo true );"; \
	  echo "inline constexpr const char* kBuildFlags = \"$(strip $(RELEASE_FLAGS))\";"; } > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

$(ROBJ)/arena/suite.o $(LOBJ)/arena/suite.o: $(BUILD_INFO)
$(ROBJ)/arena/suite.o: INCLUDES += -I$(GEN)

# ---- the terminal app (docs/APP.md) ---------------------------------------
#
# One prompt with slash commands, in front of the programs above. It runs them
# as child processes and links none of the market, so it cannot move a hash,
# a fingerprint or a published number.
APP_BIN := $(REL)/exchange-lab$(EXE)

.PHONY: app test-app

app: $(APP_BIN) $(LAB_BINS) $(REL)/controls$(EXE) $(REPL_BIN)

$(APP_BIN): $(ROBJ)/app/main.o
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(ROBJ)/app/main.o: $(BUILD_INFO)
$(ROBJ)/app/main.o: INCLUDES += -I$(GEN)

# Commands typed into the real binary. It writes runs/maker-smoke, as
# /compare maker smoke would for anyone; everything else goes under $(BUILD)/app-test.
# MSYS2_ARG_CONV_EXCL stops an MSYS2 shell rewriting `-c /help` into a Windows path,
# and tr drops the carriage returns a Windows program writes before a line ends.
test-app: $(APP_BIN) $(LAB_BINS) $(REL)/controls$(EXE) $(REPL_BIN)
	@set -e; export MSYS2_ARG_CONV_EXCL='*'; \
	n=0; ok() { n=$$((n+1)); }; fail() { echo "FAIL: $$1"; exit 1; }; \
	A=./$(APP_BIN); D=$(BUILD)/app-test; rm -rf $$D; mkdir -p $$D; \
	echo "== exchange-lab app"; \
	out=$$(echo /help | $$A 2>&1); \
	for c in $$(sed -n 's/^    {"\(\/[a-z]*\)".*/\1/p' app/commands.hpp); do \
	    echo "$$out" | grep -q "^  $$c " || fail "/help should list $$c"; done; ok; \
	rc=0; echo /nosuch | $$A > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 2 ] && grep -q 'no command /nosuch' $$D/out || fail "an unknown command should be refused"; ok; \
	rc=0; $$A -c 'hello there' > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 2 ] && grep -q 'commands start with /' $$D/out || fail "plain text should point at /help"; ok; \
	out=$$($$A -c /strategies); \
	for s in $(BUILTIN_STRATEGIES); do echo "$$out" | grep -q "^  $$s " || fail "/strategies should list $$s"; done; ok; \
	rc=0; $$A -c '/add Makefile' > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 2 ] && grep -q 'not a .hpp header' $$D/out || fail "/add of a non-header should be refused"; ok; \
	$$A -c '/compare maker smoke' > $$D/compare 2>/dev/null || fail "/compare maker smoke should complete"; \
	grep -q '^status=complete' runs/maker-smoke/outcome.cfg && grep -q 'Saved in runs/maker-smoke' $$D/compare \
	    || fail "/compare should write a complete run and say where"; ok; \
	$$A -c /runs | tr -d '\r' | grep -Eq '^maker-smoke +complete +standard +smoke +maker$$' || fail "/runs should list it"; ok; \
	$$A -c '/report maker-smoke' | grep -q 'arena suite' || fail "/report should show it"; ok; \
	cp -r runs/maker-smoke $$D/run; \
	$$A -c "/reproduce $$D/run" 2>/dev/null | grep -q '^  REPRODUCED' || fail "/reproduce should reproduce it"; ok; \
	cp -r $$D/run $$D/bad; echo >> $$D/bad/report.txt; \
	rc=0; $$A -c "/reproduce $$D/bad" > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'NOT REPRODUCED' $$D/out || fail "-c should return the child's status (got $$rc, want 1)"; ok; \
	$$A -c '/controls smoke' 2>/dev/null | grep -q '^8/8 controls held' || fail "/controls should run the controls"; ok; \
	$$A -c '/watch seed=42 still=1000000' > /dev/null || fail "/watch should draw a still"; ok; \
	$$A -c '/sweep --help' > /dev/null || fail "/sweep should pass its flags through"; ok; \
	$$A -c /book < tools/repl_demo.txt > /dev/null || fail "/book should run the REPL"; ok; \
	[ "$$($$A -c /learn | grep -c '^  [0-9]\.')" -eq 8 ] || fail "/learn should list eight exercises"; ok; \
	$$A -c '/learn 1' | grep -q '^1\. ' || fail "/learn 1 should show the first"; ok; \
	rc=0; (cd $$D && $(CURDIR)/$(APP_BIN) -c /help > /dev/null 2>&1) || rc=$$?; \
	[ $$rc -eq 2 ] || fail "outside the repository root it should refuse"; ok; \
	rm -rf $$D; \
	echo "$$n/$$n app checks passed"

$(LOBJ)/%.o: %.cpp $(MANIFEST)
	$(call announce,compiling,$<)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(LAB_FLAGS) $(RELEASE_FLAGS) $(DEPFLAGS) -c $< -o $@

$(LAB_BINS): $(REL)/%_lab$(EXE): $(LOBJ)/arena/%.o $(LIB_REL)
	$(call announce,linking,$@)
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
	grep -q '^run.candidate.imbalance.digest=[0-9]' $$D/suite/suite.cfg || fail "an installed strategy should be recorded by checksum"; \
	$(MAKE) --no-print-directory reproduce RUN=$$D/suite OUT=$$D/re > /dev/null 2>&1 || fail "make reproduce should reproduce it"; \
	exits_() { want=$$1; shift; rc=0; "$$@" > /dev/null 2>&1 || rc=$$?; [ $$rc -eq $$want ]; }; \
	exits_ 2 ./$(REL)/suite$(EXE) --reproduce=$$D/suite --out=$$D/re2 || fail "the evidence suite cannot reproduce an installed strategy"; \
	sed -i.bak 's/^\(run.candidate.imbalance.digest=\).*/\1123/' $$D/suite/suite.cfg; \
	exits_ 2 ./$(REL)/suite_lab$(EXE) --reproduce=$$D/suite --out=$$D/re3 || fail "a changed strategy should be refused"; \
	echo "  fingerprint $$(fp $$D/suite), identical in suite and suite_lab"; \
	rm -rf $$D; \
	echo "8/8 suite checks passed"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(DOBJ)/*/*.d $(ROBJ)/*/*.d $(DOBJ)/*/*/*.d $(ROBJ)/*/*/*.d $(LOBJ)/*/*.d)
