---
name: Bug report
about: Something failed to build, failed to run, or gave a wrong answer
labels: bug
---

**What happened, and what you expected instead**


**Platform**

- OS and version:
- Compiler and version (the first line of `g++ --version` or `clang++ --version`):
- Standard library, if not the compiler's default (for example libc++):

**Build command**

The exact line, for example `make app`, or `mingw32-make test` in MSYS2 UCRT64.

```

```

**Command that reproduces it**

The exact app command or program line, for example `/compare imbalance smoke` or
`build/release/sweep --seeds=4`.

```

```

**Run fingerprint**

For a `/compare` run, paste the `fingerprint=` line from the run's `outcome.cfg` and the
`build.` lines from its `suite.cfg`. For a sweep, paste the session hash it prints.

```

```

**`make canonical`**

Does it still report every output identical to `tests/canonical.txt`? If not, paste the
first line that differs.

```

```
