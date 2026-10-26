# Reproducing a run

How to check that a run's result is real: re-run it from its own record and compare every
file it wrote, byte for byte.

```
> /reproduce                                     # the latest run
> /reproduce runs/imbalance-vs-maker-standard    # into runs/imbalance-vs-maker-standard-reproduced/
> /reproduce runs/imbalance-vs-maker-all-standard   # a run of every family, family by family
```

Without the app:

```bash
make reproduce RUN=runs/imbalance-vs-maker-standard
build/release/suite --reproduce=runs/maker-standard --out=elsewhere   # built-ins only
```

`make reproduce` uses `suite_lab`, which can see installed strategies. `suite` can reproduce a
run of built-in strategies and refuses one that needs an installed strategy.

Everything is read from the run: the suite, the block, the candidates and their versions.
`--reproduce` accepts nothing else except `--out`, so a reproduction cannot quietly differ
from the run it reproduces.

## What you see

From a smoke run, with the paths shortened:

```
reproduce runs/imbalance-vs-maker-smoke into runs/imbalance-vs-maker-smoke-reproduced
  built from 5f8954e23bf5 (dirty) then, 5f8954e23bf5 (dirty) now
  suite.cfg         identical
  sessions.csv      identical
  participants.csv  identical
  comparison.csv    identical
  holm.csv          identical
  evaluation.csv    identical
  attribution.csv   identical
  report.txt        identical
  outcome.cfg       identical
  REPRODUCED: every file the run wrote came out the same
```

Each file is marked identical, DIFFERENT or MISSING, then the whole run is `REPRODUCED`
(exit 0) or `NOT REPRODUCED` (exit 1). A run written before `holm.csv` existed (v1.0.0 and
earlier) shows it as MISSING and does not reproduce on a later build: the later build writes a
file the run never had.

## What it does, in order

1. **Refuses before running anything** (exit 2), and writes nothing, if:
   - the run is incomplete (no `status=complete` in `outcome.cfg`);
   - its suite or block is not in this build;
   - a candidate cannot be built here;
   - an installed strategy's checksum differs from the one recorded;
   - the run was on a calibrated market whose file, `run.market_file`, has been calibrated
     again since; or
   - a single condition line differs. The refusal names the first difference, for example
     `maker.quote_qty=40 in the run, maker.quote_qty=50 here`.
2. **Re-runs the suite** into the output directory exactly as `/compare` would, with its own
   replay and placebo checks.
3. **Compares every file the run wrote**, byte for byte. `suite.cfg` is compared on its
   condition lines only, because its `run.*` and `build.*` lines may differ by design.

**Why every file, and not the fingerprint alone.** The fingerprint covers only what the
candidates faced: the conditions and the placebo's sessions. A candidate whose own behaviour
changed, through state carried between sessions or a dependence on something outside the
session, leaves the fingerprint alone and changes `sessions.csv`. Comparing the raw files
catches that, and comparing the derived files catches a change in the report's arithmetic.

## What a run records

Every `suite.cfg` holds four kinds of line:

| Kind | Lines | Part of the fingerprint? |
|---|---|---|
| The conditions | the suite, its version, the line-up, the session length, the seat, the health gate, the block, every session setting and every reference participant's parameter, under the names `--set=` takes | yes, with every placebo session hash |
| What ran | `run.*`: the binary, the installed strategies it could see, each candidate's origin, declared delay and timer | no |
| Which version of an installed strategy | `run.candidate.<name>.digest`: the `cksum` CRC of its header, computed by `/add` (`make strategy`). A built-in has no digest; its version is the commit | no |
| Which build | `build.commit` (12 characters of `git rev-parse`), `build.dirty`, `build.compiler`, `build.platform`, `build.flags` | no |

A run on a calibrated market (`/compare NAME market=M`) also records `calibration.*` lines:
the market's name, the tape it was matched to and that tape's `cksum`, its depth and the
statistics matched. They are conditions, so the market is a suite of its own with its own
fingerprint, and the parameters it set are among the participants' lines. That is enough to
re-run it: `/reproduce` rebuilds the market from the run's own `suite.cfg`, **without the tape
or the market file**, so a run made from paid data can be reproduced by someone who does not
have the data ([`DATA.md`](DATA.md)).

The `build.*` lines come from `$(BUILD)/generated/px_build_info.hpp`, which the Makefile
regenerates on every build but rewrites only when it changes, so it costs no rebuild. They are
never conditions: the same conditions on two builds must stay comparable, since that is
exactly what a reproduction on another machine asks.

## What it does not claim

- **Cross-platform reproduction of an installed strategy is checked, not promised.** Built-in
  strategies are integer-only, and the committed arena evidence is identical on Windows and
  Linux. An installed strategy is a stranger's header; `/reproduce` on the other platform
  says whether it reproduced, and that is the claim.
- **A dirty build is recorded, not refused.** `build.dirty=1` says the tree had uncommitted
  changes, so the commit alone does not identify the code. The condition check and the file
  comparison still say whether the result is the same.
- **A run cannot be read back into a report without re-running it.** Parsing the CSVs back
  to compare two old runs would duplicate what re-running already proves.

## Where it lives

- `arena/suite.hpp`: `BuildInfo`, `CandidateInfo::digest`, the `build.*` lines in
  `suite_cfg_text`, `condition_lines` and `cfg_value`.
- `arena/suite.cpp`: `this_build`, `digest_of`, `reproduce_one`, and `--reproduce` in `main`.
- `Makefile`: `$(BUILD_INFO)`, the digest function in the generated manifest, and
  `make reproduce RUN=...`.
- Tests: `test-cli` checks that the build is recorded, a run reproduces, changed conditions
  are refused and write nothing, a changed raw file does not reproduce, `--reproduce` takes
  nothing but `--out` and refuses a directory with no run, and a run of every family
  reproduces family by family. `test-lab` checks that an installed strategy is recorded by
  checksum, reproduces, is refused by `suite`, and is refused when its checksum changes.
  `tests/test_arena.cpp` has `test_suite_cfg_reading`.
