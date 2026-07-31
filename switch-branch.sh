#!/usr/bin/env bash
#
# Switch between UEVR integration branches without leaking one branch's UESDK
# working-tree patch into another branch's build.
#
# Background
# ----------
# The AFW branch (PureDark/UEVR:AFW) runs cmake/ApplyUESDKPatch.cmake at
# configure time. That applies patches/UESDK-StereoStuff-renderdoc.patch to
# dependencies/submodules/UESDK/src/sdk/StereoStuff.cpp -- a working-tree edit
# inside the submodule, because UESDK is private and cannot be forked publicly.
# master has no such step.
#
# StereoStuff.cpp is byte-identical between master's UESDK revision and AFW's,
# so `git submodule update` will NOT complain when moving between them. It will
# silently carry AFW's patch into the other branch's build. Hence this script.
#
# Usage:
#   ./switch-branch.sh master
#   ./switch-branch.sh wyerdev/afw     (or the short alias: afw)
#
# The script only ever reverts a submodule file whose entire diff is a known
# checked-in patch. Any other dirt aborts the switch untouched.
#
# Only the integration branches listed in BRANCHES below may be checked out.
# [2026-07-30] This guard exists because the script used to pass its argument
# straight to `git checkout`. `./switch-branch.sh afw` then landed on a stray
# local branch named `afw` that pointed at upstream puredark/AFW, silently and
# with exit code 0. Worse, had that branch not existed, git's DWIM would have
# created a new local branch tracking puredark/AFW instead -- same wrong
# result. A bare upstream name is never one of our integration branches.

set -euo pipefail

# Canonical integration branches, plus the short aliases we actually type.
# Add a target's branch here when its port begins; nothing else is accepted.
declare -A BRANCHES=(
    [master]="master"
    [afw]="wyerdev/afw"
    [wyerdev/afw]="wyerdev/afw"
    [joey]="wyerdev/afw-joeyhodge"
    [wyerdev/afw-joeyhodge]="wyerdev/afw-joeyhodge"
)

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <branch>" >&2
    printf 'known: %s\n' "${!BRANCHES[*]}" >&2
    exit 2
fi

if [[ -z "${BRANCHES[$1]+set}" ]]; then
    echo "REFUSING: '$1' is not one of our integration branches." >&2
    printf 'known: %s\n' "${!BRANCHES[*]}" >&2
    exit 2
fi

target="${BRANCHES[$1]}"
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

# The branch must already exist locally. This script switches; it never creates.
if ! git show-ref --verify --quiet "refs/heads/$target"; then
    echo "ABORT: local branch '$target' does not exist. Create it yourself." >&2
    exit 1
fi

sdk_dir="dependencies/submodules/UESDK"
sdk_patch="patches/UESDK-StereoStuff-renderdoc.patch"

# Revert the submodule dirt if -- and only if -- it is byte-exactly one of our
# checked-in patches.
#
# [2026-07-31] The patch text must be looked up across branches, not just in the
# working tree. patches/ exists only on the branches that apply it; on master
# there is no such directory. So when AFW's patch had leaked into the submodule
# and we were standing on master, this check could not find the patch file, fell
# through, and hard-aborted on precisely the leak the script exists to clean up.
# We therefore also read the patch out of every known integration branch.
revert_known_patch() {
    local tmp src
    tmp="$(mktemp)"
    for src in worktree "${BRANCHES[@]}"; do
        if [[ "$src" == worktree ]]; then
            [[ -f "$sdk_patch" ]] || continue
            cp "$sdk_patch" "$tmp"
        else
            git show "$src:$sdk_patch" >"$tmp" 2>/dev/null || continue
        fi
        if git -C "$sdk_dir" apply --reverse --check "$tmp" >/dev/null 2>&1; then
            git -C "$sdk_dir" apply --reverse "$tmp"
            echo "  Dirt matches $sdk_patch (source: $src) exactly; reverted it."
            rm -f "$tmp"
            return 0
        fi
    done
    rm -f "$tmp"
    return 1
}

# ---------------------------------------------------------------------------
# 1. Leave the UESDK submodule clean.
# ---------------------------------------------------------------------------
if [[ -d "$sdk_dir" ]] && [[ -n "$(git -C "$sdk_dir" status --porcelain)" ]]; then
    echo "UESDK submodule is dirty; checking whether it is only the known patch..."

    revert_known_patch || true

    # Re-check: anything left is not ours to discard.
    remaining="$(git -C "$sdk_dir" status --porcelain)"
    if [[ -n "$remaining" ]]; then
        echo "ABORT: UESDK submodule has changes this script will not discard:" >&2
        echo "$remaining" >&2
        echo "Resolve them yourself, then rerun." >&2
        exit 1
    fi
    echo "  UESDK submodule is clean."
fi

# ---------------------------------------------------------------------------
# 2. Switch the parent repo. git refuses if it would clobber local work.
# ---------------------------------------------------------------------------
echo "Switching to $target ..."
# --no-guess: never invent a branch from a remote-tracking ref.
git checkout --no-guess "$target"

# ---------------------------------------------------------------------------
# 3. Move submodules to the revisions this branch pins.
# ---------------------------------------------------------------------------
echo "Updating submodules ..."
git submodule update --init --recursive

# ---------------------------------------------------------------------------
# 4. Report. The next configure reapplies the branch's own patch, if it has one.
# ---------------------------------------------------------------------------
echo
echo "On branch: $(git rev-parse --abbrev-ref HEAD)  ($(git rev-parse --short HEAD))"
echo "UESDK:     $(git -C "$sdk_dir" rev-parse --short HEAD)"
if [[ -f "$sdk_patch" ]]; then
    echo "Note:      this branch patches UESDK at configure time ($sdk_patch)."
fi
echo "Run build.bat to configure and build into this branch's output directory."
