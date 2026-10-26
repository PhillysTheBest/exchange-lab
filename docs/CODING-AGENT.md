# Using a coding agent with exchange-lab

An AI coding agent, such as Claude Code or any other that can read files and run commands,
is a reasonable way to write a strategy for exchange-lab and to read what the report says
about it. This page says how to use one well, what to watch it for, and why no model may
run **inside** a session, however it is used outside one.

The app itself has no model in it, by decision ([`APP.md`](APP.md)). Everything below happens
in your editor and your shell, beside the app, never in it.

## What an agent is good for here

- **Writing a strategy header.** The contract is small and written down: `arena/strategy.hpp`
  lists the five members, and `examples/strategies/imbalance.hpp` shows the convention
  (`<name>.hpp` defines struct `<Name>` in `px::arena::strategies`). `make strategy` checks the
  name and builds it, so a mistake comes back as a message, not as a wrong number.
- **Running the standard path.** `make strategy`, `make compare ... BLOCK=smoke`, then the
  standard block, then `holdout` once ([`LEARN.md`](LEARN.md), exercise 10).
- **Reading a report.** `report.txt` is plain text and [`REPORT.md`](REPORT.md) says how to read
  it. An agent can walk through it in the documented order: health, the paired difference, the
  metrics, the attribution, then the count of tests and the Holm line.
- **Analysis outside the project.** The run's CSVs (`sessions.csv`, `participants.csv`,
  `comparison.csv`, `evaluation.csv`, `attribution.csv`) can be read by a script in any
  language. That script lives outside the repository and never feeds back into a session.

## Brief it with the rules

Give the agent these files and this list before it writes anything:

- `arena/strategy.hpp`, `examples/strategies/imbalance.hpp`, [`RUNNER.md`](RUNNER.md) and
  [`REPORT.md`](REPORT.md). For work on the project itself rather than a strategy, it should
  read [`CLAUDE.md`](../CLAUDE.md) first.
- **No floating point** in the strategy: no `float`, no `double`, no `<cmath>`. Prices are
  integers in 1/10,000ths of a currency unit and a tick is `v.tick`.
- **Include `arena/strategy.hpp` and nothing else from the project.** The engine, the session
  and the clock are out of reach by construction.
- **It is a strategy, never a bot**, and its `reaction_ns` is an input the experimenter
  chooses, never a latency. The suite's seat runs at 10,000 ns whatever it declares.
- **Tune on `standard` only. Run `holdout` once, when the strategy is final, and report both.**
- **Never edit `results/` or `tests/canonical.txt`,** and never re-pin a fingerprint.

## What to watch it for

Agents are good at writing code that compiles and less good at the statistics. The usual
failures here are the ones this project was built to catch:

- **Re-running until it separates.** Trying ratios, thresholds or sizes until one interval
  excludes zero is a search over the standard block. Count the tries and report them, and let
  the holdout block, run once, decide.
- **Reading a smoke run as a result.** Four seeds show that a strategy runs; the report says
  they cannot separate anything. A "separated" on smoke is not a finding.
- **Leading with the value.** The report's first table is market health. A number from a
  market whose book has come loose from the fundamental does not count, whatever its interval.
- **Calling an interval that straddles zero a small positive effect.** It is not established.
  "Underpowered" and "nothing there" are different claims; the `resolvable` column says which
  one the seed count can support.
- **Quoting numbers that are not in the files.** Check every number an agent quotes against
  `report.txt`, and check the run itself with `/reproduce`. A number nobody can print again has
  no evidence behind it.
- **Treating `adverse` reading zero as "no adverse selection".** It is zero by construction in
  this market ([`ARENA.md`](ARENA.md)); a stale quote's loss lands in `spread`.

## Why no model may run inside a session

A strategy may be *written* by a model. It may not *call* one.

- **It breaks determinism.** A session is a contract: the same seed and the same strategies
  give the same event stream, bit for bit, on every machine. A model's answer is sampled,
  depends on a version that changes under you, and is not reproducible from a seed. One call
  would make `/reproduce` fail and every fingerprint meaningless.
- **It needs the network,** and nothing in exchange-lab has one. Data is read from disk; no
  program opens a socket.
- **It brings the wall clock in.** A session invents its nanoseconds. A call that takes real
  milliseconds either stops the simulated clock, which hides its cost, or leaks real time into
  simulated time, which makes the run depend on the machine.
- **It cannot be checked.** The evaluator's controls, the replay check and the cross-platform
  record all assume a strategy is a pure function of what it has seen. A remote model is not.

What is allowed is a model that has been **frozen**: trained elsewhere, its weights exported as
integers into a C++ header, and run like any other strategy. It is then a pure, integer function
of the market view, and it replays like one. [`examples/strategies/frozen.hpp`](../examples/strategies/frozen.hpp)
is a worked example, and [`examples/frozen/README.md`](../examples/frozen/README.md) says how it
was made.

## A brief to start from

```
Write a strategy for exchange-lab as examples/strategies/<name>.hpp, following
arena/strategy.hpp and examples/strategies/imbalance.hpp exactly: struct <Name> in
px::arena::strategies, the five members, integer arithmetic only, including only
arena/strategy.hpp. Idea: <one sentence>. Then run
  make strategy STRATEGY=<path>
  make compare CANDIDATES=zi,<name> BLOCK=smoke
  make compare CANDIDATES=zi,<name>
and report, in this order: market health, the paired row <name> vs zi with its interval,
the resolvable size, and the Holm line. Do not tune on the holdout block, do not re-run to
get a separation, and quote only numbers that appear in report.txt.
```
