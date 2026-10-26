<!-- What changed, and what the evidence is. CONTRIBUTING.md has the rules. -->


## Checks

- **Does `make canonical` still match `tests/canonical.txt`?** If a line moved, which one, and
  why was that intended?
- **Did any fingerprint or pinned hash move?** These include the engine's
  `77a560d8ffb4fa43`, the session's `2853d2127a3486c5` and the suite fingerprints
  (`d461c30ab2446acf` for `standard`, plus one per family). A suite fingerprint that moves
  needs a suite version bump: which one?
- **Which test or control did you add?** For a new control, name its planted defect.

- [ ] `make test test-cli test-lab test-app test-data canonical` passes
