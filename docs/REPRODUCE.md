# Reproducing a run

Step 5, the last of the five steps that turned the arena into a strategy research platform.
Steps 1 to 4 are the strategy interface, the standard experiment runner
(`docs/RUNNER.md`), the evaluation report (`docs/REPORT.md`) and the evaluator's controls
(`docs/CONTROLS.md`). This is the design document. It was written after building rather
than as a plan first, because the shape was already fixed by step 2: `suite.cfg` used the
`--set=` names from the start so that it could be read back.

## One command

```bash
make reproduce RUN=runs/imbalance-vs-maker-standard            # into runs/imbalance-vs-maker-standard-reproduced/
make reproduce RUN=runs/imbalance-vs-maker-all-standard        # a SUITE=all run, family by family
build/release/suite --reproduce=runs/maker-standard --out=elsewhere   # built-ins only
```

`make reproduce` uses `suite_lab`, which can see installed strategies. `suite` can reproduce a
run of built-in strategies, and refuses one that needs an installed strategy.

The command reads everything from the run: the suite, the block, the candidates and their
versions. `--reproduce` accepts nothing else except `--out`, so a reproduction cannot quietly
differ from the run it reproduces.

## What a run records

Every `suite.cfg` holds four kinds of line:

- **The conditions:** the suite, its version, the line-up, the session length, the seat, the
  health gate, the block, every session setting and every reference participant's parameter,
  under the names `--set=` takes. Their text and every placebo session hash make the
  fingerprint.
- **What ran:** the binary, the installed strategies it could see, and each candidate's
  origin, declared delay and timer (`run.*`).
- **Which version of each installed strategy ran:** `run.candidate.<name>.digest`, the
  `cksum` CRC of its header, which `make strategy` computes into the generated manifest. A
  built-in strategy has no digest; its version is the commit.
- **Which build ran it:** `build.commit` (12 characters of `git rev-parse`), `build.dirty`
  (whether tracked files differed from that commit), `build.compiler`, `build.platform` and
  `build.flags`. The Makefile writes them into `$(BUILD)/generated/px_build_info.hpp`, which
  is regenerated on every build but rewritten only when it changes, so it costs no rebuild.

The `run.*` and `build.*` lines are never conditions and never fingerprinted. The same
conditions on two builds must stay comparable, since that is exactly what a reproduction on
another machine asks.

## What `--reproduce` does

1. **Refuses before running anything** (exit 2) if the run is incomplete (no
   `status=complete` in `outcome.cfg`), if its suite or block is not in this build, if a
   candidate cannot be built here, if an installed strategy's checksum differs from the one
   recorded, or if a single condition line differs. The refusal names the first difference,
   for example `maker.quote_qty=40 in the run, maker.quote_qty=50 here`, and writes nothing.
2. **Re-runs the suite** into the output directory, exactly as `make compare` would, with
   its own replay and placebo checks.
3. **Compares every file the run wrote**, byte for byte: `sessions.csv`, `participants.csv`,
   `comparison.csv`, `evaluation.csv`, `attribution.csv`, `report.txt` and `outcome.cfg`.
   `suite.cfg` is compared on its condition lines, since its `run.*` and `build.*` lines may
   differ by design. Each file is marked identical or DIFFERENT, then the whole run is
   `REPRODUCED` (exit 0) or `NOT REPRODUCED` (exit 1). From a smoke run, with the paths
   shortened:

```
reproduce runs/imbalance-vs-maker-smoke into runs/imbalance-vs-maker-smoke-reproduced
  built from 4fee6b45d8af (dirty) then, 4fee6b45d8af (dirty) now
  suite.cfg         identical
  sessions.csv      identical
  participants.csv  identical
  comparison.csv    identical
  evaluation.csv    identical
  attribution.csv   identical
  report.txt        identical
  outcome.cfg       identical
  REPRODUCED: every file the run wrote came out the same
```

**Why every file, and not the fingerprint alone.** The fingerprint covers only what the
candidates faced: the conditions and the placebo's sessions. A candidate whose own behaviour
changed, through state carried between sessions or a dependence on something outside the
session, leaves the fingerprint alone and changes `sessions.csv`. Comparing the raw files
catches both, and comparing the derived files catches a change in the report's arithmetic.

## What it does not claim

- **Cross-platform reproduction of an installed strategy is checked, not promised.** Built-in
  strategies are integer-only, and the committed arena evidence is identical on Windows and
  Linux. An installed strategy is a stranger's header. `make reproduce` on the other platform
  will say whether it reproduced, and that is the claim.
- **A dirty build is recorded, not refused.** `build.dirty=1` says the tree had uncommitted
  changes, so the commit alone does not identify the code. The condition check and the file
  comparison still say whether the result is the same.
- **Reading a run back into a report is not built.** The report can be regenerated by
  re-running, which `--reproduce` does. Parsing the CSVs back into a `RunRecord`, to compare
  two runs made months apart without re-running either, would duplicate what re-running proves.

## Where it lives

- `arena/suite.hpp`: `BuildInfo`, `CandidateInfo::digest`, the `build.*` lines in
  `suite_cfg_text`, `condition_lines` and `cfg_value`.
- `arena/suite.cpp`: `this_build`, `digest_of`, `reproduce_one`, and `--reproduce` in `main`.
- `Makefile`: `$(BUILD_INFO)`, the digest function in the generated manifest, and
  `make reproduce RUN=...`. `test-cli` gains eight checks (the build is recorded; a run
  reproduces; changed conditions are refused and write nothing; a changed raw file does not
  reproduce; `--reproduce` takes nothing but `--out` and refuses a directory with no run; a
  `SUITE=all` run reproduces family by family). `test-lab` gains four (an
  installed strategy is recorded by checksum; `make reproduce` reproduces it; `suite` refuses
  it; a changed checksum is refused).
- `tests/test_arena.cpp`: `test_suite_cfg_reading`.
