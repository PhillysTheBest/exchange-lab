# Changelog

Each release says what it changed in the **engine**, the **strategies**, the **scenario
definitions** (suites, seed blocks, conditions), the **report calculations** and the **output
formats**, and names every **fingerprint or hash that moved**. Two runs are comparable only if
their suite fingerprints match, so a reader of a result needs to know which release made it.
Cite a tagged release, never `main` ([`CITATION.cff`](CITATION.cff)).

## Unreleased

Experiments a researcher can customise without weakening them: a strategy's own settings, and
a suite defined in a file. Both are taken by the lab programs only, and with neither given
every output is byte for byte what v1.0.0 wrote.

### Fingerprints and hashes

**Nothing moved.** The engine determinism hash, the session and frame hashes, every pinned
suite fingerprint (`standard`, `wide`, `busy`, holdout and the sensitivity conditions) and the
26 lines of `tests/canonical.txt` are unchanged. The new condition lines are `setting.*`
(conditions of the run, compared by `reproduce`, **not** fingerprinted, because a setting
belongs to the candidate and not to the market) and `suitefile.*` (fingerprinted, written only
by a suite file, so no built-in suite carries one). A setting at its default value gives the
session hash of no setting. `examples/suites/slow_seat.cfg` has the fingerprint
`eabf7a1767548125` on its 64 seeds; it is an example, not pinned.

### Engine

No change.

### Strategies

- **A strategy's own settings.** A strategy of your own may have an optional sixth member,
  `settings(F&&)`, which names the integer fields a run may set, each with its range; the
  harness, not the strategy, checks every value. `examples/strategies/imbalance.hpp` names
  `ratio`, `qty` and `max_position`. A strategy without it compiles and runs as before. Built-in
  strategies have none; their fields stay `--set=` names. No reference strategy changed.

### Scenario definitions

- **Suite files** (`suites/NAME.cfg`, `arena/suite_file.hpp`): a suite of one's own, with a
  name, version, opposing line-up, session length, seat delay, seed block and reference
  parameters. The seat, placebo arm, health gate and step ceiling are fixed, as are `session.*`,
  `fair.*` and `market.*`; unknown and repeated keys are refused. Names reserve every built-in
  suite, block and sensitivity condition, `file`, and any calibrated market's name; seeds may
  not overlap `holdout`. A file suite is admitted only if its placebo passes the health gate on
  every seed of its block, or `suite_lab` exits 3 naming the seeds and writes nothing.
- **Example:** `examples/suites/slow_seat.cfg`, the standard market with the seat at 100 µs.

### Report calculations

No change. A setting's label (`imbalance+ratio=5`) is an arm's name in the report like any other.

### Output formats

- **Settings on a name:** `imbalance+ratio=5`, keys sorted into one label
  (`imbalance+qty=20+ratio=5`), in `/compare`, `make compare CANDIDATES=`, `suite_lab
  --candidates=`, `sweep_lab --strategies=`, and `--param=NAME.KEY` / `--set=NAME.KEY=V` in a
  sweep, which then adds a paired table for that participant. `suite`, `sweep` and `arena`
  refuse a setting (exit 2).
- `suite.cfg`: one `setting.<id>.<key>=<value>` line per setting, `<id>` the label with `=`
  written as `_`, which also names `run.candidate.<id>.*` and the run directory
  (`runs/imbalance+ratio_5-vs-imbalance-standard/`).
- `suite.cfg` of a file suite: `suitefile.format`, `suitefile.set.*`, `suitefile.digest`
  (conditions) and `run.suite_file` (not a condition); `block.name=file`.
- `reproduce` rebuilds settings from `run.candidates` and a file suite from the run's own record,
  without the file; it re-admits a file suite, refuses an edited setting or record line, and
  refuses while `run.suite_file` still exists and differs.
- New: `suite_lab --suite-file=PATH`, `make compare SUITE_FILE=`, `/compare NAME suite=S`
  (`suites/S.cfg`, or a path). Refused by `suite`, and beside `--market`, `--suite` or `--block`.
  `suites/` is git-ignored, like an installed strategy.
- The app explains exit 3 as "the market is not admissible on this build", not "the reference
  market", since a suite file can be the market, and explains it after `/reproduce` too.

### Documentation and project files

- `docs/RUNNER.md`: "A strategy's own settings" and "A suite of your own", with when to write a
  suite file and when `/sweep` is enough. `docs/REPRODUCE.md`: what both record and refuse.
  `GLOSSARY.md`: strategy setting, suite file, admission of a suite file.
- Exercise 11 in `docs/LEARN.md`: a setting and a suite file, scored and reproduced.

## v1.0.0 (2026-10-26)

The first tagged release. It starts from `main` at the merge of pull requests #2 and #1
(`6886cea`, 2026-10-26), which held the finished engine, the arena and its published negative
result, the strategy research platform (interface, standard runner, report, the evaluator's
controls, one-command reproduction), market data (importers, calibration, replay), the terminal
app, and the five-target CI. This entry lists what changed since that merge.

### Fingerprints and hashes

**Nothing that existed moved.** The engine determinism hash (`77a560d8ffb4fa43`), the session
and frame hashes, the sweep's seed-42 hashes, the `standard`, `wide` and `busy` smoke
fingerprints (`d461c30ab2446acf`, `9bb1a7c184ccb2d7`, `5d735a9faa384bf5`), every sample tape
and the known-answer tape are unchanged, and the 25 lines of `tests/canonical.txt` that existed
are identical. A market calibrated with nothing held back has the fingerprint it had before
(`85106d50923a64eb` on the known-answer tape).

**New pins:**

| What | Fingerprint | Pinned in |
|---|---|---|
| `holdout` block, standard suite | `6e1fe05931a6f560` | `kPinnedHoldoutFingerprint` |
| `delay-5us`, smoke | `c84d3a36febd5f83` | `kPinnedSensitivityFingerprints` |
| `delay-20us`, smoke | `3899f12e9a01ff39` | the same |
| `thin`, smoke | `cc9bfe4fe4bc3243` | the same |
| `no-momentum`, smoke | `20488d4a03f1b808` | the same |
| `no-rival`, smoke | `0fb101e0d939d17b` | the same |

`tests/canonical.txt` gains one line, `derived.holm.report.txt`, the only report that carries
the Holm correction's p-values.

### Engine

No change.

### Strategies

- **Added `zi`**, a zero-intelligence floor after Gode and Sunder: random limit orders within 8
  ticks of the mid, one at a time, within ±200. Built in, never in a default line-up, so no
  reference market changed. `test-cli` now checks every built-in strategy is integer-only.
- **Added `examples/strategies/frozen.hpp`**, a learned model frozen into integer weights,
  generated by `examples/frozen/train.py`. An example to install, like `imbalance`; not built in.
- No reference strategy changed.

### Scenario definitions

- **Seed block `holdout`**: 1,000,000..1,000,063, confirmatory. Tune on `standard`, report once
  on `holdout`, never pooled.
- **The sensitivity protocol** (`--suite=sensitivity`): `standard` and six conditions, each a
  suite of its own: `delay-5us`, `delay-20us` (the seat's delay), `thin`, `busy` (noise flow),
  `no-momentum`, `no-rival` (line-up). `busy` is the existing family. A third-noise-trader
  condition was tried and left out: its placebo failed the health gate on 3 of 64 seeds.
- Market names now also reserve `sensitivity` and the six condition names.

### Report calculations

- **Holm correction.** A run with more than one value comparison prints Holm-adjusted p-values
  beside the raw intervals, and `families.txt` adds one across every family cell. The metric
  rows are counted as before and not corrected. p-values come from Student's t by finite sums,
  with + − × ÷ and `sqrt` only. A run with one candidate prints exactly what it printed before.
- **Sensitivity verdicts.** `sensitivity.txt` says, for each comparison, whether its sign holds
  in every admissible condition, and gives its size as a range of shares of standard's.
- **Calibration identification.** `calibrate` lists the other parameter sets the search ran whose
  matched statistics lie within 2 s.e. of the chosen one's.
- **Stylised facts** (`stylised_facts()`, `/facts`): return and |return| autocorrelation, tails,
  volume against |return|, spread and depth percentiles, order flow per print. Integer, a
  diagnostic, never a gate. `tape_stats()` gained an optional window, and its default is the
  statistic it was, field for field.

### Output formats

- `report.txt`: a Holm section when there is more than one value comparison; a `confirmatory`
  line on the holdout block. **Consequence, checked:** a smoke run of `maker,momentum` made at
  `6886cea` reproduces under v1.0.0 in every file but `report.txt`, which lacks the Holm
  section, so `/reproduce` says NOT REPRODUCED. A one-candidate run reproduces in every file.
  Re-run a multi-candidate run on v1.0.0 before reporting it.
- `families.txt`: a Holm section.
- New `sensitivity.txt` beside one directory per condition; `/runs` and `/report` read it.
- `markets/NAME.cfg`: `identified.*` lines always; with `holdback=PCT`, a `calibration.holdback`
  condition line and `holdback.*` result lines. A market without a holdback has the same
  condition lines as before.
- `calibrate` prints the identification line, a held-back table when asked (2b) and the
  stylised facts it did not fit (step 4).
- New commands: `/facts NAME` (`make facts TAPE=`), `/calibrate NAME holdback=PCT`
  (`HOLDBACK=`), `/compare NAME holdout`, `/compare NAME sensitivity`.

### Documentation and project files

- Apache License 2.0 (`LICENSE`, `NOTICE`), covering the code, the documentation, the evidence
  and the hand-made samples.
- `CITATION.cff`, `CONTRIBUTING.md`, a bug template and a pull request template.
- `docs/CODING-AGENT.md`; exercise 10 in `docs/LEARN.md` (a study from hypothesis to holdout);
  new sections in `docs/RUNNER.md` (the floor, sensitivity, the holdout), `docs/REPORT.md`
  (Holm) and `docs/DATA.md` (identification, holdback, stylised facts).
- Still to do before going public: a code of conduct and an ORCID (`TODO.md`).
