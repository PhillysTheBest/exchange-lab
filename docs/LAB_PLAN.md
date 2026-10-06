# Strategy Lab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Anyone with the repo and a compiler can add their own bot to the baseline market and get a reproducible, honestly-qualified score for it, without any possibility of disturbing the project's published evidence.

**Architecture:** A user bot is a header in `arena/bots/user/`. `make bot BOT=<path>` validates it, copies it in, and regenerates a manifest *into the build tree*. The manifest is reachable only from translation units compiled with `-DPX_ARENA_USER_BOTS`, and the only targets that set it are two new binaries, `arena_lab` and `sweep_lab`. `arena`, `sweep`, every test and `test-cli` therefore cannot see a user bot at all — evidence immunity is structural, not a rule anyone has to remember. Scoring runs shared seeds twice through `sweep_lab`, once with the user's bot in a seat and once with an inert placebo in the same seat, so the two arms have identical roster size and owner ids.

**Tech Stack:** C++20, hand-written Makefile (`mingw32-make` on Windows / MSYS2 UCRT64 g++ 13, `make` on Linux). No CMake, no package manager, no third-party libraries. Tests are `tests/test_arena.cpp` with `PX_CHECK` from `tests/test_harness.hpp`; command-line behaviour is checked by the `test-cli` target.

**Spec:** [`CLAUDE.md`](../CLAUDE.md), section "The strategy lab". Read it before Task 1, **and read the revisions below, which supersede it in four places.** Supporting context: [`docs/ARENA.md`](ARENA.md) for the arena's design, [`handoff.md`](../handoff.md) §6.

---

## What a council review changed, and why

The spec was war-roomed before implementation. Five advisors were unanimous that the drag-a-file-onto-the-viewer mechanism should not be built, and a peer-review round found four defects none of them had. The spec's own wording is now wrong in four places; this plan is the correct version, and Task 7 reconciles the documents.

1. **The drop, the rebuild and the respawn are cut.** They served nobody: `g++` and `mingw32-make` exist only inside the MSYS2 UCRT64 shell, so the visitor who could drop a file from Explorer has no compiler, and the visitor who has a compiler already has a shell. Measured on the author's machine, one arena translation unit plus its LTO link takes **27 seconds**, and a manifest change invalidates both `arena.o` and `sweep.o` — about 50-60s per drop, against the ~20s the docs claim. Installing is now `make bot BOT=<path>`. **The viewer is not modified by this plan at all.**
2. **A user bot is isolated from every build that asserts a hash.** The spec had the manifest included unconditionally from `registry.hpp`. Verified: `arena/session.hpp:42` includes `registry.hpp` and `tests/test_arena.cpp:30` includes `session.hpp`, so a stranger's header with a typo would break `mingw32-make test` *including the pinned-hash test*; and `Makefile:84` has `test-cli` build and run the release `sweep`, so guarding only the debug objects would not have been enough. Hence the define and the two separate binaries.
3. **The manifest is generated into the build tree, not committed.** The spec had it committed and rewritten in place, which makes a generated artifact a permanently dirty tracked file that conflicts on pull. Because only the lab targets can see it, it need not exist for a normal build, so it lives in `$(BUILD)/generated/` and is already covered by `.gitignore`'s `build*/`.
4. **The floating-point source scan is dropped, and the two scoring arms get a placebo.** A grep for `double` misses `n/2.0`, `std::sqrt` and `0.5` in a template; an 80%-accurate warning is the kind of overclaim this project's documentation refuses. Instead a scored run containing a user bot prints **no session hash** and says why. And adding a participant adds wake events, which `CLAUDE.md` already states moves every session hash — so a bare with/without pairing would conflate the bot's market impact with a re-interleaved session. Both arms now carry the same extra seat.

**One claim this plan does not make.** Scoring does *not* redeem the positive control that failed in `docs/ARENA.md`. That control failed because a maker stale enough to be reliably picked off also stops tracking the fundamental. A paired design measures a perturbation; it does not make a stale probe admissible. Do not let this into the documentation.

---

## Global Constraints

- **C++20, `-Wall -Wextra -Wpedantic`, zero warnings on both platforms.**
- **No new build-time dependency.** No python, no third-party library. The manifest is written by the installer, which is C++.
- **The engine is not touched.** `git diff` over `include/ src/ sim/` must stay empty for this whole plan.
- **`arena/arena.cpp`, `arena/ui.hpp` and `arena/term.hpp` are not touched.** The viewer is out of scope.
- **No engine API is added.**
- **Determinism is a contract.** `test_determinism` must keep printing `77a560d8ffb4fa43`. Every arena session hash asserted in `test_arena` must be unchanged. Task 2 proves a user bot cannot move either.
- **`arena`, `sweep` and all test binaries must be byte-for-byte unaffected** by anything in `arena/bots/user/`. This is the plan's central invariant.
- **The default line-up never contains a user bot or the placebo,** which is what keeps `results/arena-*` valid.
- **No floating point inside a session.** A user bot can violate this and the installer cannot detect it; the consequence is handled by refusing to print a hash, not by warning.
- **A session holds at most 64 participants** (`kMaxParticipants`) and refuses more.
- **`.gitattributes` pins LF.** Write bytes or pass `newline=''` when editing from Python on Windows.
- **Binaries and objects live in separate subtrees.** Lab objects need their own subtree too, because they are compiled with different flags than `$(ROBJ)/arena/*.o`.
- **Commits carry no Claude attribution.**
- **Never commit a state where `mingw32-make test` fails.**
- Bot contract, fixed by `arena/bot.hpp`: `const char* name; SimTime reaction_ns; SimTime timer_ns; void reset(std::uint64_t); ActionBatch on_event(const MarketView&);`

## Review Focus

1. **A user bot installed but not named.** Expected: every hash and number identical to a build with no user bot at all. This is the invariant; Task 2 owns it.
2. **A user bot that does not compile.** Expected: `make bot` fails and `make test`, `make test-cli`, `make arena` and `make sweep` all still pass, because they cannot see it. → Task 2.
3. **A path that is not an installable header** — missing, a directory, no `.hpp`, quoted, containing spaces, or a name colliding with a built-in. Expected: a named refusal, nothing written. → Task 4.
4. **A user bot that never sends an order.** Expected: the session ends on `stall_limit_ns` with `stats().stalled`, and the score reports *stalled* rather than a P&L read off a market that stopped. → Task 6.
5. **A user bot that pushes the roster past 64 participants.** Expected: `StartError::TooManyParticipants` as a message, not silent mistreatment. → Task 6.

---

## File Structure

**Created:**
- `arena/bots/user/.gitkeep` — the directory exists in a clean clone; `*.hpp` inside it is ignored.
- `arena/bots/user_install.hpp` — pure installer logic: validate a candidate, derive its name, render the manifest. No file I/O in the validating functions, so they unit-test.
- `arena/bots/placebo.hpp` — an inert participant: never acts. Exists so the two scoring arms have equal roster size and identical owner ids.
- `tools/install_bot.cpp` — the `make bot BOT=` driver. Thin: argument handling plus the file I/O that `user_install.hpp` deliberately does not do.
- `tests/user_bot_fixtures/{good_bot.hpp,collides.hpp,bad_include.hpp,no_factory.hpp,silent.hpp}`
- `$(BUILD)/generated/px_user_bots.hpp` — generated, never committed.

**Modified:**
- `arena/bots/registry.hpp` — behind `#if defined(PX_ARENA_USER_BOTS)`, include the manifest and delegate an unknown name to it. Without the define, identical to today. Register `placebo`.
- `arena/sweep.cpp` — `--score=NAME`, compiled into both `sweep` and `sweep_lab`.
- `Makefile` — `$(REL)/arena_lab`, `$(REL)/sweep_lab`, `bot`, `verify-user-bot`, lab object subtree, `test-cli` additions.
- `.gitignore`, `tests/test_arena.cpp`, and in Task 7: `docs/ARENA.md`, `README.md`, `CLAUDE.md`, `handoff.md`, `bench/summarize_arena.py`.

---

## Phase A — a user bot that cannot touch the evidence

### Task 1: The manifest, the delegation, and the placebo

**Files:**
- Create: `arena/bots/user/.gitkeep`, `arena/bots/placebo.hpp`
- Modify: `arena/bots/registry.hpp`, `.gitignore`
- Test: `tests/test_arena.cpp` (extend `test_roster_from_spec`)

**Interfaces:**
- Consumes: `add_named_bot(std::string_view, const RosterConfig&, Roster&) -> bool`, `kBotNames`, `roster_from_spec`, `make_bot`.
- Produces: `struct px::arena::bots::Placebo` with the five contract members, whose `on_event` always returns an empty `ActionBatch` and whose `timer_ns` is 0, named `"placebo"`, reachable as `add_named_bot("placebo", ...)` and listed in `kBotNames`. And, **only when `PX_ARENA_USER_BOTS` is defined**, `bool add_user_bot(std::string_view, const RosterConfig&, Roster&)` plus `kUserBotNames` from the generated manifest.

- [ ] **Step 1: Write the failing tests** in `test_roster_from_spec()`

```cpp
// The placebo is a real, nameable participant that never acts.
Roster p;
PX_CHECK(add_named_bot("placebo", rc, p));
PX_CHECK(std::string(p.bots[0]->name()) == "placebo");
PX_CHECK(p.bots[0]->timer_ns() == 0);
MarketView v{};
PX_CHECK(p.bots[0]->on_event(v).empty());
// kBotNames now advertises seven, and all are addable (the existing loop checks this).
PX_CHECK(listed == 7);
// Without PX_ARENA_USER_BOTS - which the test build never sets - an unknown
// name still fails exactly as it does today.
std::string why2;
Roster junk2;
PX_CHECK(!roster_from_spec("maker,nonsense", rc, junk2, why2));
#ifdef PX_ARENA_USER_BOTS
PX_CHECK(false); // the test build must never see user bots
#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `mingw32-make build/debug/test_arena.exe`
Expected: FAIL to compile — no `placebo`.

- [ ] **Step 3: Write `arena/bots/placebo.hpp` and register it**

A struct in the `fader.hpp` idiom, written against `arena/bot.hpp` alone, whose doc comment says what it is *for*: it occupies a seat so that a scored arm and its baseline have the same roster size and the same owner ids, because owner ids are `roster index + 1` and adding a participant also adds wake events, which moves the session hash. Add the branch to `add_named_bot` and the name to `kBotNames`.

- [ ] **Step 4: Add the conditional delegation to `arena/bots/registry.hpp`**

Wrap the manifest include and the delegation in `#if defined(PX_ARENA_USER_BOTS)`. In that branch only, change the final `else { return false; }` to delegate, and append `kUserBotNames` to `roster_from_spec`'s error text. Document in the header comment why the guard exists: the registry is reachable from `session.hpp`, which the tests include, so without it a stranger's header would sit in the binary that asserts the pinned hash.

- [ ] **Step 5: Add to `.gitignore`**

Ignore `arena/bots/user/*.hpp`, keeping `.gitkeep` tracked. `$(BUILD)/generated/` is already covered by `build*/`.

- [ ] **Step 6: Run the suite and confirm nothing moved**

Run: `mingw32-make test && mingw32-make test-cli`
Expected: PASS. `test_determinism` prints `77a560d8ffb4fa43`; every arena session hash is unchanged, because `placebo` is registered but in no default line-up.

- [ ] **Step 7: Commit**

```bash
git add arena/bots/placebo.hpp arena/bots/registry.hpp arena/bots/user/.gitkeep .gitignore tests/test_arena.cpp
git commit -m "Add an inert placebo participant and guard user-bot delegation

The registry is reachable from session.hpp, which the tests include, so
delegation is compiled only under PX_ARENA_USER_BOTS, which no test or
published-evidence target sets. Evidence: make test and test-cli green,
every session hash and test_determinism's 77a560d8ffb4fa43 unchanged."
```

---

### Task 2: The lab binaries, and the proof of immunity

**Files:**
- Modify: `Makefile`, `tests/test_arena.cpp`
- Test: `Makefile` `test-cli`

**Interfaces:**
- Produces: `$(REL)/arena_lab$(EXE)` and `$(REL)/sweep_lab$(EXE)`, compiled from the same sources as `arena` and `sweep` but with `-DPX_ARENA_USER_BOTS -I$(BUILD)/generated`, into their own object subtree `$(REL)/obj-lab/`. A `lab` phony builds both. Each lab target depends on `$(BUILD)/generated/px_user_bots.hpp`, and a rule writes an empty manifest if it does not exist, so `make lab` works in a clean clone with no user bots.

- [ ] **Step 1: Write the failing immunity checks** in `test-cli`

```
# A user bot is installed for the whole of this check.
cp tests/user_bot_fixtures/good_bot.hpp arena/bots/user/
mingw32-make bot BOT=arena/bots/user/good_bot.hpp
# 1. The published-evidence binary cannot see it, by construction.
! ./build/release/sweep --bots=maker,rival,good_bot --values=1000 --seeds=1 --requests=500 --quiet
# 2. The lab binary can.
./build/release/sweep_lab --bots=maker,rival,good_bot --values=1000 --seeds=1 --requests=500 --quiet
# 3. THE INVARIANT: with a user bot installed but not named, sweep_lab's
#    session hash equals plain sweep's.
test "$(./build/release/sweep_lab --values=1000 --seeds=1 --requests=2000 --quiet --print-hash)" \
   = "$(./build/release/sweep      --values=1000 --seeds=1 --requests=2000 --quiet --print-hash)"
# 4. A broken user bot cannot break the suite.
cp tests/user_bot_fixtures/bad_include.hpp arena/bots/user/
! mingw32-make bot BOT=arena/bots/user/bad_include.hpp
mingw32-make test
rm -f arena/bots/user/good_bot.hpp arena/bots/user/bad_include.hpp
```

Check 3 needs a `--print-hash` flag on `sweep` that prints the first session hash and nothing else; add it in this task, in both binaries. Raise the printed count from 11 to 17.

- [ ] **Step 2: Run to verify it fails**

Run: `mingw32-make test-cli`
Expected: FAIL — no rule to make target `bot`, no `sweep_lab`.

- [ ] **Step 3: Add the Makefile rules**

Each lab binary needs an explicit rule, not the `$(ARENA_BINS)` pattern: that pattern derives the object from the binary stem, and there is no `arena_lab.cpp`. Lab objects must not share `$(ROBJ)` with the ordinary ones, because the flags differ and make would otherwise reuse an object built without the define. Do not add the lab binaries to `all` or to `ARENA_BINS`; `make arena` and `make all` must keep building exactly what they build today.

- [ ] **Step 4: Run to verify it passes**

Run: `mingw32-make test-cli`
Expected: `17/17 command-line checks passed`, with check 3 — the invariant — passing.

- [ ] **Step 5: Verify the optimisation claim, and record the answer**

A session is all int64 with no floating point, so optimisation level should not be able to move a session hash. If that holds, the lab binaries can drop `-march=native -flto` for a far faster `make bot`. Verify rather than assume:

Run: `mingw32-make lab && ./build/release/sweep_lab --values=1000 --seeds=4 --requests=20000 --quiet --print-hash`, then rebuild the lab targets with `-O1 -fno-lto` and compare.
Expected: identical hashes. If they differ, something is reading floating point or UB inside a session and **that is a finding to report, not to work around** — keep the release flags and say so in Task 7.

- [ ] **Step 6: Commit**

```bash
git add Makefile tests/test_arena.cpp
git commit -m "Add arena_lab and sweep_lab, and prove a user bot cannot reach the evidence

sweep, arena and every test are compiled without PX_ARENA_USER_BOTS, so a
user bot is not in their translation units at all. Evidence: 17/17
command-line checks, including that sweep refuses a user-bot name, that
sweep_lab accepts it, that an installed-but-unnamed bot leaves sweep_lab's
session hash equal to sweep's, and that a user bot that does not compile
leaves mingw32-make test green."
```

---

## Phase B — installing a bot

### Task 3: The installer's logic

**Files:**
- Create: `arena/bots/user_install.hpp`, `tests/user_bot_fixtures/*`
- Test: `tests/test_arena.cpp` (new `test_user_install()`, called from `main`)

**Interfaces:**
- Produces, all pure — no file I/O:
```cpp
struct InstallCheck {
    bool ok = false;
    std::string name;   // registry name: the file stem
    std::string error;  // why not, when !ok
};
[[nodiscard]] InstallCheck check_user_bot(std::string_view filename, std::string_view text);
[[nodiscard]] std::string render_manifest(const std::vector<std::string>& names);
```

The contract `check_user_bot` enforces: the file is `<stem>.hpp`; `<stem>` matches `[a-z][a-z0-9_]*`; `<stem>` is not one of `kBotNames`; every `#include` line names `arena/bot.hpp` and nothing else; and the text contains `make_<stem>`, the factory the manifest calls, which the user declares as
`inline std::unique_ptr<BotHandle> make_<stem>() { return make_bot(YourStruct{}); }`. Requiring a factory is what lets the installer avoid parsing C++ for a struct name.

- [ ] **Step 1: Write the fixtures**

`good_bot.hpp` (valid, integer-only, `make_good_bot`), `collides.hpp` (valid, to be installed as `maker`), `bad_include.hpp` (includes `arena/session.hpp`), `no_factory.hpp` (no `make_no_factory`), `silent.hpp` (valid, never returns an action — used by Task 6).

- [ ] **Step 2: Write the failing test** `test_user_install()`

```cpp
InstallCheck ok = check_user_bot("good_bot.hpp", read_fixture("good_bot.hpp"));
PX_CHECK(ok.ok);
PX_CHECK(ok.name == "good_bot");
PX_CHECK(!check_user_bot("maker.hpp", read_fixture("collides.hpp")).ok);
PX_CHECK(!check_user_bot("placebo.hpp", read_fixture("good_bot.hpp")).ok);
InstallCheck bad = check_user_bot("bad_include.hpp", read_fixture("bad_include.hpp"));
PX_CHECK(!bad.ok);
PX_CHECK(bad.error.find("arena/session.hpp") != std::string::npos);
InstallCheck nf = check_user_bot("no_factory.hpp", read_fixture("no_factory.hpp"));
PX_CHECK(!nf.ok);
PX_CHECK(nf.error.find("make_no_factory") != std::string::npos);
PX_CHECK(!check_user_bot("good_bot.cpp", read_fixture("good_bot.hpp")).ok);
PX_CHECK(!check_user_bot("Good-Bot.hpp", read_fixture("good_bot.hpp")).ok);
PX_CHECK(!check_user_bot("9bot.hpp", read_fixture("good_bot.hpp")).ok);
PX_CHECK(!check_user_bot("", "").ok);
// The manifest is sorted and stable, so reinstalling cannot churn it.
const std::string m = render_manifest({"zeta", "alpha"});
PX_CHECK(m.find("alpha") < m.find("zeta"));
PX_CHECK(render_manifest({"zeta", "alpha"}) == m);
// An empty manifest still declares the symbols the guarded registry needs.
PX_CHECK(render_manifest({}).find("add_user_bot") != std::string::npos);
```

- [ ] **Step 3: Run to verify it fails**

Run: `mingw32-make build/debug/test_arena.exe`
Expected: FAIL to compile — `check_user_bot` not declared.

- [ ] **Step 4: Implement `arena/bots/user_install.hpp`**

Names sorted, LF endings only, and `render_manifest({})` must still define `add_user_bot` and `kUserBotNames` so a lab build with no user bots compiles.

- [ ] **Step 5: Run to verify it passes**

Run: `mingw32-make test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add arena/bots/user_install.hpp tests/user_bot_fixtures tests/test_arena.cpp
git commit -m "Add the user-bot installer's validation and manifest rendering

Evidence: test_arena covers the good case, collisions with a built-in and
with placebo, an include past the bot contract, a missing factory, four bad
filenames, and that an empty manifest still declares what the guarded
registry needs."
```

---

### Task 4: `make bot BOT=<path>`

**Files:**
- Create: `tools/install_bot.cpp`
- Modify: `Makefile`
- Test: `Makefile` `test-cli`

**Interfaces:**
- Consumes: `check_user_bot`, `render_manifest`.
- Produces: `make bot BOT=<path>` — validates, copies the header into `arena/bots/user/`, regenerates `$(BUILD)/generated/px_user_bots.hpp`, runs `verify-user-bot`, and on success prints the two commands to run it and to score it. On any failure it restores the previous manifest, removes nothing it did not add, and exits non-zero. `make verify-user-bot BOT=<path>` compiles a TU that includes `arena/bot.hpp` and the header and nothing else, proving it stands alone.

- [ ] **Step 1: Write the failing checks** in `test-cli` — Review Focus item 3

```
! mingw32-make bot                                            # no BOT= given
! mingw32-make bot BOT=does/not/exist.hpp
! mingw32-make bot BOT=arena/bots                             # a directory
! mingw32-make bot BOT=tests/user_bot_fixtures/good_bot.cpp   # wrong suffix
! mingw32-make verify-user-bot BOT=tests/user_bot_fixtures/bad_include.hpp
mingw32-make verify-user-bot BOT=tests/user_bot_fixtures/good_bot.hpp
```

Raise the count to 23.

- [ ] **Step 2: Run to verify it fails**

Run: `mingw32-make test-cli`
Expected: FAIL — `bot` does not refuse a missing `BOT=`.

- [ ] **Step 3: Implement `tools/install_bot.cpp` and the rules**

A path may arrive quoted or containing spaces, so handle both. The tool does the file I/O that `user_install.hpp` deliberately does not, and it is the only new place that writes to the tree. Print, on success, exactly the commands a reader should run next — `./build/release/arena_lab --bots=...` and `./build/release/sweep_lab --score=<name>` — because the whole value of this task is that nobody has to read a document to find out what to do.

- [ ] **Step 4: Run to verify it passes**

Run: `mingw32-make test-cli`
Expected: `23/23 command-line checks passed`.

- [ ] **Step 5: Commit**

```bash
git add tools/install_bot.cpp Makefile
git commit -m "Add make bot BOT=<path>: install a user bot and say what to run

Evidence: 23/23 command-line checks, covering a missing BOT=, a nonexistent
path, a directory, the wrong suffix, and a header that reaches past the bot
contract failing the standalone compile."
```

---

## Phase C — the score

### Task 5: `--score=NAME`, paired against a placebo

**Files:**
- Modify: `arena/sweep.cpp`
- Test: `Makefile` `test-cli`

**Interfaces:**
- Consumes: `Options`, `Cell`, `Spread`, `spread_of`, `paired`, `column`, `run_one`, `roster_from_spec`, `Session::StartError`, `SessionStats::mean_gap_ticks()`, `stats().stalled`.
- Produces: `Options::score`. With it set, each shared seed is run twice over the same explicit line-up — once with `<name>` in the final seat, once with `placebo` in it — and the output prints, in this order: the book-to-fundamental gap for both arms; the named bot's paired value with its standard error and the smallest effect that seed count can resolve; then each baseline participant's value in both arms with the shift. `--score` and `--param` are mutually exclusive. `--score` prints **no session hash** and says why.

- [ ] **Step 1: Write the failing checks** — Review Focus items 4 and 5

```
! ./sweep_lab --score=nope --seeds=2 --requests=2000 --quiet
! ./sweep_lab --score=good_bot --param=maker.reaction_ns --values=1000 --seeds=2 --requests=2000 --quiet
! ./sweep_lab --score=good_bot --set=noise.count=100 --seeds=1 --requests=2000 --quiet
./sweep_lab --score=good_bot --seeds=2 --requests=2000 --quiet
./sweep_lab --score=silent  --seeds=2 --requests=2000 --quiet | grep -q stalled
./sweep_lab --score=good_bot --seeds=2 --requests=2000 | grep -qv '^hash'
```

Raise the count to 29.

- [ ] **Step 2: Run to verify it fails**

Run: `mingw32-make test-cli`
Expected: FAIL — `--score` is an unknown flag, exit 2.

- [ ] **Step 3: Implement `--score`**

Print the gap before any P&L, and say plainly when a paired range straddles zero, reusing the existing wording. A stalled arm reports `stalled` and no value. Both arms must use the same explicit line-up, differing only in the occupant of the last seat — derive it from `build_roster`'s defaults rather than hard-coding a second copy, so the two cannot drift. State in the output that the scoring baseline is the default line-up **plus one inert seat**, and so is not identical to the published market.

- [ ] **Step 4: Run to verify it passes, and that the evidence still reproduces**

Run: `mingw32-make test-cli`
Expected: `29/29 command-line checks passed`.

Run: `./build/release/sweep --seeds=8 --requests=100000 --replay-check`
Expected: `replay: every session run twice, hashes identical`, and the same numbers as before this plan.

- [ ] **Step 5: Commit**

```bash
git add arena/sweep.cpp Makefile
git commit -m "Score a user bot against the baseline, paired against a placebo seat

Both arms carry the same extra seat, so roster size and owner ids match and
the difference is the bot rather than a re-interleaved session. Prints the
gap first and no session hash, because a user bot may use floating point and
cross-platform replay cannot be promised for it. Evidence: 29/29
command-line checks; plain sweep reproduces the committed numbers unchanged."
```

---

### Task 6: Score the built-in bots, and reconcile the documents

**Files:**
- Modify: `docs/ARENA.md`, `README.md`, `CLAUDE.md`, `handoff.md`, `bench/summarize_arena.py`, `bench/run_arena_evidence.sh`
- Create: `docs/LAB.md`; delete `docs/LAB_PLAN.md`

- [ ] **Step 1: Score the participants that already exist**

Run `--score` on `maker`, `rival`, `informed`, `noise`, `fader` and `momentum`. This needs no user bot, gives a published table on known quantities, and is the honest demonstration that the harness works. Add it to `bench/run_arena_evidence.sh` so it regenerates, and teach `bench/summarize_arena.py` to print it — a number in the documentation that script does not print has no evidence behind it.

- [ ] **Step 2: Update `docs/ARENA.md`**

Its boundary claim — "runtime bot loading of any kind... nothing a running session will accept from outside itself" — **stays true and must not be weakened**, because nothing is loaded at runtime. What changes: "Writing a bot" gains `make bot BOT=`, and "What is not built" gains the drag-and-drop mechanism with the reason it was cut. Record in the "four things that were wrong first" idiom that this was designed as a drop-on-the-viewer feature and cut after review, with the measured 27s-per-translation-unit rebuild and the empty-audience argument, because that is the part worth reading.

- [ ] **Step 3: Update `CLAUDE.md`, `handoff.md` and `README.md`**

`CLAUDE.md`'s "The strategy lab" section describes the cut design and must be replaced with this one; add `make bot`, `make lab` and the two lab binaries to the build block; add a gotcha that `arena`, `sweep` and the tests are compiled without `PX_ARENA_USER_BOTS` and that this is what keeps published evidence out of a stranger's reach. `handoff.md`: move the lab from §6 into §3 with its commit range. Do not add a viewer key to either: the viewer was not touched.

- [ ] **Step 4: Turn this plan into `docs/LAB.md`**

Keep the council's findings and the four revisions — the design that was cut is the most useful thing in here. `docs/ARENA.md` is what became of the arena's plan; this follows it.

- [ ] **Step 5: Verify everything**

Run: `mingw32-make all`
Expected: 7 tests, 29 command-line checks, benchmarks, REPL and arena green, zero warnings.

Run: `wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/Philb/prediction-markets && make BUILD=build-asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all' test"`
Expected: PASS. The installer is the only new file I/O in the tree, so this is the run that matters.

- [ ] **Step 6: Commit**

```bash
git add docs/ARENA.md docs/LAB.md README.md CLAUDE.md handoff.md bench/summarize_arena.py bench/run_arena_evidence.sh results/
git rm docs/LAB_PLAN.md
git commit -m "Document the strategy lab, and score the built-in participants

The boundary prose stands: nothing is loaded at runtime, so what changed is
that installing a bot is now one make target. Records the drop-on-the-viewer
design that was cut and why. Evidence: mingw32-make all green on Windows,
make test green under ASan/UBSan on Linux, and every table regenerated by
bench/run_arena_evidence.sh."
```
