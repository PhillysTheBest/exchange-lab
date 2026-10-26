# Still to do before going public

Left for Philbert on purpose (2026-10-26). Everything else on the gap-review task list is
built on the `release-1.0` branch.

- [ ] **Code of conduct.** Add `CODE_OF_CONDUCT.md` (Contributor Covenant). It needs a contact
      for reports: an address, or GitHub's private reporting. Then link it from
      `CONTRIBUTING.md`.
- [ ] **ORCID.** If you have one, add `orcid: "https://orcid.org/XXXX-XXXX-XXXX-XXXX"` under
      the author in `CITATION.cff`.

Only you can do these, because they happen in GitHub's settings or on another site:

- [ ] **Branch protection on `main`**: pull requests only, CI must pass, owner approval required.
- [ ] **Push the branch and the tag.** Nothing has been pushed. `git push -u origin release-1.0`,
      open the pull request, and after it merges move the tag onto the merge commit:
      `git tag -f -a v1.0.0 <merge-commit>` and `git push origin v1.0.0`. The tag is
      currently local and on the branch's last commit.
- [ ] **A DOI.** Connect the repository to Zenodo, publish a GitHub release for `v1.0.0`, and
      add the DOI Zenodo gives to `CITATION.cff` (`doi:`) and the README.

Delete this file when the list is empty.
