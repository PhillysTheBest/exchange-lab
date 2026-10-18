# Repositioning for researchers and students: what is left

On 2026-10-17 the project's audience was decided. exchange-lab is a research and learning
tool for students of market microstructure, and for researchers who need controlled,
reproducible experiments. It is not a backtester, and it is not a way to decide whether to
trade a strategy.

This file is the plan for the rest of that change. Nothing in it has been carried out.
Each step says what it changes, why, what depends on it and how to check it. When the
steps are done, delete this file, as CLAUDE.md asks of a finished plan.

## Already done, in the commit that added this file

- **`README.md` reordered for the two readers, with nothing removed.** It opens with what
  the project is and is not, a "Start here" table, the negative result, then a section for
  students and one for researchers. The researchers' section covers what it gives you, what
  it can and cannot answer, and the runner's known weaknesses. The workflow and the arena
  come next. The engine's measured numbers moved under "The engine underneath". The title
  is now `exchange-lab`, not `px`.
- **`docs/LEARN.md`, new.** Eight exercises for students, from one order book to a
  strategy of their own, scored, controlled and reproduced. Every command in it was run on
  Linux before it was written down, including the example strategy's code block, which
  compiled warning-free and was scored on the standard block.
- **`GLOSSARY.md`, new.** 128 terms, defined as this project uses them, each pointing
  at the file that defines it. It ends with the words the project avoids, and why.
  `CLAUDE.md` and `docs/agents/domain.md` already pointed at a root glossary that did not
  exist.
- **`CLAUDE.md`** records the audience, so later work is written for it.

## 1. Deletions and trims (proposed)

| # | What | Why | What depends on it | Check |
|---|---|---|---|---|
| D1 | **Delete `handoff.md`**, after moving its §2 (the viewer's purpose, what is on screen, how a frame is drawn) into `docs/ARENA.md` "The viewer" | It is an agent-handoff narrative, not a reader's document, and it is stale: "450 checks across seven tests" and "twelve commits", against 1,429 checks in `test_arena` alone today. Its §6 asks for a history scrub that `CLAUDE.md` now forbids ("Existing history keeps the trailers it already has; do not rewrite it"). Everything else in it is duplicated by README.md, `docs/ARENA.md` or `CLAUDE.md` | `CLAUDE.md` names it twice (the opening paragraph and the `ui.hpp` paragraph). The word "handoff" in `bench/bench_ring.cpp` is unrelated | `grep -rn handoff.md` finds nothing; `make test` passes |
| D2 | **Trim `docs/RUNNER.md` "How steps 3-5 plug in"** (about 90 lines) to a three-line pointer at `docs/REPORT.md`, `docs/CONTROLS.md` and `docs/REPRODUCE.md`. Reword its opening, which still introduces step 2 as the newest thing | It is the sketch written before steps 3-5 existed. The three design documents are now the record, and each says where it departed from the sketch. Keep "Known weaknesses" and "Rejected alternatives": the first is linked from the README, and `CLAUDE.md` asks that wrong approaches be kept | Only two sentences inside `docs/RUNNER.md` itself, its opening and the note that defers a holdout block, both of which the trim rewrites (`grep -rn "plug in"`) | The README's link to `docs/RUNNER.md#known-weaknesses` still resolves |
| D3 | **Shorten `CLAUDE.md`'s history, naming and local-checkout paragraphs** to the rules they carry: no market-data client or strategy in the engine; never name anything latency or tick-to-trade; the folder rename is pending | They tell a story a working session does not need. The rules inside them are what matters | Nothing; it is guidance text | none needed |
| D4 | **Delete the local branches `backup-main-pre-scrub` and `backup-arena-pre-scrub`** on your computer | They exist only for the history scrub that is no longer happening. They are not on GitHub | Nothing | `git branch` no longer lists them |
| D5 | **Optional: move `docs/agents/` to `.claude/agents-docs/`** so that `docs/` holds reader-facing documents only | It is contributor tooling for coding agents, and `issue-tracker.md` tells agents to use the `gh` CLI, which cloud sessions do not have | `CLAUDE.md` "Agent skills" names both files | The skills still find their configuration |

## 2. Things that stay, though they might look like candidates

- **`results/` (all four directories).** Every number in README.md is printed from them by
  `bench/summarize_results.py` or `bench/summarize_arena.py`. `CLAUDE.md` says a number
  those scripts do not print has no evidence behind it.
- **`bench/*.py`.** Python is out of scope for new work. These scripts are the only path
  from `results/` to the published tables, and `bench/frame_to_svg.py` draws the README's
  picture.
- **The sweep's 200,000-request sessions, beside the suite's 25 ms ones.** The published
  result came from the sweep. Both documents explain why the suite ends on simulated time
  instead, and `GLOSSARY.md` defines both.
- **`arena/strategies/fader.hpp`.** It is the worked example written against
  `arena/strategy.hpp` alone, and it is the evidence for a known weakness: it is inert on
  all 64 seeds of the 25 ms suite.
- **The engine's benchmark sections, the REPL and `docs/arena.svg`.** The engine is the
  instrument, so they were moved down the README, not removed. Exercise 1 of
  `docs/LEARN.md` uses the REPL.

## 3. Decisions only you can make

| # | Decision | Why it matters for this audience | Recommendation |
|---|---|---|---|
| C1 | **A licence.** None exists | Without one, nobody may legally reuse, modify or teach from the code. A course or a paper cannot depend on it | MIT for the fewest conditions, or Apache-2.0 if you want an explicit patent grant. Either is standard for research software |
| C2 | **A `CITATION.cff`** | GitHub then shows "Cite this repository", which is how researchers cite software | Tell me the name as you want it cited (and an ORCID if you have one), and I will write it |
| C3 | **Merge PR #2 into `arena`, then PR #1 into `main`** | `main` is the default branch, and its README is still the old "px — a price-time-priority matching engine" page. A visitor today sees no arena, no strategies and no research workflow, so none of this repositioning is visible until both merges land | Build the branch once on Windows first (C5), then merge |
| C4 | **GitHub description and topics**, set in the repository's settings | The description is what search shows, and topics are how people browsing a subject find a repository | Description: *Price-time-priority matching engine in C++20 (p99.9 under 2µs, zero allocation) and a deterministic market simulator for teaching and researching trading strategies: paired experiments, tested controls, byte-for-byte reproduction.* Topics: `market-microstructure`, `matching-engine`, `order-book`, `market-making`, `agent-based-simulation`, `quantitative-finance`, `reproducible-research`, `education`, `cpp20`. Once it is set, `CLAUDE.md`'s description paragraph changes to match |
| C5 | **A Windows build of this branch** | Every step of the research platform was built and tested on Linux only, and `CLAUDE.md` promises a zero-warning build on both | Run `mingw32-make test test-cli test-lab` in MSYS2 UCRT64 before merging |

## 4. Additions that need no decision

These are useful to the new audience. They are listed so they are not lost; none of them
has been started.

- **A holdout seed block.** It would be used by no test or example, for reporting after
  tuning on `standard` (`docs/CONTROLS.md`, "Not done"). A researcher who tunes against the
  report needs one.
- **A zero-intelligence baseline**, a random limit-order strategy. It would be a better
  floor to publish beside a candidate than `roundtrip`.
- **Worked answers to the questions in `docs/LEARN.md`**, kept in a separate file so a
  student can try first.

## Order

1. C5, the Windows build (you).
2. D1-D3 in one commit, with `make test test-cli test-lab` green (Claude, on your go-ahead).
3. C1 and C2, once you have chosen (Claude writes the files).
4. C3, the two merges (you).
5. C4, the description and topics (you, in GitHub settings), then `CLAUDE.md` to match.
6. D4 on your computer, and D5 if you want it.
