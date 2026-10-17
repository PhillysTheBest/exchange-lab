# Handoff

Where the project stands, what was built, and the things a reader will want
explained before touching anything. `CLAUDE.md` is the working rulebook,
`README.md` the published results, `docs/ARENA.md` the arena's design and the
mistakes behind it. This file is the narrative that ties them together.

**State:** all work is on the branch `arena`, open as pull request
[#1](https://github.com/PhillysTheBest/exchange-lab/pull/1) against `main`,
twelve commits, nothing uncommitted. The suite is 450 checks across seven tests
plus eleven command-line checks, green on Windows and under ASan/UBSan on Linux.

---

## 1. What the project was, and what it is now

It was a matching engine: the part of an exchange that takes buy and sell orders
and decides who trades with whom, in what order, at what price. Single-threaded,
no heap allocation after start-up, int64 fixed-point prices, fed by lock-free
rings, with the measurement and test machinery to back its latency claims. That
part was finished before this work started and **has not been touched since** —
`git diff` over `include/ src/ sim/` is empty across all twelve commits, and the
pinned determinism hash is still `77a560d8ffb4fa43`.

What was added is `arena/`: a seeded simulated market of strategies trading *inside*
that engine, each with a reaction delay the experimenter sets, plus a terminal
renderer to watch it and a headless runner to sweep it. About 4,300 lines.

The project's centre of gravity moved as a result. The engine is now the
instrument; the experiment is the result. The strategy lab, whose first three steps
are built (§6), moves it once more: it makes the instrument something other people
can submit a strategy to, so "add your own strategy" becomes part of what the project
is rather than a paragraph about what it would permit. That is why the repository is being
renamed to `exchange-lab` rather than `matching-engine` — see the name section in
`CLAUDE.md` for the reasoning and the two names that must be avoided.

---

## 2. The terminal: what it is and why it exists

`build/release/arena` opens a retro desktop drawn in 24-bit ANSI — a purple grid
with chunky windows, hard drop shadows and block-character meters. There is a
picture at the top of README.md's arena section, generated from a seed rather
than screenshotted.

### Its purpose

The engine was finished, correct and measured, and **a reader could not see
anything**. The repository was numbers and tests. The terminal exists so that an
order book is a thing you watch move rather than a table you read, and so that
the experiment's central claim — a slow participant loses to a fast one — is
visible as one bar shrinking while another grows, rather than a figure in a CSV.

It is also a deliberate scope *limit*. Making the engine watchable could have
meant a web server, a browser and a package manager. It instead meant one header
that owns a grid of coloured cells, because the actual requirement is a 2D array
of glyphs and one write syscall.

### What is on screen

| Window | Colour | Shows |
|---|---|---|
| ORDER BOOK | teal | The ladder. Asks above, bids below, size as bars, with the fundamental value printed on the dividing line so you can see how far the book has drifted from a truth no participant can observe. Drawn from the engine itself, so it is the exchange's view rather than any participant's. |
| PARTICIPANTS | violet | One row per strategy: reaction delay, position, marked value as a bar running both ways from zero. The informed trader's row is orange. |
| REACTION DELAY | orange | Each strategy's delay on a log slider spanning 1 µs to 1 ms. The dial the experiment turns, labelled *simulated, not measured* on the window itself. |
| SESSION | teal | Seed, simulated clock, orders, trades, and the book-to-fundamental gap — the number that says whether what you are watching is a market at all. |
| TAPE | magenta | Prints as they happen: direction, price, size, which participant was the aggressor. |
| ! NOTABLE | magenta | A modal dialog, on a print that moved the mid several ticks, and on anything the `f` key has to tell you. |

### How it works

Each frame composes into a grid of cells in `arena/ui.hpp`, which knows nothing
about trading — no price, no order, no participant appears anywhere in it.
`arena/arena.cpp` is the only place the renderer and the market meet. The frame
is written in **one** `fwrite`, with a colour escape emitted only where a colour
changes, so a full 120×40 repaint is a few kilobytes rather than 40,000 escape
sequences and a frame can never be seen half-drawn. The cursor is parked at home
rather than the screen cleared, so there is no flash between frames.

`arena/term.hpp` holds the four things a terminal is actually needed for: turn on
24-bit colour, ask how big the window is, read a key without blocking, and put it
all back on the way out. It is the only file that reaches for `<windows.h>` and
`<termios.h>`, and exactly one translation unit includes it.

No curses. What is needed here is a 2D array of coloured glyphs and one write;
ncurses would bring a build dependency, a wide-character configuration problem
and an input model, in exchange for optimising a repaint already small enough not
to matter at thirty frames a second.

### Controls

```
space  pause and resume          1-9  focus a participant
+ -    faster, slower (1/64x..512x)   f    reload the script file and restart
                                      q    quit
```

**There is no mouse support at all** — no clicking, no dragging, no resizing
panes. The layout is computed from the terminal size each frame, so it reflows
when you resize the window, but the panes are not movable. That was a scope
decision: dragging panes would mean parsing mouse escape sequences, hit-testing,
z-order and a focus model — a window manager, to make six fixed panes movable.
The strategy lab does not change that either: an early design accepted a dragged
file, and that was cut on review, so installing a strategy is a make target and the
viewer is not involved at all.

**No control can change what happens.** The viewer paces a session by asking it
to advance to a simulated timestamp; the keyboard only changes how fast it asks.
A run watched at 512×, paused halfway and resumed has the same session hash as
the same seed run headless. That is why `f` *restarts* rather than injecting: a
participant appearing at the instant a human pressed a key would be a wall-clock
event, and the session could never be reproduced again.

### Running it

```bash
mingw32-make arena
build/release/arena                                   # watch the default market
build/release/arena --maker-reaction=1000000          # watch a slow maker get left behind
build/release/arena --strategies=maker,informed,noise:2     # choose the line-up
build/release/arena --script=tools/arena_script_demo.txt   # put yourself in it
build/release/arena --still=60000000 --seed=42        # one reproducible frame, then exit
```

Needs a terminal that interprets 24-bit colour and at least 96×32 characters —
Windows Terminal, or any modern Linux terminal. It refuses with a clear message
rather than drawing garbage if either is missing.

---

## 3. What was built, in order

Twelve commits on `arena`. The themes, not the diffs:

**The core** (`a1d22f2`). A session owns a `MatchingEngine`, a simulated
nanosecond clock, a roster of strategies and the accounting. The reaction model is the
new mechanism: a strategy with delay *d*, woken by something at time *t*, acts at
*t + d* and sees the world as of *t* — one knob buys both halves of being slow,
because its view is stale and its order is late. Queue position is reconstructed
from the `ExecEvent` stream alone in `arena/book_mirror.hpp`, so **no engine API
was added** and a strategy provably sees no more than a real participant would.

**The experiment** (`368e6c1`). A sweep *runner* rather than one hard-coded
sweep: any roster or session field can be the swept variable, several seeds per
point, CSV out.

**The viewer** (`b371ac1`). As above.

**The measurement** (`42964c0`), and the first surprise — see §4.

**Two bugs found by probing the finished build** (`0d9f413`). A roster above 64
participants was accepted and then silently mistreated: with 104 identical noise
traders the first 64 took 151.9 fills each and the rest took 55.9. A session that
stopped generating orders ran forever. Both are now refused or bounded at
construction and pinned by regression tests. Both fixes are provably no
behavioural change in the measured range, which the unchanged hashes confirm.

**The worked example** (`639e917`). `arena/strategies/fader.hpp`, written against the
strategy contract alone, so "anyone can add a strategy" is tested rather than claimed.

**The name** (`83f676d`). Settled on `exchange-lab`, with the reasoning.

**The ledger and a failed control** (`318dc83`). See §4.

**Composing a line-up** (`75b566d`). `--strategies=maker,rival,informed,noise:3`.

**Linux evidence, scripted trading, and a picture** (`ae02f92`).

**The `f` key** (`ec5345c`).

---

## 4. The result, and why it is not the tidy one

**What is 10 µs worth? Less than 24 seeds can resolve.** 100 µs and 1 ms both
cost real money. The standard error is published beside it, because
"underpowered" and "nothing there" are different claims and only one is usually
earned:

| Delay | Paired mean | Std error | Resolvable at 24 seeds | Verdict |
|---|---|---|---|---|
| 10 µs | −39.96 | ±56.19 | ±157 | underpowered, not null |
| 100 µs | −601.83 | ±93.05 | ±261 | separated |
| 1 ms | −3117.13 | ±74.71 | ±209 | separated |

**Then the null control overturned the obvious story.** Remove the informed
trader entirely — nothing left to be adversely selected by — and 96% of the 1 ms
effect remains. The ledger says where the money actually goes, and sums to
exactly zero: the maker's **−3117.13** goes to the **rival** at **+3665.65**,
while the informed trader ends up **−119.43 worse off**. It is competition for
uninformed flow, not adverse selection.

**And the positive control failed.** A null run cannot distinguish a measure that
works from one that is always zero, so a second control tries to create the
effect on purpose. Three designs, all three rejected by the instrument's own
health line — a maker in this session is not a probe you can insert without
perturbing what it measures, because jointly with the rival it *is* price
discovery. So the post-fill column's **sensitivity is not established**, and that
is published rather than glossed. What the reading rests on instead is analytic:
the fundamental is a martingale, so the expected post-fill move is zero for every
participant unless one *predicts* it, and none does.

This matters for anyone writing about the project. The honest summary is "I built
an instrument, asked it a question, and the controls changed the answer twice" —
not "I proved latency matters". The second version would be easier to tell and
would not survive contact with the evidence in `results/`.

---

## 5. Things that will bite you

- **Two engine rules stop at `arena/`'s edge.** Zero-allocation and
  no-C-runtime-on-the-hot-path are claims about `MatchingEngine::process()`.
  Arena code allocates freely and nothing in it is timed. The arena adds one rule
  of its own: **no floating point inside a session**, which is what makes results
  bit-identical across platforms.
- **Several arena parameters are load-bearing in non-obvious ways** — the
  informed trader's position limit, the maker's inventory limit, the learning
  rate, the inventory skew. Change one, read only the P&L, and you will get a
  confident wrong answer. Read the book-to-fundamental gap too. `docs/ARENA.md`
  records what each one cost to discover.
- **A session holds at most 64 participants** and refuses more. The ceiling is
  hard rather than a resizable buffer: changing how own-event wakes are marked
  would change wake *ordering* and move every session hash, invalidating the
  committed evidence.
- **Binaries and objects live in separate subtrees** (`build/release/` and
  `build/release/obj/`). They have to: object paths mirror the source tree, so
  without the split the viewer's binary and its own object directory are the same
  path. Windows hides the clash behind `.exe`; on Linux the directory wins and
  the viewer cannot be linked at all.
- **`BUILD=` picks the output tree**, for the evidence scripts as well as the
  Makefile. WSL executes Windows `.exe` files on `/mnt/c` through interop, so a
  Linux evidence run that probed for `build/release/sweep.exe` would silently
  measure the *Windows* binary and write it into `results/arena-wsl2/`.
- **Line endings.** `.gitattributes` pins LF, and Windows binaries write CRLF
  through stdout redirection, so generated evidence needs normalising before it
  is committed.

---

## 6. What is left

**Next:** turning the arena into a strategy research platform, in five steps
(strategy interface, standard experiment runner, evaluation report, validating the
evaluator, one-command reproducibility). Steps 1 to 3 are built.

- **Step 1, the strategy interface.** `make strategy STRATEGY=path/to/name.hpp`
  installs a strategy of your own and builds `sweep_lab`, `arena_lab` and `suite_lab`,
  which are the only binaries that can see it.
- **Step 2, the standard experiment runner.** `make compare CANDIDATES=a,b` puts every
  candidate in the same seat of the default market on the same 64 seeds, beside a
  placebo arm in which the seat never acts. It replays every candidate session, checks
  every placebo session against the market with no seat, writes the conditions and the
  raw per-session results to `runs/` (git-ignored, never `results/`), and prints the
  paired comparison with its standard error. `docs/RUNNER.md` is its design.

- **Step 3, the evaluation report.** The same run now says how the seat made its
  money (spread, adverse, carry, inventory, drawdown, paired on every metric), which
  reference participant paid for it, and, with `SUITE=all`, how it fares in the `wide`
  and `busy` scenario families. `docs/REPORT.md` is its design, including the `null`
  family that was built and dropped because its market fails the health gate.

Steps 4 and 5 (validating the evaluator, one-command reproducibility) are not built. See "Strategies of your own" in `CLAUDE.md`.

**Not done, and yours to do:**

- **Rename the local checkout.** The GitHub rename to `exchange-lab` is done, the
  description is set, and the remote points at the new URL. Only the local folder is
  still `prediction-markets`, because renaming it pulls the directory out from under a
  running session. When it moves, the WSL path in `CLAUDE.md`'s sanitizer command moves
  with it — it is the only path in the tree that names the folder.
- **Scrub the Claude co-author trailers from the 17 old commits.** Approved but
  blocked by a permission guard on destructive git. The command and the five
  files holding commit SHAs that would need remapping are known; backup branches
  `backup-main-pre-scrub` and `backup-arena-pre-scrub` are in place. New commits
  already carry no attribution, enforced by the checked-in `.claude/settings.json`.
- **Merge [PR #1](https://github.com/PhillysTheBest/exchange-lab/pull/1).**

**Deliberately not built,** so the list is bounded: runtime strategy loading of any
kind (no plugin DLL, no scripting language, nothing a running session accepts
from outside itself), mouse support, more than one instrument on screen, live
keyboard trading, and fee models beyond a flat per-unit fee. On the engine side:
order amend, stop orders, icebergs, auctions, a network gateway, FIX/ITCH/OUCH,
journaling, recovery, per-participant risk limits.

---

## 7. Verifying it in three commands

```bash
mingw32-make test       # 7 tests, 450 checks; test_determinism must print 77a560d8ffb4fa43
mingw32-make test-cli   # 11 checks: bad flags fail, unterminating configs are refused
build/release/sweep --seeds=8 --requests=100000 --replay-check
```

The third should report `replay: every session run twice, hashes identical`, a
book-to-fundamental gap of a few ticks on every row, and the paired column
separating only at the slow end. `python bench/summarize_arena.py
results/arena-windows` prints every table the documentation quotes — a number in
the docs that this script does not print has no evidence behind it.
