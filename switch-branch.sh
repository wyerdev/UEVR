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
#   ./switch-branch.sh wyerdev/afw
#
# The script only ever reverts a submodule file whose entire diff is a known
# checked-in patch. Any other dirt aborts the switch untouched.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <branch>" >&2
    exit 2
fi

target="$1"
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

sdk_dir="dependencies/submodules/UESDK"
sdk_patch="patches/UESDK-StereoStuff-renderdoc.patch"

# ---------------------------------------------------------------------------
# 1. Leave the UESDK submodule clean.
# ---------------------------------------------------------------------------
if [[ -d "$sdk_dir" ]] && [[ -n "$(git -C "$sdk_dir" status --porcelain)" ]]; then
    echo "UESDK submodule is dirty; checking whether it is only the known patch..."

    if [[ -f "$sdk_patch" ]] &&
       git -C "$sdk_dir" apply --reverse --check "$repo_root/$sdk_patch" >/dev/null 2>&1; then
        echo "  Dirt matches $sdk_patch exactly; reverting it."
        git -C "$sdk_dir" apply --reverse "$repo_root/$sdk_patch"
    fi

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
git checkout "$target"

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
