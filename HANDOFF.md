# Handoff: customisable experiments (strategy settings and suite files)

Stopped on 2026-10-27, partway through change B. Nothing has been pushed. The task was the
"make exchange-lab's experiments customisable without weakening them" prompt: A (a strategy's
own settings) and B (a suite defined in a file). The design is in `docs/CUSTOMISE_PLAN.md`.

## Branches, all local

| Branch | Holds | State |
|---|---|---|
| `release-1.0` | the v1.0.0 work, tag `v1.0.0` | done; see `TODO.md` for what is left before going public |
| `customise-settings` | **A**: on top of `release-1.0` | **done**: code, tests, docs; every check passes on Linux and Windows |
| `customise-suite-file` | **B**: on top of A | code and tests committed; **docs, exercise and some checks left** (below) |

A and B were built on `release-1.0`, not `main`, because both touch files the v1.0.0 work
changed (`arena/suite.hpp`, `arena/suite.cpp`, the Makefile, the app). They are meant as two
pull requests, A first, each after `release-1.0`.

## What B has

- `arena/suite_file.hpp`: the reader (`parse_suite_file`), the definition lines with a digest
  (`suite_definition`), `suite_from_file`, `suite_file_from_record` and `admit_suite`.
- `SuiteDef::definition`, appended to `conditions_text()`; empty for every built-in suite.
- `suite_lab --suite-file=PATH`, `make compare CANDIDATES=x SUITE_FILE=path`,
  `/compare x suite=NAME` (`suites/NAME.cfg`, or a path); `run.suite_file` in `suite.cfg`;
  `reproduce` rebuilds from the record.
- Admission: exit 3, naming the seeds, writing nothing. Refusals (exit 2): beside `--market`,
  `--suite` or `--block`; in the evidence `suite`; reserved names; fixed and unknown keys.
- `examples/suites/slow_seat.cfg`: standard with the seat at 100 µs.
- Tests: `test_arena` (`test_suite_file`, three checks each paired with a planted defect),
  `test_app` (the `/compare ... suite=` plans), `test-lab` ("suite-file checks", 12),
  `test-cli` (the evidence suite refuses a file; "evidence-refusal checks" now 6).

## Left to do on B, in order

1. **Finish the Linux checks** on `customise-suite-file`: `make test-app`, `make test-data`,
   `make canonical` (expect 26 identical lines). Already passing on B: `make test`,
   `test-cli`, `test-lab`, `recipe-check`.
2. **Docs.** Turn the plan's B section into:
   - `docs/RUNNER.md`, "A suite of your own": the format, what a file cannot set, admission,
     the fingerprint and the digest, use and reproduction, and **when to write a suite file and
     when `/sweep` is enough**. The task asks for that guidance to go with the `slow_seat`
     example, whose header already points to this section. The reader takes no comment after
     a value on the same line, so the plan's `set.maker... # any number` example must not be
     copied as written.
   - `docs/REPRODUCE.md`: `run.suite_file`, the `suitefile.*` lines including the digest, the
     refusal of a changed file and of an edited record, and re-admission on reproduce.
   - `docs/APP.md`: the `/compare ... suite=S` row. `README.md`: the commands table.
   - `GLOSSARY.md`: "Suite file" and "Admission (of a suite file)".
   - `CLAUDE.md`: "Strategies of your own" still says a different set of conditions is a new
     suite in `arena/suite.hpp`. Add suite files, the reserved names and the digest.
   - Then **delete `docs/CUSTOMISE_PLAN.md`**. The A half is already in `docs/RUNNER.md`
     ("A strategy's own settings") and `docs/REPRODUCE.md`.
3. **Exercise 11 in `docs/LEARN.md`**, covering a setting (`/compare imbalance+ratio=5,imbalance`,
   `/sweep ... --param=imbalance.ratio`) and a suite file (`/compare imbalance
   suite=examples/suites/slow_seat.cfg`, then reproduce it with the file moved aside). Run every
   command as written through `build/release/exchange-lab` (in WSL: `build-linux/release/exchange-lab`)
   and quote the real output. Then change "ten exercises" to eleven in `README.md` (two places),
   the `docs/LEARN.md` intro and `docs/APP.md`, and `-eq 10` to `-eq 11` in the Makefile's
   `test-app`. Remove the runs and files the walkthrough leaves in `runs/`, `data/` and `~`.
4. **`CHANGELOG.md`**: an "Unreleased" section for A and B under its five headings. No pinned
   fingerprint or hash moved; the new conditions are `setting.*` (A) and `suitefile.*` (B).
5. **The full check on Windows** of the branch tip, from WSL (MSYS2 needs its PATH set):

   ```bash
   /mnt/c/msys64/usr/bin/bash.exe -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /c/Users/Philb/prediction-markets && for t in recipe-check test test-cli test-lab test-app test-data canonical; do mingw32-make BUILD=build-win $t || exit 1; done'
   ```

6. **The report the task asks for**, for each of A and B: what was built, the final syntax with
   one example command and its output, test counts before and after, confirmation that the
   canonical record and the pinned fingerprints did not move, and every departure from the
   prompt (below).

## Numbers so far, for the report

| | before (`release-1.0`) | after A | after B |
|---|---|---|---|
| `test_arena` | 1900 | 1931 | 1978 |
| `test_app` | 348 | 356 | 367 |
| `test-cli` | 11 + 42 + 4 | + 5 evidence refusals | evidence refusals 6 |
| `test-lab` | 7 + 9 + 5 | + 15 strategy settings | + 12 suite files |
| `test-app`, `test-data` | 25; 37 + 11 + 6 + 23 + 11 | unchanged | not yet re-run |
| `make canonical` | 26 identical | 26 identical (Linux, Windows) | not yet re-run |

- A: `suite_lab --candidates=imbalance+ratio=5,imbalance` on the standard block gives
  `imbalance+ratio=5 vs placebo -15.87 +- 3.00` (exactly exercise 10's copied `picky.hpp`) and
  `imbalance vs imbalance+ratio=5 -19.05 +- 1.88`. The fingerprint stays `9e8d07b24ee5d562`,
  standard's on that block, and `ratio=3`, the default, gives the session hash of no setting.
- B: `suite_lab --suite-file=examples/suites/slow_seat.cfg --candidates=imbalance,maker` gives
  `imbalance vs placebo -7.60 +- 1.94` (against −34.92 at the standard 10 µs seat) and
  `maker vs placebo +62.03 +- 7.38`, with fingerprint `6d8419db371945ea`.

## Departures from the prompt, to report

- **Base and pushing:** built on `release-1.0`, as local branches, nothing pushed (the user's
  choice). So `make canonical` is 26 lines, not the prompt's 25: `release-1.0` added
  `derived.holm.report.txt`.
- **The sixth member** is one visitor, `settings(F&&)`, that both names the settings and gives
  write access with a range, so the harness checks every value and the author writes no parsing.
- **Settings** are conditions of the run but not fingerprinted (they belong to the candidate,
  not the market). The run directory and `suite.cfg` keys write `=` as `_`. Help text for the
  evidence programs is unchanged; only the lab builds mention settings and suite files.
- **Suite files** also refuse `session.*` and `fair.*` keys and seeds that overlap `holdout`,
  and reserve `holdout`, `all`, `sensitivity` and its conditions beside the prompt's names. A
  `suitefile.digest` line was added, because without it an edited record rebuilds a suite that
  agrees with itself and is re-run instead of refused. `reproduce` re-admits a file suite.
- A planted defect was not built for every new check: where a check is a refusal in a shell
  test, its pair is the same input accepted by the lab program or an untampered run.
