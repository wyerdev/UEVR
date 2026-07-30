# Workspace Agent Instructions

## Edit-tool discipline

- Before calling `apply_patch`, provide its required short `explanation` metadata along with the patch input.
- Keep patches small and local. Prefer one file or one tightly related file group per patch.
- Make sure every patch context matches the current file contents and source order. Do not combine distant or uncertain hunks into one large patch.
- If a patch is rejected, assume nothing was applied until the file state is verified. Correct the context and resubmit the smallest affected patch.
- Preserve staged and unstaged user changes. Never use a rejected patch as a reason to reset, restore, checkout, or otherwise discard worktree state.

##
- This is WWindows, dont run commands that are not available like `bash: rg: command not found`

- When reporting information to be, be extremely concise and sacrifice grammer for the sake of concision.