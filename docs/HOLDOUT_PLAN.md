# A holdout seed block: plan

Written 2026-10-26, before any code changes, as `CLAUDE.md` asks. It is task 5 of the
pre-publication list. When it is built, this file becomes part of `docs/RUNNER.md` ("The
seeds") and is deleted.

## Why

Someone who tries ten variants of a strategy on `standard` and reports the best is quoting
the best of ten draws. Its 95% interval no longer means what it says, because the seeds
chose the variant as much as the variant earned the result. The fix is old and simple: keep
a block of seeds that no tuning has seen, and run the finished strategy on it once. That
gives a confirmatory result beside an exploratory one. `docs/RUNNER.md` and
`docs/CONTROLS.md` both list it as not built.

## What was checked before planning

- **The seeds are admissible.** Seeds 1,000,000..1,000,063 pass the reference market's
  health gate on all 64 seeds, in all three families. This comes from one run on
  2026-10-26 that read only `reference_admissible` and the fingerprints, and was deleted
  unread. Nobody has looked at a strategy's numbers on these seeds.
- **The fingerprints are reproducible without a candidate.** Computed from the placebo arm
  alone, they are `6e1fe05931a6f560` (`standard`), `8d95ab9f5a9e2d80` (`wide`) and
  `b66de703b5562711` (`busy`), the same values the suite binary reports.
- **Pinning costs about six seconds.** 64 placebo sessions take 1.6, 1.8 and 2.8 s in the
  debug build on Linux. `test_arena` takes 8.4 s today.
- **Nothing existing moves.** A fingerprint covers the `block.*` condition lines, so the
  `standard` and `smoke` fingerprints are untouched. A smoke run's files, which the
  canonical record holds, are unchanged too, so `tests/canonical.txt` stays as it is. No
  suite version bump is needed, because no existing set of conditions changes. The new
  block is a new scenario definition, and `CHANGELOG.md` records it as one.

## Design

1. **The block.** `kSeedBlocks` gains `{"holdout", 1000000, 64}`. It has 64 seeds, like
   `standard`, so a confirmation has the same power as the exploration it checks: an
   interval as narrow, and an effect as small that it can resolve. It overlaps no other
   block, unlike `smoke`, which is a prefix of `standard`.
2. **The pins.** `test_arena` pins one holdout fingerprint per family, computed from the
   placebo arm alone. The placebo arm is the market with an empty seat, so pinning it
   reveals nothing about any strategy. **No test, example or document runs a candidate on
   the holdout.** That is the rule that keeps it a holdout, and the plan's tests are built
   around it.
3. **The report.** A holdout run's `report.txt` ends with this line: *holdout block:
   confirmatory. Tune on standard; run this once, when the strategy is final, and report
   it whatever it says. Never pool it with standard.* Only holdout runs carry the line, so
   no existing output changes. The line comes from one small function of the block, which
   `test_arena` checks for all three blocks without running anything.
4. **Never pooled.** Nothing in the tool combines runs from different blocks:
   `families.txt` sets families side by side within one block, and that stays true. Each
   run directory names its block in `suite.cfg`, its fingerprint and its default path
   (`runs/<name>-holdout/`).
5. **Commands.**
   - In the app, `/compare NAME holdout` sits beside `smoke`, and naming two blocks is
     refused.
   - `make compare CANDIDATES=x BLOCK=holdout`, and `suite` and `suite_lab` with
     `--block=holdout`. All of these work once the block exists. Their `--help` text
     gains the name.
   - `controls` refuses the holdout. The controls check the evaluator, `standard` and
     `smoke` are enough for that, and they run strategies on the block, which would spend
     it.
   - With `market=NAME`, a calibrated market is held to the health gate on the holdout
     seeds like any other block. If it fails, the run exits 3, as it should.
6. **Reproduction.** `make reproduce` needs no change: it reads the block from `suite.cfg`.

## Tests

- **`test_arena`:**
  - the three holdout pins;
  - that no seed of the holdout is in `standard` or `smoke`;
  - the confirmatory line: present for the holdout, absent for `standard` and `smoke`;
  - the existing pins, unchanged.
- **`test_app`:** `/compare imbalance holdout` plans `--block=holdout`, and `/compare
  imbalance smoke holdout` is refused.
- **`test-cli`:** `controls --block=holdout` exits 2.
- **`make canonical`:** unchanged, 25/25.

## Docs

- `docs/RUNNER.md`: the seeds section, the command-line table, and the "Not built" line
  removed.
- `docs/CONTROLS.md`: its "not built" entry removed, and why the controls refuse the
  holdout.
- `docs/APP.md` and the `/help` text for `/compare`.
- `GLOSSARY.md`: "holdout block" and "confirmatory".
- `CHANGELOG.md`: an unreleased entry.
- `CLAUDE.md`: the blocks, and the rule that nothing runs a candidate on the holdout.

How to use the holdout in practice (state a hypothesis, tune on `standard`, confirm on
`holdout`) is task 11's exercise in `docs/LEARN.md`. Here `docs/RUNNER.md` states the rule
in two lines.

## Not in this plan, and why

- **Enforcing "run it once".** The tool cannot know whether you tuned, short of keeping a
  register of runs across sessions, which would make results depend on history. The rule
  is stated where the result is printed, which is as far as an instrument can go.
- **A second holdout for repeated confirmation.** A holdout that has been looked at is
  spent. If a researcher needs a fresh one, that is a new named block, recorded in the
  changelog.
