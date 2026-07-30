# Merging Upstream (praydog/UEVR) into this Fork

## Prerequisites

- Remote `upstream` points to `https://github.com/praydog/UEVR.git`
- If not added: `git remote add upstream https://github.com/praydog/UEVR.git`

## Workflow

```bash
# 1. Stash uncommitted changes (if any)
git stash push -m "WIP before upstream merge"

# 2. Fetch upstream
git fetch upstream

# 3. (Optional) Preview incoming commits
git log --oneline HEAD..upstream/master

# 4. Merge — use --no-edit to skip the editor prompt
git merge upstream/master --no-edit

# 5. Update submodules (merge only updates the pointer, not the checkout)
git submodule update

# 6. Verify before push
git status                  # should be "working tree clean"
git log --oneline origin/master..HEAD   # review what will be pushed
git submodule status        # no + or - prefix = all good

# 7. Push
git push

# 8. Restore stashed work
git stash pop
```

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
