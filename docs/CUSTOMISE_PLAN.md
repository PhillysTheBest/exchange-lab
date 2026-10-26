# Plan: customisable experiments that stay as strong as fixed ones

Two changes, built in order on `release-1.0` (the v1.0.0 work), as two local branches: A
(`customise-settings`), then B (`customise-suite-file`) on top of it. When both are built, this
plan becomes sections of `docs/RUNNER.md` and `docs/REPRODUCE.md` and is deleted.

What must not move: `make canonical` (26 lines on `release-1.0`; the task's "25" predates the
Holm line), the pinned fingerprints (`d461c30ab2446acf`, `9bb1a7c184ccb2d7`, `5d735a9faa384bf5`,
the holdout and sensitivity pins) and hashes (`77a560d8ffb4fa43`, `2853d2127a3486c5`,
`ddfa48c44bff4de8`). With no setting and no suite file given, every output is byte for byte
what it was.

## A. A strategy's own settings

### The sixth member

Optional, so a strategy without it compiles and behaves exactly as before:

```cpp
// The settings a run may change, by name, each with the range it accepts.
template <class F> void settings(F&& f) {
    f("ratio", ratio, 1, 100);
    f("qty", qty, 1, 10'000);
    f("max_position", max_position, 1, 100'000);
}
```

One member both names the settings and gives write access to them: the caller passes a visitor,
and the strategy calls it once per setting with its name, the field and the inclusive range.
The harness, not the strategy, checks a value: an integer within the range and within the
field's own type. A strategy author writes one line per setting and no parsing. It is detected
with a C++20 `requires` on a named probe type (not a lambda in an unevaluated context, which
Apple clang 15 does not reliably accept). `StrategyHandle` gains a virtual `apply_setting`, so
the session, the suite and every wrapper reach it without knowing the strategy's type.

Built-in strategies do not get the member. Their fields stay `RosterConfig` fields under
`set_roster_param()`, so there is one mechanism for each kind of strategy, not two for the
built-ins.

### The syntax

A setting is attached to the strategy's name with `+`:

```
imbalance+ratio=5
imbalance+ratio=5+qty=20
imbalance+ratio=5:2                 # two of them, both with ratio 5 (sweep_lab line-ups)
```

- `,` still separates names and `:N` still repeats one, so nothing that parses today parses
  differently: no built-in or installed name may contain `+` (names are `[a-z][a-z0-9_]*`), and
  `:` is read first, from the right, before the settings.
- `+` needs no quoting in bash, in MSYS2 or in make (`CANDIDATES=imbalance+ratio=5`).
- Keys are `[a-z_][a-z0-9_]*`; values are whole numbers with an optional `-`. A key given twice
  is refused.
- The **canonical label** sorts the keys: `imbalance+qty=20+ratio=5` whichever order was typed.
  It is the arm's name everywhere: the report, `sessions.csv`, `participants.csv`,
  `run.candidates`. Two orders of one setting are one candidate, named twice and refused.

Where it is taken:

| Where | Form |
|---|---|
| `make compare`, `suite_lab --candidates=` | `CANDIDATES=imbalance+ratio=5,imbalance` |
| `/compare` | `/compare imbalance+ratio=5,imbalance` |
| `sweep_lab --strategies=`, `/sweep` | `--strategies=maker,rival,informed,noise:2,momentum,imbalance+ratio=5` |
| a sweep over one setting | `--param=imbalance.ratio --values=3,5,8` |
| a fixed setting in a sweep | `--set=imbalance.ratio=5`, the same as inline |

`--param=NAME.KEY` is a setting when NAME is a participant in `--strategies=` and `NAME.KEY` is
not a built-in field (`maker.*`, `session.*` and so on stay what they are). The sweep runs each
value on the same seeds, as it does for built-in fields, and adds a paired table for the
participant whose setting moves, because its maker-centred tables say nothing about it.

### Refusals, each naming the token, exit 2

- `imbalance+ratoi=5`: `imbalance has no setting 'ratoi'; its settings: ratio qty max_position`
- `imbalance+ratio=five`, `imbalance+ratio=`, `imbalance+ratio=0` (outside 1..100)
- `zi+band_ticks=4`: `zi has no settings member` (and for a built-in: its fields are `--set=` names)
- any `+` given to `suite`, `sweep` or `arena`: settings are taken by the lab programs only.
  `controls` and the tests take no strategies at all.

### Recording and reproduction

- A run with settings writes, after the roster lines, one condition line per setting:
  `setting.<id>.<key>=<value>`, where `<id>` is the label with `=` written as `_` (so a line's
  first `=` is always its key's end): `setting.imbalance+ratio_5.ratio=5`. Its `run.candidate.*`
  keys use the same id.
- They are **conditions** (compared by `reproduce`, line by line, before anything runs) but not
  **fingerprinted**: a setting belongs to the candidate, not to the market it faced, and runs of
  different candidates on one market must keep one fingerprint.
- `reproduce` rebuilds the settings from `run.candidates`, so a tampered or edited setting line,
  or a strategy that no longer accepts one of its keys, is refused (exit 2).
- The run directory uses the same id: `runs/imbalance+ratio_5-vs-imbalance-standard`.
- A run with no settings writes nothing new.

### Tests for A, each with its planted defect

| Check | Where | Planted defect that must make it fail |
|---|---|---|
| an unknown key is refused | `test_arena` | a lenient adapter that ignores unknown keys |
| a value outside the range or the type is refused | `test_arena` | a setter that clamps instead of refusing |
| a strategy without the member is refused and unchanged | `test_arena` | — (compile-time: a plain strategy still builds) |
| the label is canonical and the parse unambiguous with `,` and `:N` | `test_arena` | a parser that splits on the first `:` |
| `+` refused by an evidence build's `roster_from_spec` | `test_arena` | — |
| `suite`, `sweep` and `arena` refuse a setting, naming it | `test-cli` | the same token through `suite_lab` is accepted |
| a setting moves the strategy's behaviour, not the fingerprint | `test-lab` | — |
| a changed setting is caught by `reproduce` | `test-lab` | a run whose setting line was edited must be refused |
| `/compare a+k=v` and `/sweep --param=a.k` plan as above | `test-app` | — |

## B. A suite of one's own, as a file

### The format: `suites/NAME.cfg`

Integer text, `key=value`, `#` comments, in the style of `markets/NAME.cfg`:

```
# exchange-lab suite file (docs/RUNNER.md, "A suite of your own")
suitefile.format=1
suite.name=slow_seat
suite.version=1
suite.lineup=maker,rival,informed,noise:2,momentum
suite.session_ns=25000000
seat.reaction_ns=100000
block.first_seed=42
block.seeds=64
set.maker.half_spread_ticks=2      # any number of these: the names --set= takes
```

- **Set by the file:** the name, a version (≥ 1), the opposing line-up (a `roster_from_spec()`
  spec, installed strategies and their settings allowed), `suite.session_ns` (1 ms to 1 s),
  `seat.reaction_ns` (1 ns to 10 ms), the seed block (2 to 1024 seeds), and any reference
  parameter under its `--set=` name as `set.NAME=VALUE`.
- **Fixed, refused if present:** the seat (appended last, never informed), the placebo arm, the
  health gate (`health.*`), the step ceiling (`suite.step_ceiling`), and `session.*` and `fair.*`
  (the fundamental and the markout stay the standard's for now). Unknown and repeated keys are
  refused, naming the line.
- **Names** follow the market rule (`[a-z0-9][a-z0-9._-]*`) and refuse `standard`, `wide`,
  `busy`, `smoke`, `holdout`, `all`, `sensitivity` and its conditions, and the name of any
  calibrated market in `markets/`.
- **Seeds** may not overlap the `holdout` block, so a suite file cannot spend it.

### Admission: exit 3

Before any candidate runs, the placebo arm runs on every seed of the file's block and must pass
the health gate on each one. Otherwise nothing is written and `suite_lab` exits 3:
`suite slow_seat is not admitted: its placebo fails the health gate on 3 of 64 seeds:
79 (gap), 94 (gap), 105 (gap)`. `reproduce` admits again, from the record.

### The fingerprint

The conditions already carry the name, version, line-up, session length, seat delay and block.
A file suite adds its whole definition to them, sorted by key, as `suitefile.*` lines:
`suitefile.format=1` and one `suitefile.set.NAME=VALUE` per reference parameter. So two files
that differ in any line differ in fingerprint, even when a parameter they differ in never moves
the placebo (a `fader.*` value with no fader in the line-up). A built-in suite has no such line,
so no pinned fingerprint moves.

### Use, and reproduction

- `suite_lab --suite-file=PATH`, `make compare CANDIDATES=x SUITE_FILE=path`,
  `/compare x suite=NAME` (or a path). The block comes from the file, so `--block` is refused
  beside it, as are `--suite=` and `--market=` (combining a calibrated market with a suite file
  is out of scope for now, and the refusal says so).
- `suite.cfg` records the whole definition among its conditions and the file's path as
  `run.suite_file`, so `reproduce` rebuilds the suite from the record, with no file needed. If
  the file is still at that path and now differs, `reproduce` refuses, as it does for a market
  calibrated again.
- `suite`, the evidence program, refuses `--suite-file=` (exit 2).
- The run's block is named `file`: `runs/maker-slow_seat-file/`.

### The worked example

`examples/suites/slow_seat.cfg`: `standard` with the seat at 100 µs. Its documentation says when
to write a suite file (a different market, line-up, session length or seed block that should
carry its own fingerprint and be reported as a named set of conditions) and when `/sweep` is
enough (asking how one number moves, without a run directory).

### Tests for B, each with its planted defect

| Check | Where | Planted defect that must make it fail |
|---|---|---|
| every field of a file changes the fingerprint, version included | `test_arena` | a fingerprint that leaves out the `suitefile.*` lines |
| fixed, unknown and repeated keys and reserved names are refused | `test_arena` | a reader that ignores unknown keys |
| a placebo failing the gate is refused with its seeds | `test_arena` | an admission that skips failing seeds |
| a built-in suite's conditions carry no `suitefile.*` line | `test_arena` | — |
| `suite` refuses `--suite-file`; `suite_lab` refuses it with `--market`, `--suite`, `--block` | `test-cli` | — |
| a non-admitted file exits 3 and writes nothing | `test-lab` | — |
| a run from a file reproduces after the file is deleted | `test-lab` | a record with an edited condition must be refused |
| `/compare x suite=NAME` plans `--suite-file=suites/NAME.cfg`, and refuses `market=` with it | `test-app` | — |

## Docs

`docs/RUNNER.md` (strategy settings, a suite of your own), `docs/REPRODUCE.md` (what is
recorded and refused), `docs/APP.md`, `GLOSSARY.md` ("strategy setting", "suite file",
"admission"), `CLAUDE.md`, and exercise 11 in `docs/LEARN.md`, every command run as written.
