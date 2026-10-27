# Still to do before going public

Left for Philbert on purpose (2026-10-26). Everything else on the gap-review task list is
built on the `release-1.0` branch: every check passes on Linux (GCC 13.3, with ASan and UBSan)
and on Windows (MSYS2 GCC 13.2), and `make canonical` gives the same 26 lines on both. It
merged in #7 after CI passed on all five targets, macOS and ARM64 included.

- [ ] **Code of conduct.** Add `CODE_OF_CONDUCT.md` (Contributor Covenant). It needs a contact
      for reports: an address, or GitHub's private reporting. Then link it from
      `CONTRIBUTING.md`.
- [ ] **ORCID.** If you have one, add `orcid: "https://orcid.org/XXXX-XXXX-XXXX-XXXX"` under
      the author in `CITATION.cff`.

Only you can do these, because they happen in GitHub's settings or on another site:

- [ ] **Branch protection on `main`**: pull requests only, CI must pass, owner approval required.
- [x] **Push the branch and the tag.** Done 2026-10-27: merged in #7, and `v1.0.0` moved onto
      the merge commit `4cb384e` and pushed.
- [ ] **A DOI.** Connect the repository to Zenodo, publish a GitHub release for `v1.0.0`, and
      add the DOI Zenodo gives to `CITATION.cff` (`doi:`) and the README.

Delete this file when the list is empty.
