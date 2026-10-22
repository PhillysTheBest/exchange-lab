# The exchange-lab terminal app: plan

Decided 2026-10-22: exchange-lab gets one terminal app, in the spirit of Claude Code. You
open it, get a prompt, and drive everything with slash commands. There is **no browser
whatsoever**, and no web or desktop interface. This file is the plan, written before
building as `CLAUDE.md` asks. When the app is built, it becomes `docs/APP.md`, the design
document.

## What it is, and what it is not

```
$ build/release/exchange-lab
exchange-lab · a market you can re-run · build 54d7288 · 8 built-in strategies, 1 installed
type /help for commands, /learn to start the exercises

> /add ~/strategies/joiner.hpp
installed joiner, lab rebuilt
> /compare joiner smoke
...the report...
> /watch joiner
...the live viewer, full screen, until q...
>
```

- **One program, one prompt.** A student or researcher meets `exchange-lab` and nothing
  else. The seven programs and the `make` targets stay underneath, for scripts and for the
  published evidence, but nobody has to learn them to use the project.
- **Commands, not a chat.** It looks like Claude Code, but there is no AI model inside. Each
  slash command does one fixed thing. Typing plain text gets a pointer to `/help`.
- **No browser, no mouse, no network, no new dependencies.** The project's existing rules
  still apply: one hand-written Makefile, g++ and nothing else.

## The decision that shapes it: a front door, not a merger

The app **runs the existing programs as child processes**. It does not link the market
into itself. Three reasons, each enough on its own:

1. **Installing a strategy is a rebuild.** `/add` has to compile the new header into the
   lab programs. A program cannot recompile itself, but it can rebuild its children and
   run the new ones.
2. **The evidence stays out of reach, by construction.** Today an installed strategy can
   only exist in `sweep_lab`, `suite_lab` and `arena_lab`. A single app linking everything
   would put a stranger's header in the same binary as the programs that pin hashes.
3. **Nothing about a session changes.** Every number the app shows comes from the same
   programs as before, so no hash, fingerprint or published result can move.

Rejected: one monolithic binary that re-executes itself after `/add`. It breaks reason 2,
and it would need the session code compiled twice anyway.

## Commands

| Command | Does | Runs underneath |
|---|---|---|
| `/help [command]` | lists commands, or explains one with examples | nothing |
| `/strategies` | built-in strategies, and the installed ones with their checksums | reads `kStrategyNames` and `arena/strategies/user/` |
| `/add PATH` | validates and installs a strategy, rebuilds the lab programs | `make strategy STRATEGY=PATH` |
| `/compare A[,B...] [smoke] [wide\|busy\|all]` | scores strategies in the standard seat, then shows the report | `suite_lab` |
| `/runs` | lists run directories: complete or not, suite, block, candidates | reads `runs/` |
| `/report [RUN]` | shows a run's report, a page at a time; the latest run by default | reads `report.txt` |
| `/reproduce [RUN]` | re-runs a run and compares every file | `suite_lab --reproduce` |
| `/watch [LINE-UP] [seed=N] [script=PATH]` | the live viewer, full screen, back to the prompt on `q` | `arena_lab` |
| `/sweep ARGS` | free exploration: change one thing, many seeds | `sweep_lab` |
| `/controls [smoke]` | the evaluator's own controls | `controls` |
| `/learn [N]` | shows exercise N of `docs/LEARN.md`, using app commands | reads `docs/LEARN.md` |
| `/quit` | leaves | nothing |

Exit codes from the programs underneath are explained in words. For example, exit 3 from
`suite_lab` means "the reference market is not admissible on this build", not just "3".

## How it works

- **Where it lives:** `app/` (`main.cpp`, `commands.hpp`, `line.hpp`). It builds as
  `build/release/exchange-lab` with `make app`, which also builds the programs it drives.
  Objects go to `$(REL)/obj/app/`, keeping the binary-and-object split the Makefile needs.
- **Launching:** a small `run(argv)` that uses `fork`/`execvp` on Linux and `_spawnvp` on
  Windows (UCRT's `<process.h>`). Arguments are passed as a vector, never through a shell,
  so a path with spaces cannot be split or reinterpreted. Children inherit the terminal, so
  their output appears as it does today, and `/watch` hands the screen to the viewer and
  takes it back.
- **The prompt:** in a terminal, a line editor on `arena/term.hpp`'s raw mode, with history
  (up and down), cursor movement, and tab completion of command names, strategy names and
  run directories. When input is not a terminal, it reads one command per line, so a file
  of commands can be piped in. `exchange-lab -c '/compare maker smoke'` runs one command
  and exits with its status.
- **The banner:** build commit and dirty flag (from the generated `px_build_info.hpp`),
  strategy counts, and the next exercise. It is in colour when the terminal supports it,
  and plain when piped.
- **It must be run from the repository root**, like `make`. Anywhere else, it refuses with
  a message that says so.

`arena/term.hpp` then has two includers, `arena/arena.cpp` and `app/main.cpp`, in two
different programs. `CLAUDE.md`'s rule becomes "one translation unit per program".

## Tests

- **`tests/test_app.cpp` (in `make test`):** the command parser as a pure function from a
  line to an argument vector. It checks quoting, unknown commands, missing arguments, and
  that every command in `/help` has a handler and the other way round, the way `test_arena`
  holds `kStrategyNames` to the registry. It runs no child processes.
- **`make test-app`:** commands piped into the real binary.
  - `/help` lists every command.
  - An unknown command is refused.
  - `/strategies` matches `kStrategyNames`.
  - `/compare maker smoke` writes a complete run, and `/reproduce` of it says REPRODUCED.
  - `/add` of a non-header is refused.
  - `-c` returns the child's exit code.
- **Nothing existing changes:** every pinned hash and fingerprint, `test-cli` and `test-lab`
  as they are, engine diff empty, memmove count 0.

## Stages

1. **The skeleton.**
   - The piped-input prompt, the command table and `run(argv)`.
   - `/help`, `/strategies`, `/compare`, `/runs`, `/report` and `/quit`.
   - `make app`, `test_app` and `test-app`.
2. **The rest of the commands:** `/add`, `/watch`, `/reproduce`, `/controls` and `/sweep`, with
   exit codes in words.
3. **The interactive prompt:** the line editor with history and tab completion, and the
   colour banner.
4. **Docs and the student path.**
   - `/learn`.
   - README's "Start here" opens with `make app`.
   - `docs/LEARN.md` gives the app command beside each raw one, and every command in it is
     re-run.
   - `GLOSSARY.md` and `CLAUDE.md` updated.
   - This plan turned into `docs/APP.md`.

Each stage is a commit with `make test test-cli test-lab test-app` green. Every stage is
built and checked on Linux. The Windows build is checked by you, as for PR #2.

## Not in this plan

- A browser, a web server, a desktop window or mouse support.
- An AI model or natural-language commands.
- Live trading, a network API or market data. The "calibrate to real data" question is a
  separate decision, still open.
- Changing what any existing program does.
