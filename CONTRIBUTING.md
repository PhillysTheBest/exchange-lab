# Contributing

exchange-lab is an instrument for teaching and research, so a change is judged first on
whether every published result can still be reproduced, byte for byte, on every platform.
The rules below exist for that. Each one says why, because a rule that looks arbitrary is
the one that gets broken in good faith.

You do not need a pull request to use a strategy of your own: `make strategy
STRATEGY=path/to/yours.hpp` installs it locally, and `docs/LEARN.md` shows the rest. A pull
request is for changing the project itself. For anything larger than a fix, open an issue
first, so we can agree what the change is before you build it.

## The rules

1. **Integer arithmetic inside a session.** No `float` or `double` anywhere a session
   runs, and none in an importer. A `std::log` is free to differ between library
   versions, and one differing digit would silently break cross-platform replay. The
   report's statistics are the one exception: they are computed in `double` outside the
   session, and the canonical record labels them `derived.*`.
2. **Never hand-edit `results/`, or a number in the docs that comes from it.** Engine numbers
   come from `bench/run_evidence.sh` and arena numbers from `bench/run_arena_evidence.sh`.
   A number that `bench/summarize_results.py` or `bench/summarize_arena.py` does not print
   has no evidence behind it.
3. **A new importer comes with a hand-made sample and a test.** The sample goes in
   `examples/data/`, made up by hand so that it carries no one else's licence, and the
   test in `tests/test_data.cpp`. The importer's version string changes whenever what it
   writes changes. Never commit a real data file, a tape or a market file: paid data
   usually may not be redistributed.
4. **A new control comes with a planted defect.** Add a defect in `test_arena` (or
   `test_data`) that makes the control fail. A control that has never been seen to fail
   might be one that cannot.
5. **Say "strategy", never "bot".** Everything that takes part in a session is a strategy.
   Nothing in `arena/` is called latency: the engine's latency is measured, while a
   strategy's reaction delay is an input someone chose. Never say "tick-to-trade" either,
   because the engine's measurement is service time. `GLOSSARY.md` has the rest.
6. **No new dependencies.** There are no third-party libraries, no package manager and no
   CMake, only a hand-written Makefile and a C++20 compiler. The tool has no network
   connection, no browser interface and no AI model in it.
7. **Strategies live in `arena/strategies/`, never in the engine** (`include/px/`, `src/`).
   A new reference strategy changes the reference market, so it moves the suite
   fingerprints (rule 8).
8. **A moved hash means behaviour changed.** If a change moves a pinned hash, find out why,
   and say in the pull request that it was intended. A suite fingerprint that moves needs a
   suite version bump in `arena/suite.hpp`, not a silent re-pin, because two runs are
   comparable only if their fingerprints are equal. `tests/canonical.txt` is never
   re-recorded to make CI pass.
9. **New words and new commands are documented.** A new term gets an entry in `GLOSSARY.md`.
   A new command goes in `docs/LEARN.md` or the document it belongs to, and must run as
   written.

## Running the checks

On Linux and macOS:

```bash
make test test-cli test-lab test-app test-data canonical
```

On Windows, run the same line with `mingw32-make`, in an MSYS2 UCRT64 shell with
`diffutils` installed. Do not add `-j` to that line: `test-app` and `test-data` both write
under `runs/` and fail when they run at the same time. `make -j8 test` on its own is fine.

`make canonical` must end with every output identical to `tests/canonical.txt`. Two more
checks run in CI: `make memmove-check`, which shows the engine calls no `memmove`, and
`make recipe-check`, which keeps every Makefile command short enough for MSYS2's make. On
Linux, run the tests under the sanitizers too:

```bash
make BUILD=build-asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all' test
```

A clean build has no warnings under `-Wall -Wextra -Wpedantic`, and should stay that way.

## Commits and pull requests

- Work on a branch. Make each commit one coherent step, whose message says what changed
  and what the evidence was. Never commit a state where `make test` fails.
- Match the code around you: its naming, its comment density and its idiom.
- Line endings are LF, which `.gitattributes` pins. On Windows, a CRLF shell script fails
  under bash.
- The pull request template asks whether `make canonical` still matches, whether any
  fingerprint moved, and which test or control you added. Answer all three.

## Conduct and licence

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md). exchange-lab is
licensed under the [Apache License 2.0](LICENSE). By contributing, you agree that your
contribution is licensed under it too, as section 5 of the licence states.
