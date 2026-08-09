# Merging PureDark's Joey-Merged Branch into this Fork

| Our branch | Upstream merged | Base download users need |
|---|---|---|
| `master` | `upstream/master` (praydog/UEVR) | a [praydog UEVR-nightly](https://github.com/praydog/UEVR-nightly/releases) tag |
| `wyerdev/afw` | `puredark/AFW` (PureDark/UEVR) | a [PureDark/UEVR](https://github.com/PureDark/UEVR/releases) release tag |
| `wyerdev/afw-joeyhodge` | `puredark/Joey-Merged` (PureDark/UEVR) | a [PureDark/UEVR](https://github.com/PureDark/UEVR/releases) release tag |

## Prerequisites

- Remote `puredark` points to `https://github.com/PureDark/UEVR.git`
- If not added: `git remote add puredark https://github.com/PureDark/UEVR.git`

## Workflow

```bash
# 1. Stash uncommitted changes (if any)
git stash push -m "WIP before upstream merge"

# 2. Fetch PureDark's upstream
git fetch puredark

# 3. (Optional) Preview incoming commits
git log --oneline HEAD..puredark/Joey-Merged

# 4. Merge — use --no-edit to skip the editor prompt
git merge puredark/Joey-Merged --no-edit

# 5. Update submodules (merge only updates the pointer, not the checkout)
git submodule update

# 6. Verify before push
git status                  # should be "working tree clean"
git log --oneline origin/wyerdev/afw-joeyhodge..HEAD   # review what will be pushed
git submodule status        # no + or - prefix = all good

# 7. Push
git push

# 8. Restore stashed work
git stash pop
```

## Step 6: BASE_NIGHTLY — [2026-07-30]

**Every upstream merge must update `BASE_NIGHTLY` in the repo root.** It holds
one line: the tag of the upstream build this branch is now patched against.

It feeds three things in `.github/workflows/release.yml` and
`scripts/generate_release_notes.py`:

- the **base-download link** at the top of the release body's install steps,
  built as `https://github.com/<base_repo>/releases/tag/<BASE_NIGHTLY>`
- the **build number** in the release tag, the release title, and the release
  zip name — parsed out of the tag by the "Read base nightly tag" step
- the release's claim about which upstream build it is compatible with

**Nothing fails if you forget.** The workflow still goes green and the release
still publishes; it just tells users to download the *previous* upstream build
and overwrite it with a shader plugins zip built against a newer one.
That is a silent, user-visible mismatch, which is why this step is called out
separately.

Rules:

- The tag must exist in that branch's base repo (see the table above). Open the
  URL and confirm before pushing — do not assume the tag naming held.
- The tag must correspond to the upstream commit the merge actually brought in,
  not merely the newest release on that page.
- The build-number parser in `release.yml` understands `nightly-(\d+)-` and
  `beta\.(\d+)`. A tag matching neither silently yields `00000-<run>` for the
  tag, title, and shader plugins zip name. If a branch's upstream adopts a new
  tag form, extend that step in the same change.
## Notes

- The merge creates a merge commit (not fast-forward) because the fork has
  its own commits. Git opens an editor for the commit message — `--no-edit`
  accepts the default.
- After merge, VS Code "Changes" panel will be empty because the merge is
  already committed. This is normal.
- If `git stash pop` has conflicts (stashed changes overlap upstream), git
  will tell you and the stash is preserved — resolve manually.
- Submodule pointer changes (like UESDK) require `git submodule update` to
  actually check out the new code locally.
