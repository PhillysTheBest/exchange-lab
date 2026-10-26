## What changed, and the evidence

<!-- What the change does and how you know it works. CONTRIBUTING.md lists the rules. -->

## Checks

- [ ] **Does `make canonical` still match?** If not, paste the diff and say why each moved
      line was intended. `tests/canonical.txt` is never re-recorded to make CI pass.
- [ ] **Did any fingerprint or hash move?** Name each one: engine determinism, session, sweep,
      suite fingerprint (`standard`, `wide`, `busy`, `holdout`). A moved suite fingerprint
      needs a suite version bump, never a silent re-pin.
- [ ] **Which test or control was added?** A new control needs a planted defect in
      `tests/test_arena.cpp` that makes it fail.
- [ ] `make test`, `test-cli`, `test-lab`, `test-app` and `test-data` pass.
- [ ] No floating point inside a session; no new dependency; no network.
- [ ] New terms are in `GLOSSARY.md`; new commands are documented and run as written.
- [ ] `CHANGELOG.md` says what this changes in the engine, strategies, scenario
      definitions, report calculations or output formats.
