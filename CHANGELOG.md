# Changelog

Every release of exchange-lab, newest first. Results are comparable only between runs whose
fingerprints are equal, so each entry names what changed in six places, even when the
answer is "nothing":

- the engine;
- the strategies;
- the scenario definitions (seed blocks, suites and their parameters);
- the report's calculations;
- the output formats (run directories, tapes, market files);
- every pinned hash or fingerprint that moved.

Cite a tagged release, not `main` (`CITATION.cff`).

## Unreleased

**Engine.** Unchanged.

**Strategies.** Unchanged.

**Scenario definitions.** A new seed block, `holdout`: seeds 1,000,000 to 1,000,063, kept for
confirming a finished strategy once (`docs/RUNNER.md`). It is the first block marked
confirmatory. Suite version 1 is unchanged, because no existing set of conditions changed.

**Report calculations.** Unchanged. A holdout run's `report.txt` ends with a line saying it
is confirmatory and is never pooled with `standard`.

**Output formats.** Unchanged. `--block` and `/compare` accept `holdout`, and `controls`
refuses it.

**Pinned hashes and fingerprints.** None moved. Three are new, each a family over the
holdout block, pinned from the placebo arm alone: `standard` `6e1fe05931a6f560`, `wide`
`8d95ab9f5a9e2d80` and `busy` `b66de703b5562711`.

## v1.0.0, 2026-10-26

The first release. It is `main` after #2 and #1 merged the arena, the strategy research
platform, the terminal app and market data onto the matching engine, plus the licence,
citation file and contributor pack (#4). As the first release it has nothing to move against,
so each section below records the baseline that later releases are compared with.

**Engine.** The price-time-priority matching engine as it was before the arena, unchanged.
`matching_engine.*`, `sorted_levels.hpp` and `messages.hpp` are as they were. The
portability work changed only the timer and platform layer around it (`docs/PLATFORMS.md`):

- `include/px/cycle_clock.hpp` replaces `tsc_clock.hpp`.
- `include/px/platform.hpp` and `src/platform_{linux,win32}.cpp` changed, and
  `src/platform_posix.cpp` is new.
- The pipeline in `sim/pipeline.hpp` and `include/px/histogram.hpp` changed to use the new
  timer and platform code.

The event-stream hashes below show it behaves identically.

**Strategies.** The reference market is `maker`, `rival`, `informed`, `noise` and
`momentum`. Beside it are `placebo`, the inert strategy of the placebo arm; `roundtrip`,
the positive control; and `fader`, the worked example.

**Scenario definitions.** Suite version 1. The seed blocks are `standard` (seeds 42 to 105)
and `smoke` (42 to 45). The scenario families are `standard`, `wide` and `busy`. A
candidate sits in seat 6 at a reaction delay of 10,000 ns, and a session ends on 25 ms of
simulated time (`docs/RUNNER.md`).

**Report calculations.** Twelve metrics, paired against the placebo arm. A seat's value is
exactly spread + adverse + carry, and its attribution to each reference participant is
exact. The statistics are computed in `double` outside the session, with floating-point
contraction off for every compiler (`docs/REPORT.md`).

**Output formats.**

- A run directory holds `suite.cfg`, `sessions.csv`, `participants.csv`,
  `comparison.csv`, `evaluation.csv`, `attribution.csv`, `report.txt` and `outcome.cfg`.
- Tapes are `tape.version=1`. The importers are `csv 1`, `lobster 1`, `databento 2`,
  `tardis 1`, `binance 1`, `coinbase 1`, `kraken 1` and `itch 1`.
- Calibrated market files are version 1.

**Pinned hashes and fingerprints.** None moved, because this is the baseline:

| What | Value |
|---|---|
| engine determinism (`test_determinism`, seed 42) | `77a560d8ffb4fa43` |
| pipeline, seed 42 | `63bb035a395bae79` (thin), `75ec1ab46ce5260a` (deep) |
| arena session | `2853d2127a3486c5` |
| arena still frame | `ddfa48c44bff4de8` |
| suite `standard` | `d461c30ab2446acf` |
| suite `wide` | `9bb1a7c184ccb2d7` |
| suite `busy` | `5d735a9faa384bf5` |

`tests/canonical.txt` holds the rest of the canonical record, 25 lines in all, identical
on all five CI targets.
