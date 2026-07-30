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
from pathlib import Path

# Match commits starting with fix or feat (case-insensitive)
COMMIT_RE = re.compile(r"^(fix|feat)", re.IGNORECASE)

# How much history a release body is allowed to show. Both limits apply; see
# get_changelog(). CHANGELOG_WINDOW is used both in prose and, with " ago"
# appended, as git's --since argument.
CHANGELOG_WINDOW = "3 months"
CHANGELOG_MAX_ENTRIES = 7

# Build lines, keyed by variant ID (must match UEVR_VARIANT_ID in
# include/uevr/Variant.hpp). Deferred lines are deliberately absent; see
# docs/active/joeyhodge-support-plan.md.
#
# "published" means the line has release CI and has actually shipped at least
# one release. Unpublished lines are still listed, but never linked, so we
# never point people at an empty search. Flip it to True in the same change
# that adds the line's release workflow.
BUILD_LINES = {
    "reshade": {
        "name": "UEVR ReShade",
        "upstream": "[praydog/UEVR](https://github.com/praydog/UEVR) `master`",
        "upstream_repo": "praydog/UEVR",
        "upstream_branch": "master",
        "tag_query": "uevr-reshade-release",
        "published": True,
        # Where BASE_NIGHTLY's tag lives, and what to call it in the install
        # steps. Each line is patched against a different upstream build.
        "base_repo": "praydog/UEVR-nightly",
        "base_label": "base UEVR nightly",
    },
    "reshade-afw": {
        "name": "UEVR ReShade AFW",
        "upstream": "[PureDark/UEVR](https://github.com/PureDark/UEVR) `AFW`"
                    " \u2014 asynchronous frame warp",
        "upstream_repo": "PureDark/UEVR",
        "upstream_branch": "AFW",
        "tag_query": "uevr-reshade-afw-release",
        "published": True,
        # AFW is not a praydog nightly: the base is PureDark's own release,
        # which is also the only source of the proprietary PDAFWPlugin.dll
        # this build line delay-loads.
        "base_repo": "PureDark/UEVR",
        "base_label": "base UEVR AFW release",
    },
}

# Where the credits come from. The release body copies this file's Credits
# section instead of repeating it, so there is only one place to edit when
# contributors or shader ports change.
CREDITS_DOC = Path(__file__).resolve().parent.parent / "docs" / "reference" / "INSTALL.md"
CREDITS_HEADING = "## Credits"


def credits(repo_slug: str, commit_sha: str) -> str:
    """Return INSTALL.md's Credits section, ready to paste into a release body.

    The text is copied into the release rather than linked, so the credits are
    visible on the release page itself. It is copied from INSTALL.md every
    time, so that file stays the only place the credits are written down.
    """
    text = CREDITS_DOC.read_text(encoding="utf-8")
    start = text.find(CREDITS_HEADING)
    if start == -1:
        raise SystemExit(
            f"{CREDITS_DOC} has no '{CREDITS_HEADING}' section to copy."
        )

    end = text.find("\n## ", start + len(CREDITS_HEADING))
    section = text[start:] if end == -1 else text[start:end]

    # Links inside INSTALL.md point at paths relative to docs/reference/, which
    # means nothing on a release page. Turn them into full github.com links.
    blob = f"https://github.com/{repo_slug}/blob/{commit_sha}"
    section = re.sub(
        r"\]\(\.\./\.\./([^)]+)\)", rf"]({blob}/\1)", section
    )
    return section.rstrip() + "\n"


def provenance(variant: str, upstream_commit: str, commit_sha: str,
               repo_slug: str) -> str:
    """Table showing exactly which commits this build was made from."""
    line = BUILD_LINES[variant]
    repo = line["upstream_repo"]
    branch = line["upstream_branch"]
    upstream_cell = (
        f"[`{upstream_commit[:10]}`](https://github.com/{repo}/commit/{upstream_commit})"
        if upstream_commit else "_not recorded_"
    )
    return f"""## What this build was made from

| | |
| :--- | :--- |
| Upstream UEVR | [{repo}](https://github.com/{repo}) `{branch}` |
| Upstream commit used | {upstream_cell} |
| Fork commit built | [`{commit_sha[:10]}`](https://github.com/{repo_slug}/commit/{commit_sha}) |
"""


def build_line_header(variant: str, repo_slug: str, commit_sha: str) -> str:
    """Banner saying which build line this release is, with links to the others.

    Every build line shares one releases page and the releases are mixed
    together by date, so it is easy to land on the wrong one. This block goes
    at the top of every release body so people can find their way back.
    """
    line = BUILD_LINES[variant]
    install_url = (
        f"https://github.com/{repo_slug}/blob/{commit_sha}"
        "/docs/reference/INSTALL.md"
    )

    others = []
    for other_id, other in BUILD_LINES.items():
        is_this_one = other_id == variant
        # A line that has not shipped yet has no releases to link to. The line
        # being released now obviously has, whatever the table says.
        if other["published"] or is_this_one:
            url = (f"https://github.com/{repo_slug}/releases"
                   f"?q={other['tag_query']}")
            entry = f"- [{other['name']}]({url})"
        else:
            entry = f"- {other['name']} — not released yet"
        if is_this_one:
            entry += " ← **this build line**"
        others.append(entry)
    other_lines = "\n".join(others)

    return f"""# {line['name']}

This release patches **{line['upstream']}**. The core zip, the shaders zip, and
your installed shader folder must all come from this same build line — you
cannot mix them.

**On the wrong page?** Pick your build line:

{other_lines}

Not sure which one you want, or how they coexist?
See [How to Install → Choose Your Build Line]({install_url}#choose-your-build-line).
"""


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


def _resolve_upstream_ref(explicit: str = "") -> str:
    """Return a ref for this branch's upstream, or empty if none is available.

    `explicit` is the commit the workflow already resolved from *this* branch's
    own upstream branch (`--upstream-commit`, i.e. `UPSTREAM_SHA`). It is the
    only reliable source: the name-based candidates below all assume the
    upstream is praydog `master`, which is false on every integration branch.
    On `wyerdev/afw` the workflow adds an `upstream` remote pointing at
    PureDark and fetches only `AFW`, so none of the candidate *names* exist in
    CI at all.

    The candidates remain as a local-dev fallback, where a contributor on
    `master` may not pass `--upstream-commit`.
    """
    if explicit and git("rev-parse", "--verify", "--quiet", f"{explicit}^{{commit}}"):
        return explicit

    candidates = ["upstream/master", "praydog/master", "upstream/main"]
    for ref in candidates:
        if git("rev-parse", "--verify", "--quiet", ref):
            return ref
    return ""


def get_changelog(repo_slug: str, commit_sha: str, upstream_commit: str = "") -> str:
    """Collect recent fix/feat commits unique to this fork (not in its upstream).

    Uses `git log <upstream>..HEAD` so every contributor to the fork is
    included, regardless of author name. `upstream_commit` is what the workflow
    resolved from this branch's own upstream branch; without it the range
    degrades to plain `HEAD`, i.e. **the entire history of UEVR**, upstream
    commits and all. That is not hypothetical — it is what AFW release
    beta5-44 shipped with, listing praydog commits from 2022.

    Even with the right base, the range is the fork's whole divergence from
    upstream, which on the baseline line reaches back to 2023. A release body
    is meant to say what is new in this build, not to restate the project's
    history, so the list is capped twice: by age (`CHANGELOG_WINDOW`) and then
    by count (`CHANGELOG_MAX_ENTRIES`). Whichever bites first wins. Anything
    trimmed is still one click away via the full commit list.
    """
    upstream = _resolve_upstream_ref(upstream_commit)
    if not upstream:
        print(
            "warning: no upstream ref resolved; the changelog will cover all"
            " reachable commits, not just this fork's. Pass --upstream-commit.",
            file=sys.stderr,
        )
    log_range = f"{upstream}..HEAD" if upstream else "HEAD"

    raw = git(
        "log", log_range,
        "--no-merges",
        f"--since={CHANGELOG_WINDOW} ago",
        "--pretty=format:%s|%h|%as",
    )

    empty = (
        f"- No fix/feat commits in the last {CHANGELOG_WINDOW}"
        f" \u2014 [full commit list](https://github.com/{repo_slug}/commits/{commit_sha})"
    )

    if not raw:
        return empty

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
        entries.append(f"- [{date}] {subject} ({short_sha})")

    if not entries:
        return empty

    # git log is newest-first, so the head of the list is what to keep.
    trimmed = len(entries) - CHANGELOG_MAX_ENTRIES
    shown = entries[:CHANGELOG_MAX_ENTRIES]
    if trimmed > 0:
        shown.append(
            f"- \u2026and {trimmed} more in the last {CHANGELOG_WINDOW}"
            f" \u2014 [full commit list](https://github.com/{repo_slug}/commits/{commit_sha})"
        )

    return "\n".join(shown)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate release notes")
    parser.add_argument("--nightly-tag", required=True)
    parser.add_argument("--zip-name", required=True)
    parser.add_argument("--plugins-zip-name", required=True)
    parser.add_argument("--repo-slug", required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument(
        "--variant", default="reshade", choices=sorted(BUILD_LINES),
        help="Build line this release belongs to (UEVR_VARIANT_ID).",
    )
    parser.add_argument(
        "--upstream-commit", default="",
        help="Upstream commit this build was made from, shown in the notes.",
    )
    parser.add_argument("--out-file", required=True)
    args = parser.parse_args()

    base = BUILD_LINES[args.variant]
    nightly_url = f"https://github.com/{base['base_repo']}/releases/tag/{args.nightly_tag}"
    changelog = get_changelog(args.repo_slug, args.commit_sha, args.upstream_commit)
    header = build_line_header(args.variant, args.repo_slug, args.commit_sha)

    notes = f"""{header}
## How to install

1. Download the **{base['base_label']}** this build is patched against:
   **[{args.nightly_tag}]({nightly_url})**
2. Extract the base zip to a folder.
3. Download **{args.zip_name}.zip** from this release.
4. Extract and **overwrite/replace** the files from step 2 with the files from this release.
5. Download **{args.plugins_zip_name}.zip** and run ``install-plugins.bat`` to install shaders and presets.
   Or install manually -- see [INSTALL.md](https://github.com/{args.repo_slug}/blob/{args.commit_sha}/docs/reference/INSTALL.md).
6. Run UEVR as normal.

> **Note:** The post-processing shaders require this patched fork, and only the
> build line they were released for. They will **not** load on stock UEVR
> nightly or on another build line.

## Changes

_Recent fix/feat commits, from the last {CHANGELOG_WINDOW}._

{changelog}

{provenance(args.variant, args.upstream_commit, args.commit_sha, args.repo_slug)}
{credits(args.repo_slug, args.commit_sha)}"""

    with open(args.out_file, "w", encoding="utf-8") as f:
        f.write(notes)

    count = changelog.count("\n- ") + (1 if changelog.startswith("- ") else 0)
    print(f"Release notes written to {args.out_file} ({count} changelog entries)")


if __name__ == "__main__":
    main()
