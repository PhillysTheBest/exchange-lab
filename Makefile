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
#   make data FILE=path [OPTS='name=x ...']   bring a market data file in as a tape (docs/DATA.md)
#   make test-data the ingest program's command line, run against the samples in examples/data/
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
           test_arena test_app test_data
BENCHES := bench_engine bench_ring bench_matching_latency

TEST_BINS  := $(TESTS:%=$(DBG)/%$(EXE))
BENCH_BINS := $(BENCHES:%=$(REL)/%$(EXE))
REPL_BIN   := $(REL)/repl$(EXE)
# The arena: the headless experiment runner and the terminal viewer. Both are
# consumers of the same MatchingEngine the benchmarks measure, so neither can
# affect a latency number.
ARENA_BINS := $(REL)/sweep$(EXE) $(REL)/arena$(EXE) $(REL)/suite$(EXE) $(REL)/controls$(EXE)
# Bringing market data in (docs/DATA.md). ingest links none of the market;
# calibrate runs the reference market, and pins nothing; replay is a lab
# program (it runs installed strategies), defined with them below.
INGEST_BIN := $(REL)/ingest$(EXE)
CALIBRATE_BIN := $(REL)/calibrate$(EXE)
REPLAY_BIN := $(REL)/replay$(EXE)

.PHONY: all test bench latency repl arena sweep controls reproduce clean

all: test test-cli test-lab test-app test-data $(BENCH_BINS) $(REPL_BIN) $(ARENA_BINS)

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

lab: $(LAB_BINS) $(REPLAY_BIN)

# The standard experiment (docs/RUNNER.md): every candidate in the same seat of
# the same market on the same named block of seeds, written to runs/.
#   make compare CANDIDATES=mine,other [BLOCK=smoke] [SUITE=wide|busy|all | MARKET=name] [OUT=dir]
# BLOCK rather than SEEDS, because bench/run_arena_evidence.sh reads a numeric
# SEEDS from the environment and an exported one would leak in here.
compare: $(REL)/suite_lab$(EXE)
	@[ -n '$(CANDIDATES)' ] || { echo "make compare: say which strategies: make compare CANDIDATES=a[,b,...]" >&2; exit 2; }
	./$(REL)/suite_lab$(EXE) --candidates=$(CANDIDATES) $(if $(BLOCK),--block=$(BLOCK)) $(if $(SUITE),--suite=$(SUITE)) $(if $(MARKET),--market=$(MARKET)) $(if $(OUT),--out=$(OUT))

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

app: $(APP_BIN) $(LAB_BINS) $(REL)/controls$(EXE) $(REPL_BIN) $(INGEST_BIN) $(CALIBRATE_BIN) $(REPLAY_BIN)

$(APP_BIN): $(ROBJ)/app/main.o
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(ROBJ)/app/main.o: $(BUILD_INFO)
$(ROBJ)/app/main.o: INCLUDES += -I$(GEN)

# Commands typed into the real binary. It writes runs/maker-smoke, as
# /compare maker smoke would for anyone; everything else goes under $(BUILD)/app-test.
# MSYS2_ARG_CONV_EXCL stops an MSYS2 shell rewriting `-c /help` into a Windows path,
# and tr drops the carriage returns a Windows program writes before a line ends.
test-app: $(APP_BIN) $(LAB_BINS) $(REL)/controls$(EXE) $(REPL_BIN) $(INGEST_BIN) $(CALIBRATE_BIN) $(REPLAY_BIN)
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
	$$A -c '/data add examples/data/trades.csv name=app-test-trades' | grep -q '^added app-test-trades ' \
	    || fail "/data add should import a file"; ok; \
	$$A -c /data | grep -Eq '^app-test-trades +trades +500 ' || fail "/data should list it"; ok; \
	$$A -c '/data show app-test-trades' | grep -q '^  print size ' || fail "/data show should show it"; ok; \
	rc=0; $$A -c '/calibrate app-test-trades' > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'trades-only tape is not calibrated' $$D/out || fail "/calibrate should refuse a trades-only tape"; ok; \
	rc=0; $$A -c '/replay app-test-trades' > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'needs an order-by-order (L3) tape' $$D/out || fail "/replay should refuse a trades-only tape"; ok; \
	$$A -c '/data add examples/data/DEMO_2012-06-21_34200000_57600000_message_3.csv name=app-test-l3' > /dev/null; \
	$$A -c '/replay app-test-l3' | grep -q '^The control holds' || fail "/replay should run the control"; ok; \
	$$A -c '/replay app-test-l3 momentum for=3s' | grep -q '^History cannot react' || fail "/replay should run a strategy, labelled"; ok; \
	$$A -c '/data remove app-test-l3' > /dev/null; \
	$$A -c '/data remove app-test-trades' > /dev/null && [ ! -e data/app-test-trades.tape ] \
	    || fail "/data remove should remove it"; ok; \
	[ "$$($$A -c /learn | grep -c '^  [0-9]\.')" -eq 9 ] || fail "/learn should list nine exercises"; ok; \
	$$A -c '/learn 1' | grep -q '^1\. ' || fail "/learn 1 should show the first"; ok; \
	rc=0; (cd $$D && $(CURDIR)/$(APP_BIN) -c /help > /dev/null 2>&1) || rc=$$?; \
	[ $$rc -eq 2 ] || fail "outside the repository root it should refuse"; ok; \
	rm -rf $$D; \
	echo "$$n/$$n app checks passed"

# ---- market data of your own (docs/DATA.md) ----------------------------------
#
#   make data                                 list the tapes in data/
#   make data FILE=path [OPTS='name=x ...']   import a file into data/NAME.tape
#   make data SHOW=name                       what a tape holds, and what calibration matches
#   make calibrate TAPE=name                  data/NAME.tape -> markets/NAME.cfg, for MARKET=name
#
# ingest reads a file already on disk and never fetches anything. It links none
# of the market, so an importer cannot reach a session, a hash or a published
# number. data/ and markets/ are git-ignored: a tape made from data you paid for
# stays yours. Only the lab programs read a market (--market=NAME).
.PHONY: data calibrate replay test-data

$(INGEST_BIN): $(ROBJ)/marketdata/ingest.o
	$(call announce,linking,$@)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(CALIBRATE_BIN): $(ROBJ)/marketdata/calibrate.o $(LIB_REL)
	$(call announce,linking,$@)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

data: $(INGEST_BIN)
	@set -e; \
	if [ -n '$(FILE)' ]; then ./$(INGEST_BIN) add '$(FILE)' $(OPTS); \
	elif [ -n '$(SHOW)' ]; then ./$(INGEST_BIN) show '$(SHOW)'; \
	else ./$(INGEST_BIN) list; fi

calibrate: $(CALIBRATE_BIN)
	@[ -n '$(TAPE)' ] || { echo "make calibrate: say which tape: make calibrate TAPE=name (make data lists them)" >&2; exit 2; }
	./$(CALIBRATE_BIN) '$(TAPE)'

# An order-by-order tape played back through the engine; history cannot react.
#   make replay TAPE=name [STRATEGY=name] [OPTS='from=1h for=10m reaction=50us']
replay: $(REPLAY_BIN)
	@[ -n '$(TAPE)' ] || { echo "make replay: say which tape: make replay TAPE=name [STRATEGY=name]" >&2; exit 2; }
	./$(REPLAY_BIN) '$(TAPE)' $(if $(STRATEGY),strategy=$(STRATEGY)) $(OPTS)

# The command line, run against the samples, in a directory of its own; then
# the known-answer control, which must hold, with its planted defects, which must
# break it; then a market calibrated from the control's own tape, used by the lab
# programs, refused by the evidence ones, and reproduced without its data.
test-data: $(INGEST_BIN) $(CALIBRATE_BIN) $(REPLAY_BIN) $(LAB_BINS) $(REL)/suite$(EXE) $(REL)/sweep$(EXE) $(REL)/arena$(EXE)
	@set -e; \
	n=0; ok() { n=$$((n+1)); }; fail() { echo "FAIL: $$1"; exit 1; }; \
	exits() { want=$$1; shift; rc=0; "$$@" > /dev/null 2>&1 || rc=$$?; [ $$rc -eq $$want ]; }; \
	I=./$(INGEST_BIN); D=$(BUILD)/data-test; rm -rf $$D; mkdir -p $$D; \
	L=examples/data/DEMO_2012-06-21_34200000_57600000_message_3.csv; \
	echo "== ingest"; \
	$$I --help > /dev/null || fail "--help should succeed"; ok; \
	exits 2 $$I || fail "no command should exit 2"; ok; \
	exits 2 $$I fetch x || fail "an unknown command should exit 2"; ok; \
	exits 2 $$I add $$L dir=$$D colour=red || fail "an unknown setting should exit 2"; ok; \
	exits 2 $$I add $$L dir=$$D format=parquet || fail "an unknown format should exit 2"; ok; \
	printf 'DBN\001' > $$D/x.dbn; \
	rc=0; $$I add $$D/x.dbn dir=$$D > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'metadata is cut short' $$D/out || fail "a DBN file cut short should be refused"; ok; \
	exits 2 $$I add $$L dir=$$D name=Bad/Name || fail "a bad name should exit 2"; ok; \
	$$I list dir=$$D | grep -q 'no tapes yet' || fail "an empty list should say so"; ok; \
	$$I add $$L dir=$$D > $$D/out || fail "the LOBSTER sample should import"; \
	grep -q '^added demo-2012-06-21 ' $$D/out && grep -q 'with the orderbook file beside it' $$D/out \
	    || fail "it should be named from the file and read the orderbook file"; ok; \
	grep -q '^tape.version=1$$' $$D/demo-2012-06-21.tape || fail "it should write a tape"; ok; \
	[ "$$(sed -n 's/^source.cksum=//p' $$D/demo-2012-06-21.tape)" = "$$(cksum < $$L | cut -d' ' -f1-2)" ] \
	    || fail "the tape should record the source's cksum, as cksum prints it"; ok; \
	cp $$D/demo-2012-06-21.tape $$D/first.tape; \
	$$I add $$L dir=$$D | grep -q '^replaced demo-2012-06-21 ' || fail "the same file again should replace it"; ok; \
	cmp -s $$D/first.tape $$D/demo-2012-06-21.tape || fail "the same file should give the same tape, byte for byte"; ok; \
	rm $$D/first.tape; \
	$$I add examples/data/trades.csv dir=$$D name=demo-2012-06-21 > $$D/out 2>&1 && fail "a name taken by another file should be refused"; \
	grep -q 'already exists, made from another file' $$D/out || fail "and say why"; ok; \
	$$I add examples/data/trades.csv dir=$$D > /dev/null || fail "the trades sample should import"; ok; \
	$$I add examples/data/databento_mbo.csv dir=$$D > $$D/out || fail "the Databento sample should import"; \
	grep -q '^databento_mbo: DEMO, 2024-01-02, order by order (L3)$$' $$D/out && grep -q '2 trades read through their fills' $$D/out \
	    || fail "it should be read order by order, each trade through its fills"; ok; \
	$$I add examples/data/demo_incremental_book_L2_2024-01-02_DEMO.csv dir=$$D > $$D/out || fail "the Tardis sample should import"; \
	grep -q '^added demo-demo-2024-01-02 .*(with its trades file beside it)$$' $$D/out && grep -q '4 trades, 15 level updates, 1 reset$$' $$D/out \
	    || fail "it should be read with its trades file, the second snapshot a reset"; ok; \
	$$I add examples/data/demo_trades_2024-01-02_DEMO.csv dir=$$D | grep -q '^added demo-demo-2024-01-02-trades ' \
	    || fail "its trades file alone should be a tape of its own"; ok; \
	$$I add examples/data/BTCUSDT-bookTicker-2024-01-02.csv dir=$$D > $$D/out || fail "the Binance sample should import"; \
	grep -q '^added btcusdt-bookticker-2024-01-02 .*(with its trades file beside it)$$' $$D/out && grep -q '3 trades, 8 level updates$$' $$D/out \
	    || fail "a bookTicker file should be read with its trades file"; ok; \
	$$I add examples/data/ETHUSDT-trades-2025-01-02.csv dir=$$D > $$D/out || fail "a spot trades file with no header should import"; \
	grep -q 'read as binance' $$D/out && grep -q 'binance trades (spot, no header)' $$D/out || fail "and be read as Binance's"; ok; \
	$$I add examples/data/coinbase_feed.jsonl dir=$$D > $$D/out || fail "the Coinbase sample should import"; \
	grep -q '^coinbase_feed: BTC-USD, 2024-01-02, price levels (L2)$$' $$D/out && grep -q '2 trades, 8 level updates$$' $$D/out \
	    || fail "a captured feed should be read as JSON, a message a line"; ok; \
	$$I add examples/data/XBTUSD.csv dir=$$D > $$D/out || fail "the Kraken sample should import"; \
	grep -q '^xbtusd: XBTUSD, 2024-01-02, trades only$$' $$D/out && grep -q 'read as kraken' $$D/out \
	    || fail "Kraken's trade history should be found by its name and shape"; ok; \
	exits 1 $$I add examples/data/01022024.NASDAQ_ITCH50 dir=$$D || fail "an ITCH file without symbol= should be refused"; ok; \
	$$I add examples/data/01022024.NASDAQ_ITCH50 symbol=DEMO dir=$$D > $$D/out || fail "the ITCH sample should import"; \
	grep -q '^demo-2024-01-02: DEMO, 2024-01-02, order by order (L3)$$' $$D/out && grep -q '4 adds, 4 cancels, 3 trades$$' $$D/out \
	    && grep -q "cksum $$(cksum < examples/data/01022024.NASDAQ_ITCH50)" $$D/out \
	    || fail "it should be streamed for one stock, its cksum as cksum prints it"; ok; \
	exits 1 $$I add examples/data/quotes_and_trades.csv dir=$$D || fail "a file with no header needs columns="; ok; \
	$$I add examples/data/quotes_and_trades.csv dir=$$D 'columns=time,price,qty,side,bid,bid_qty,ask,ask_qty' > $$D/out \
	    || fail "with columns= it should import"; \
	grep -q 'price levels (L2)' $$D/out || fail "quotes should make an L2 tape"; ok; \
	printf 'time,price,qty\n1,10,1\n2,ten,1\n' > $$D/bad.csv; \
	rc=0; $$I add $$D/bad.csv dir=$$D > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q "line 3: price 'ten' is not a number" $$D/out || fail "a malformed file should be refused naming the line"; ok; \
	[ ! -e $$D/bad.tape ] || fail "a refused file should write nothing"; ok; \
	printf '\037\213rest' > $$D/x.csv.gz; \
	rc=0; $$I add $$D/x.csv.gz dir=$$D > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'gzip -dk' $$D/out || fail "a compressed file should be refused with the command that opens it"; ok; \
	exits 1 $$I add $$D/nosuch.csv dir=$$D || fail "a missing file should exit 1"; ok; \
	$$I list dir=$$D > $$D/out; \
	grep -Eq '^demo-2012-06-21 +L3 +' $$D/out && grep -Eq '^trades +trades +500 ' $$D/out \
	    && grep -Eq '^quotes_and_trades +L2 ' $$D/out || fail "list should show every tape"; ok; \
	$$I show demo-2012-06-21 dir=$$D > $$D/out || fail "show should succeed"; \
	grep -q '^  spread  *[0-9]* ticks* ' $$D/out && grep -q '^  per move ' $$D/out || fail "show should print the statistics"; ok; \
	exits 1 $$I show nosuch dir=$$D || fail "show of a missing tape should exit 1"; ok; \
	sed -i.bak '$$d' $$D/trades.tape; \
	exits 1 $$I show trades dir=$$D || fail "a tape cut short should be refused"; ok; \
	$$I remove trades dir=$$D > /dev/null && [ ! -e $$D/trades.tape ] || fail "remove should delete the tape"; ok; \
	exits 1 $$I remove trades dir=$$D || fail "removing it twice should exit 1"; ok; \
	[ -f examples/data/trades.csv ] || fail "remove must never touch the source"; ok; \
	rm -rf $$D; \
	echo "$$n/$$n ingest checks passed"; \
	echo "== replay"; \
	P=./$(REPLAY_BIN); D=$(BUILD)/replay-test; rm -rf $$D; mkdir -p $$D; r=0; okr() { r=$$((r+1)); }; \
	$$I add $$L dir=$$D > /dev/null; $$I add examples/data/coinbase_feed.jsonl dir=$$D > /dev/null; \
	$$P --help > /dev/null || fail "replay --help should succeed"; okr; \
	exits 2 $$P || fail "replay with no tape should exit 2"; okr; \
	exits 2 $$P demo-2012-06-21 data=$$D colour=red || fail "an unknown setting should exit 2"; okr; \
	exits 2 $$P demo-2012-06-21 data=$$D reaction=soon || fail "a span that is not one should exit 2"; okr; \
	exits 1 $$P nosuch data=$$D || fail "a missing tape should exit 1"; okr; \
	rc=0; $$P coinbase_feed data=$$D > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'needs an order-by-order (L3) tape' $$D/out || fail "a tape of price levels should be refused"; okr; \
	$$P demo-2012-06-21 data=$$D > $$D/out || fail "the control should hold on the LOBSTER tape"; \
	grep -q '^The control holds' $$D/out && grep -q 'after every one of 771 events' $$D/out || fail "and say so"; okr; \
	exits 2 $$P demo-2012-06-21 strategy=nosuch data=$$D || fail "an unknown strategy should exit 2"; okr; \
	exits 1 $$P demo-2012-06-21 strategy=informed data=$$D || fail "informed has no fundamental to trade on"; okr; \
	$$P demo-2012-06-21 strategy=maker data=$$D > $$D/a || fail "a strategy should run in it"; \
	$$P demo-2012-06-21 strategy=maker data=$$D > $$D/b; \
	cmp -s $$D/a $$D/b && grep -q '^History cannot react' $$D/a && grep -q '^  value ' $$D/a \
	    || fail "it should be labelled, and give the same report twice"; okr; \
	h=$$(sed -n 's/^  hash *\([0-9a-f]*\) .*/\1/p' $$D/out); \
	$$P demo-2012-06-21 strategy=placebo data=$$D | grep -q "^  hash *$$h " || fail "an inert strategy should leave the tape's stream as it was"; okr; \
	rm -rf $$D; \
	echo "$$r/$$r replay checks passed"; \
	echo "== the known-answer control (a few hundred simulated markets; under a minute)"; \
	C=./$(CALIBRATE_BIN); m=0; okc() { m=$$((m+1)); }; \
	$$C --help > /dev/null || fail "calibrate --help should succeed"; okc; \
	exits 2 $$C --bogus || fail "calibrate: an unknown flag should exit 2"; okc; \
	out=$$($$C --control) || { echo "$$out"; fail "the control should hold and both defects break it"; }; okc; \
	echo "$$out" | grep -Eq '^  L3 +as written +3 / (30|40|50) / (15000|20000|30000) +yes$$' \
	    || { echo "$$out"; fail "order by order, the truth should come back within one grid step"; }; okc; \
	[ "$$(echo "$$out" | grep -c 'no, as it must not$$')" -eq 2 ] || fail "both planted defects should break it"; okc; \
	echo "$$out" | grep -Eq '^  trades .* no$$' || fail "trades alone should not be enough"; okc; \
	echo "$$out" | grep -E '^  (L3|trades) ' | sed 's/^/  /'; \
	echo "$$m/$$m control checks passed"; \
	echo "== a calibrated market (the control again, then 64 sessions of the health gate)"; \
	D=$(BUILD)/market-test; rm -rf $$D; mkdir -p $$D; k=0; okm() { k=$$((k+1)); }; \
	R=./$(REL); M=$$D/markets/known.cfg; \
	$$I add $$L dir=$$D/data > /dev/null; $$I add examples/data/trades.csv dir=$$D/data > /dev/null; \
	rc=0; $$C demo-2012-06-21 data=$$D/data markets=$$D/markets > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'too few prints' $$D/out || fail "a short tape should be refused (exit $$rc)"; okm; \
	rc=0; $$C trades data=$$D/data markets=$$D/markets > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] && grep -q 'trades-only tape is not calibrated' $$D/out || fail "a trades-only tape should be refused"; okm; \
	exits 2 $$C all data=$$D/data || fail "a market cannot take a suite's name"; okm; \
	exits 1 $$C nosuch data=$$D/data || fail "a missing tape should exit 1"; okm; \
	$$C --known=known data=$$D/data > /dev/null || fail "--known should write the control's tape"; okm; \
	$$C known data=$$D/data markets=$$D/markets > $$D/out || { cat $$D/out; fail "the known tape should calibrate"; }; okm; \
	grep -q '^status=complete$$' $$M && grep -q '^health.passed=64$$' $$M && grep -q '^control.held=1$$' $$M \
	    || fail "the market should be complete, admissible and say why"; okm; \
	grep -q '^set.maker.half_spread_ticks=3$$' $$M && grep -Eq '^set.noise.qty=(30|40|50)$$' $$M \
	    && grep -Eq '^set.noise.timer_ns=(15000|20000|30000)$$' $$M || fail "it should recover the known market"; okm; \
	sed -n 's/^set\./  /p' $$M; \
	$$R/suite_lab --market=$$M --candidates=maker --block=smoke --out=$$D/run > /dev/null 2>&1 \
	    || fail "suite_lab should run on the market"; okm; \
	grep -q '^calibration.tape_cksum=' $$D/run/suite.cfg && grep -q '^suite.name=known$$' $$D/run/suite.cfg \
	    || fail "the run should record the market among its conditions"; okm; \
	$$R/suite --candidates=maker --block=smoke --out=$$D/standard > /dev/null 2>&1; \
	fp() { sed -n 's/^fingerprint=//p' $$1/outcome.cfg; }; \
	[ -n "$$(fp $$D/run)" ] && [ "$$(fp $$D/run)" != "$$(fp $$D/standard)" ] || fail "a calibrated market is a suite of its own"; okm; \
	$$R/suite_lab --reproduce=$$D/run --out=$$D/re > /dev/null 2>&1 || fail "it should reproduce"; okm; \
	mv $$M $$D/aside.cfg; rm $$D/data/known.tape; \
	$$R/suite_lab --reproduce=$$D/run --out=$$D/re2 2>/dev/null | grep -q '^  REPRODUCED' \
	    || fail "it should reproduce from its own record, with no tape and no market file"; okm; \
	sed 's/^set.noise.qty=.*/set.noise.qty=99/' $$D/aside.cfg > $$M; \
	rc=0; $$R/suite_lab --reproduce=$$D/run --out=$$D/re3 > $$D/out 2>&1 || rc=$$?; \
	[ $$rc -eq 2 ] && grep -q 'calibrated again since the run' $$D/out || fail "a market calibrated again should be refused"; okm; \
	sed 's/^set.noise.qty=.*/set.informed.edge_ticks=9/' $$D/aside.cfg > $$D/bad.cfg; \
	exits 2 $$R/suite_lab --market=$$D/bad.cfg --candidates=maker --block=smoke --out=$$D/x \
	    || fail "a market setting what calibration never moves should be refused"; okm; \
	exits 2 $$R/suite_lab --market=$$D/aside.cfg --suite=wide --candidates=maker --out=$$D/x \
	    || fail "a market and a family together should be refused"; okm; \
	exits 2 $$R/suite --market=$$D/aside.cfg --candidates=maker --block=smoke --out=$$D/x \
	    || fail "the evidence suite must refuse a market"; okm; \
	exits 2 $$R/suite --reproduce=$$D/run --out=$$D/x || fail "the evidence suite must not reproduce one"; okm; \
	$$R/sweep_lab --market=$$D/aside.cfg --seeds=1 --requests=2000 --values=10000 --quiet > /dev/null \
	    || fail "sweep_lab should run on it"; okm; \
	exits 2 $$R/sweep --market=$$D/aside.cfg --seeds=1 --requests=2000 || fail "the evidence sweep must refuse it"; okm; \
	$$R/arena_lab --market=$$D/aside.cfg --still=1000000 | grep -q 'market known' || fail "arena_lab should show it"; okm; \
	exits 2 $$R/arena --market=$$D/aside.cfg --still=1000000 || fail "the evidence viewer must refuse it"; okm; \
	echo "  fingerprint $$(fp $$D/run) on the market, $$(fp $$D/standard) on standard"; \
	rm -rf $$D; \
	echo "$$k/$$k market checks passed"

$(LOBJ)/%.o: %.cpp $(MANIFEST)
	$(call announce,compiling,$<)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDES) $(LAB_FLAGS) $(RELEASE_FLAGS) $(DEPFLAGS) -c $< -o $@

$(LAB_BINS): $(REL)/%_lab$(EXE): $(LOBJ)/arena/%.o $(LIB_REL)
	$(call announce,linking,$@)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

$(REPLAY_BIN): $(LOBJ)/marketdata/replay.o $(LIB_REL)
	$(call announce,linking,$@)
	$(CXX) $(RELEASE_FLAGS) $^ -o $@ $(LDLIBS)

# The whole path, run the way a newcomer would: refusals first, then install the
# example, score it, and confirm the evidence binary cannot see it.
test-lab: $(REL)/sweep$(EXE) $(REL)/suite$(EXE) $(INGEST_BIN)
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
	./$(INGEST_BIN) add examples/data/DEMO_2012-06-21_34200000_57600000_message_3.csv dir=$$D/data > /dev/null; \
	./$(REPLAY_BIN) demo-2012-06-21 strategy=imbalance data=$$D/data | grep -q '^  value ' \
	    || fail "replay should run an installed strategy"; \
	echo "  fingerprint $$(fp $$D/suite), identical in suite and suite_lab"; \
	rm -rf $$D; \
	echo "9/9 suite checks passed"

# The outputs that must be identical on every platform, compared line by line with
# the record committed from Linux (docs/PORTABILITY_PLAN.md, stage 0). The first
# differing line names what diverged first; derived.* lines are the report's
# double arithmetic, apart from the market's integer state.
.PHONY: canonical
canonical: $(DBG)/test_determinism$(EXE) $(DBG)/test_arena$(EXE) $(REL)/sweep$(EXE) $(REL)/suite$(EXE) $(INGEST_BIN) $(CALIBRATE_BIN)
	@sh tools/canonical.sh $(BUILD) > $(BUILD)/canonical.txt
	@if diff tests/canonical.txt $(BUILD)/canonical.txt; then \
	    echo "canonical: $$(wc -l < tests/canonical.txt | tr -d ' ') outputs identical to tests/canonical.txt"; \
	else echo "canonical: differs from tests/canonical.txt (< recorded, > this build)"; exit 1; fi

clean:
	rm -rf $(BUILD)

-include $(wildcard $(DOBJ)/*/*.d $(ROBJ)/*/*.d $(DOBJ)/*/*/*.d $(ROBJ)/*/*/*.d $(LOBJ)/*/*.d)
