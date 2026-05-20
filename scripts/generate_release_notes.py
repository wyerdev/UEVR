#!/usr/bin/env python3
"""Generate RELEASE_NOTES.md for CI or local testing.

Usage:
    python scripts/generate_release_notes.py \
        --nightly-tag "nightly-00397-..." \
        --zip-name "uevr-patched-00397-42" \
        --plugins-zip-name "uevr-plugins-00397-42" \
        --repo-slug "wyerdev/UnrealVRMod" \
        --commit-sha "abc1234" \
        --out-file "./RELEASE_NOTES.md"
"""

import argparse
import re
import subprocess
import sys

# Conventional-commits-style match: fix / feat, optional (scope), optional !, then ':'
COMMIT_RE = re.compile(r"^(fix|feat)(\([^)]+\))?!?:", re.IGNORECASE)


def git(*args: str) -> str:
    """Run a git command and return stripped stdout (empty string on failure)."""
    try:
        result = subprocess.run(
            ["git", *args],
            capture_output=True, text=True, check=False
        )
        if result.returncode != 0:
            return ""
        return result.stdout.strip()
    except FileNotFoundError:
        return ""


def _resolve_upstream_ref() -> str:
    """Return a ref representing upstream/master, or empty if not available.

    Tries common remote names so this works in CI (where workflow adds an
    'upstream' remote) and locally (where a contributor may have configured
    the upstream under a different name).
    """
    candidates = ["upstream/master", "praydog/master", "upstream/main"]
    for ref in candidates:
        if git("rev-parse", "--verify", "--quiet", ref):
            return ref
    return ""


def get_changelog() -> str:
    """Collect fix/feat commits unique to this fork (not in upstream master).

    Uses `git log upstream..HEAD` so every contributor to the fork is
    included, regardless of author name. Falls back to all reachable commits
    if no upstream ref is found (e.g. local dev clone without the remote).
    """
    upstream = _resolve_upstream_ref()
    log_range = f"{upstream}..HEAD" if upstream else "HEAD"

    raw = git(
        "log", log_range,
        "--no-merges",
        "--pretty=format:%s|%h|%as",
    )

    if not raw:
        return "- No fix/feat commits in this build"

    entries = []
    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("|", 2)
        if len(parts) != 3:
            continue
        subject, short_sha, date = parts
        if not COMMIT_RE.match(subject):
            continue
        entries.append(f"- {subject} ({short_sha}) [{date}]")

    if not entries:
        return "- No fix/feat commits in this build"

    return "\n".join(entries)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate release notes")
    parser.add_argument("--nightly-tag", required=True)
    parser.add_argument("--zip-name", required=True)
    parser.add_argument("--plugins-zip-name", required=True)
    parser.add_argument("--repo-slug", required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--out-file", required=True)
    args = parser.parse_args()

    nightly_url = f"https://github.com/praydog/UEVR-nightly/releases/tag/{args.nightly_tag}"
    changelog = get_changelog()

    notes = f"""## How to install

1. Download the **base UEVR nightly** this build is patched against:
   **[{args.nightly_tag}]({nightly_url})**
2. Extract the nightly zip to a folder.
3. Download **{args.zip_name}.zip** from this release.
4. Extract and **overwrite/replace** the files from step 2 with the files from this release.
5. Download **{args.plugins_zip_name}.zip** and run ``install-plugins.bat`` to install shaders and presets.
   Or install manually -- see [INSTALL.md](https://github.com/{args.repo_slug}/blob/{args.commit_sha}/docs/INSTALL.md).
6. Run UEVR as normal.

> **Note:** The post-processing shaders require this patched fork. They will **not** load on stock UEVR nightly.

## Changes
{changelog}
"""

    with open(args.out_file, "w", encoding="utf-8") as f:
        f.write(notes)

    count = changelog.count("\n- ") + (1 if changelog.startswith("- ") else 0)
    print(f"Release notes written to {args.out_file} ({count} changelog entries)")


if __name__ == "__main__":
    main()
