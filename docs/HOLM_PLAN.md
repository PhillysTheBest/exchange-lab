# A Holm correction for the seat-value tests: plan

Written 2026-10-26, before any code changes, as `CLAUDE.md` asks. It is task 6 of the
pre-publication list. When it is built, this file becomes part of `docs/REPORT.md` ("Many
comparisons") and is deleted.

## The decision this rests on

`docs/REPORT.md` says a correction "would need a decision about which metrics are hypotheses at
all". It was decided on 2026-10-26:

- **The seat's value is the primary outcome.** Holm adjusts its tests and nothing else.
- **The other eleven metrics are descriptive.** They stay uncorrected, and the report says so.

Several metric separations are true by construction (`maker` has a higher maker share than
`imbalance`). Correcting them would mix mechanics with hypotheses, which is the mixing
`REPORT.md` warns about.

## What exists, and what it constrains

- **The family is already counted.** `compare()` produces the seat-value rows: each candidate
  against the placebo, then each later candidate against the first. The report's "Many
  comparisons" line counts the rows whose verdict is `separated` or `not_separated`. **That
  set is the Holm family.** Rows that are `identical` or `not_admissible` are not tests now,
  and they are not tests under Holm either. The existing count stays as it is, as the task
  asks.
- **The report computes no library mathematics.** Its t critical values are a table, "so the
  same number comes out on every platform", and its only function is `sqrt`, which IEEE 754
  rounds correctly everywhere. `docs/PLATFORMS.md` names the library's maths functions as a
  difference it has not ruled out. Holm needs p-values, so they must come from arithmetic
  alone (below).
- **The canonical run has one candidate.** It is `--candidates=maker --block=smoke`, so its
  family has one test, and for one test Holm changes nothing. If the report prints the Holm
  section only when the family has two or more tests, the existing 25 canonical lines stay
  identical.
- **`reproduce` compares exactly the files in `kFiles`.** A file missing from either side is
  `MISSING`, and the run is not reproduced.

## Design

1. **p-values without library functions.** A test's statistic is t = mean / s.e. with
   n − 1 degrees of freedom, as the interval already uses. Its two-sided p-value comes from
   the closed form of Student's t distribution for whole degrees of freedom (Abramowitz and
   Stegun 26.7.3 and 26.7.4):
   - **Even df:** a finite sum in cos²θ = ν/(ν + t²), times sinθ = t/√(ν + t²). This is
     arithmetic and `sqrt` only.
   - **Odd df:** (2/π)(θ + sinθ cosθ times a finite sum), where θ = atan(t/√ν).

   `atan` is written by hand. For x > 1 it uses π/2 − atan(1/x). Two halvings with
   atan(x) = 2 atan(x / (1 + √(1 + x²))) bring the argument below 0.2. A fixed 15-term series
   then finishes it. Every step is `+ − × ÷` or `sqrt` in a fixed order, and contraction is
   already off, so the bits are the same on every target.

   If s.e. = 0 and the mean is not zero, then p = 0. The p-value is printed to six decimals,
   which hides the tail's cancellation error. That error is near 10⁻¹⁶, and it only arises
   where p is far below any threshold.
2. **Holm.** Sort the family's p-values in ascending order, keeping the row order to break
   ties, and adjust each one to p̃₍ₖ₎ = max over j ≤ k of min(1, (m − j + 1) · p₍ⱼ₎). A test is
   `separated` under Holm when p̃ < 0.05, which is strict to match "the interval excludes 0".
   The family-wise error rate is held at 5%.
3. **`holm.csv`.** It is new, written by every run, one row per seat-value test:
   `suite,baseline,arm,seeds,t_stat,df,p_value,family_tests,holm_p,holm_verdict`. Rows that
   are not tests (identical, or not admissible) are listed with an empty p-value and their
   verdict, so the file lines up with `comparison.csv`. It is in `kFiles`, so `reproduce`
   compares it. As a result, a run written before this change does not reproduce on a later
   build. The run records `holm.csv` as `MISSING`, and `CHANGELOG.md` says so.
4. **`report.txt`.** When the family has two or more tests, a section follows the paired
   table:

   ```
   Holm, over the N seat-value tests above (family-wise 5%):
     arm vs baseline          | p        | Holm p   | verdict
   ```

   The "Many comparisons" line keeps its count. When the Holm section is present, its
   wording changes from "no correction is applied" to "the seat-value tests are corrected
   above; the metrics are descriptive and uncorrected". With one test, nothing in
   `report.txt` changes.
5. **`families.txt`** (`--suite=all`). Holm runs across every family's seat-value tests
   together, because a reader who reports several families has made all of those tests. The
   estimates stay unpooled: correcting tests is not pooling estimates. Each family's own
   `report.txt` and `holm.csv` correct within that family only, and `families.txt` says which
   adjustment to quote when families are reported together.
6. **A stated limit.** The raw verdict uses the t table's three decimals, while Holm uses the
   exact p-value. Within 0.0004 of the critical value the two can disagree, and `REPORT.md`
   says so. The raw verdict is not changed, because it is in the canonical record.

## Tests

- **`test_arena`, the t distribution against known answers:**
  - at df = 1, p(1) = 0.5, and p = 1 − (2/π) atan(t);
  - at df = 2, p = 1 − t/√(2 + t²);
  - for every row of `t975` (df 1..30, 63, 127), p(t975(df), df) is within 10⁻⁴ of 0.05. The
    table is an independent published source, and its three decimals move p by at most about
    6 × 10⁻⁵;
  - the hand-written `atan` agrees with `std::atan` to 4 × 10⁻¹⁶ over a grid. `std::atan` is
    used only inside this test, never in any output.
- **`test_arena`, Holm:**
  - a textbook example: p = 0.01, 0.04, 0.03, 0.005 gives Holm 0.03, 0.06, 0.06, 0.02, and
    rejects two;
  - adjusted p is never below raw p, never above 1, and keeps the order;
  - one test is unchanged;
  - ties keep the row order.
- **`test_arena`, the report:** with two candidates the section appears, and its m equals the
  "Many comparisons" count of seat-value tests. With one candidate it does not appear.
- **`test-cli`:** `holm.csv` is among the files a run writes, and two runs write it
  byte-identically.
- **`make canonical`:**
  - the existing 25 lines are unchanged;
  - two lines are added, labelled derived: `holm.csv` and `families.txt` from a two-candidate
    smoke run with `--suite=all`;
  - the five CI targets must agree on both lines, which is the evidence that the hand-written
    distribution gives the same bits everywhere.

## Docs

- `docs/REPORT.md`: "Many comparisons" gains Holm, the primary-outcome decision and its
  reason, and the stated limit. The "Not built" entry goes.
- `docs/RUNNER.md`, `CLAUDE.md`: the run directory's file list gains `holm.csv`.
- `docs/PLATFORMS.md`: the canonical record grows to 27 lines, and the p-values use no
  library functions.
- `GLOSSARY.md`: "Holm correction", "family-wise error rate", "primary outcome" and
  "descriptive metric". "Many comparisons" is updated.
- `CHANGELOG.md`, the unreleased entry:
  - report calculations: Holm;
  - output formats: `holm.csv`;
  - no fingerprint moved;
  - the canonical record gains two lines;
  - runs written before this change do not reproduce.

## Not in this plan, and why

- **Correcting the descriptive metrics.** That was decided against, above.
- **Other corrections** (Benjamini–Hochberg, Bonferroni). Holm controls the family-wise
  rate, which suits a small family of pre-declared tests. False-discovery control suits
  screening many hypotheses, and nothing here screens.
- **p-values for the metrics.** Their intervals stay as they are, with no new column.
