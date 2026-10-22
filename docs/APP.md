# The exchange-lab app

One terminal program, `build/release/exchange-lab`, with a prompt and slash commands, in the
spirit of Claude Code. It is how a student or a researcher meets the project. There is **no
browser whatsoever**: no web or desktop interface, no mouse, no network and no new
dependency. There is no AI model in it either. Each command does one fixed thing, and plain
text gets a pointer to `/help`.

```
$ make app
$ build/release/exchange-lab
exchange-lab · a simulated market on a real matching engine, re-run exactly · build 1a2b3c4d5e6f
8 built-in strategies, 0 installed. /help lists the commands; /learn starts the exercises.
> /add examples/strategies/imbalance.hpp
> /compare imbalance smoke
> /watch imbalance
```

## A front door, not a merger

The app **runs the existing programs as child processes**. It does not link the market into
itself. Three reasons, each enough on its own:

1. **Installing a strategy is a rebuild.** `/add` has to compile the new header into the lab
   programs. A program cannot recompile itself, but it can rebuild its children and run the
   new ones.
2. **The evidence stays out of reach, by construction.** An installed strategy can only exist
   in `sweep_lab`, `suite_lab` and `arena_lab`. A single app linking everything would put a
   stranger's header in the same binary as the programs that pin hashes.
3. **Nothing about a session changes.** Every number the app shows comes from the same
   programs as before, so no hash, fingerprint or published result can move.

Rejected: one binary that re-executes itself after `/add`. It breaks reason 2, and it would
need the session code compiled twice anyway.

## Commands

| Command | Does | Runs underneath |
|---|---|---|
| `/help [command]` | lists the commands, or explains one with examples | nothing |
| `/learn [N]` | lists the exercises in `docs/LEARN.md`, or shows exercise N | reads `docs/LEARN.md` |
| `/strategies` | the built-in strategies, and the installed ones, saying whether the lab is built with them | reads `arena/strategies/user/` and the generated manifest |
| `/add PATH` | validates and installs a strategy, then rebuilds the lab programs | `make strategy STRATEGY=PATH` |
| `/compare NAME[,NAME...] [smoke] [wide\|busy\|all]` | scores strategies in the standard seat, then prints the report | `suite_lab` |
| `/runs` | the run directories in `runs/`, newest first: complete or not, suite, block, candidates | reads `runs/` |
| `/report [RUN]` | a run's report; the newest complete run by default | reads `report.txt`, or `families.txt` |
| `/reproduce [RUN]` | re-runs a run and compares every file | `suite_lab --reproduce` |
| `/watch [NAME,...] [seed=N] [still=NS] [script=PATH] [lineup=SPEC]` | the live viewer, full screen, back to the prompt on `q` | `arena_lab` |
| `/sweep [FLAGS]` | free exploration: change one thing, many seeds | `sweep_lab`, flags passed through |
| `/controls [smoke]` | the evaluator's own controls | `controls` |
| `/book` | an order book driven by hand | `repl` |
| `/quit` (or `/exit`) | leaves | nothing |

A run is named as `/runs` lists it, or given as a path. Names given to `/watch` join the
default line-up; `lineup=` replaces it. Before running, each command asks `make` to build what
it needs, silently, so a source change is picked up and nothing is rebuilt when nothing
changed.

Exit codes from the programs underneath are explained in words: exit 3 from `suite_lab`
says "the reference market is not admissible on this build, so nothing was compared", not
just "3". After a complete `/compare`, the app says where the run was saved.

## How it works

- **Where it lives:** `app/commands.hpp` turns a typed line into a plan: answer it inside, or
  a `make` invocation and a program to run. It is pure functions, so `tests/test_app.cpp`
  checks it without starting anything. `app/main.cpp` holds everything that touches the
  terminal, the file system or a child; `app/run.hpp` starts children and `app/line.hpp`
  edits the line. It builds as `build/release/exchange-lab` with `make app`, which also
  builds every program the commands run. Its object goes to `$(REL)/obj/app/`, keeping the
  binary-and-object split the Makefile needs.
- **Launching:** `run_child(argv)` uses `fork`/`execvp` on Linux and `_spawnvp` on Windows.
  Arguments are passed as a vector, never through a shell, so a path with spaces cannot be
  split or reinterpreted; on Windows each word is quoted by the rules the child's runtime
  splits it with. A close-on-exec pipe tells "could not be started" apart from a program
  that exits 127. Children inherit the terminal, so their output appears as it always has,
  and `/watch` hands the screen to the viewer and takes it back. While a child runs the app
  ignores Ctrl-C and the child does not, so Ctrl-C stops a long sweep and leaves you at the
  prompt.
- **The prompt:** in a terminal, a line editor with history (up and down), cursor movement,
  Ctrl-A/E/U/K/C/D and tab completion of command names, strategy names, run directories and
  paths. It has its own raw mode rather than `arena/term.hpp`'s, because the viewer wants
  reads that never block and the prompt wants reads that always do, and so `term.hpp` keeps
  exactly one includer. The terminal is restored before each command runs. On Windows the
  editor reads keys with `_getch` and turns on the console's escape-sequence handling.
- **Other ways in:** when input is not a terminal, it reads one command per line and echoes
  each as `> command`, so a file of commands can be piped in; lines starting with `#` are
  comments. `exchange-lab -c '/compare maker smoke'` runs one command and exits with its
  status: the child's, or 2 for a refused command. From an MSYS2 shell, run
  `export MSYS2_ARG_CONV_EXCL='*'` first: MSYS2 rewrites an argument that starts with `/`
  into a Windows path before the app sees it, and the app says so when it gets one.
- **The banner:** build commit and dirty flag (from the generated `px_build_info.hpp`) and
  strategy counts, in colour on a terminal and plain otherwise or under `NO_COLOR`.
- **It must be run from the repository root**, like `make`. Anywhere else it refuses and says
  so. It finds the programs beside itself, so `BUILD=build-linux make app` gives an app that
  runs `build-linux/release/*` and builds with `BUILD=build-linux`.

## Tests

- **`tests/test_app.cpp`, in `make test`:** quoting and unclosed quotes; that every command
  `/help` lists has a handler and none falls through; unknown commands and plain text; the
  argument vector each command builds, on Linux and with Windows names; that every name in
  the registry's `kStrategyNames` has a line in `/strategies`; exit codes in words. It runs no
  child processes.
- **`make test-app`:** commands typed into the real binary. `/help` lists every command, an
  unknown command and plain text are refused, `/strategies` lists every built-in, `/add` of a
  non-header is refused, `/compare maker smoke` writes a complete run and `/runs` and
  `/report` find it, `/reproduce` of a copy says REPRODUCED and of a tampered copy exits 1
  through `-c`, `/controls smoke`, `/watch` with `still=`, `/sweep --help` and `/book` each
  run, `/learn` lists the eight exercises, and outside the repository root it refuses. It
  writes `runs/maker-smoke`, as that command would for anyone, and nothing else outside
  `$(BUILD)/app-test`.

The interactive editor was checked through a pseudo-terminal (completion, history, Ctrl-C at
the prompt and during a sweep, `/watch` and back on `q`); that check is not automated.

## Not built

- A browser, a web server, a desktop window or mouse support.
- An AI model or natural-language commands.
- Live trading or a network API of any kind.
- Bringing your own market data. It is planned in [`DATA_PLAN.md`](DATA_PLAN.md): the app
  will read files you already have from disk and never fetch anything.
- Paging long reports: `/report` prints the whole file and the terminal scrolls.
- Changing what any existing program does.
