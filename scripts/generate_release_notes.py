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

# Match commits starting with fix or feat (case-insensitive)
COMMIT_RE = re.compile(r"^(fix|feat)", re.IGNORECASE)

# How much history a release body is allowed to show. Both limits apply; see
# get_changelog(). CHANGELOG_WINDOW is used both in prose and, with " ago"
# appended, as git's --since argument.
CHANGELOG_WINDOW = "3 months"
CHANGELOG_MAX_ENTRIES = 7

# Build lines, keyed by variant ID (must match UEVR_VARIANT_ID in
# include/uevr/Variant.hpp). Deferred lines are deliberately absent; see
# docs/active/build-lines-todo.md.
#
# "published" means the line has release CI and has actually shipped at least
# one release. Unpublished lines are still listed, but never linked, so we
# never point people at an empty search. Flip it to True in the same change
# that adds the line's release workflow.
#
# "marker" is the opaque, single-word ID that identifies this line on the
# releases page. Every "all builds of this line" link queries it and nothing
# else -- /releases?q=buildline2afw.
#
# GitHub's releases filter is an AND over the words of the query, matched
# against each release's title and body, and it highlights every match
# (measured 2026-07-30). So the query word must be something that occurs
# exactly once, in exactly one line's bodies:
#
#   - The full tag prefix is wrong: `uevr-reshade-afw-release` lights up
#     "uevr", "reshade" and "release" all over every card.
#   - A natural word is wrong too: `afw` matches the AFW line *and* the
#     AFW + Joeyhodge line, whose name contains it. Any human-readable name
#     for a combined line contains the names of its parts, so this is not
#     fixable by choosing better words.
#
# Hence a made-up token. Rules for adding one:
#
#   1. No hyphens, spaces or punctuation -- the filter splits on them.
#   2. No marker may be a substring of another. The numbering is what
#      guarantees this: `buildline2afw` cannot occur inside
#      `buildline3afwjoeyhodge`. Give a new line the next free number.
#   3. It must appear in its own bodies exactly once, which
#      build_line_header() does, and never in another line's --
#      check_keywords() enforces that.
#
# The token is invisible to users; they follow a link whose text is the line
# name. It exists only to be a search term nothing else can collide with.
BUILD_LINES = {
    "reshade": {
        "name": "UEVR ReShade Mainline",
        "upstream": "[praydog/UEVR](https://github.com/praydog/UEVR) `master`",
        "upstream_repo": "praydog/UEVR",
        "upstream_branch": "master",
        "tag_prefix": "uevr-reshade-mainline-release",
        "marker": "buildline1mainline",
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
        "tag_prefix": "uevr-reshade-afw-release",
        "marker": "buildline2afw",
        "published": True,
        # AFW is not a praydog nightly: the base is PureDark's own release,
        # which is also the only source of the proprietary PDAFWPlugin.dll
        # this build line delay-loads.
        "base_repo": "PureDark/UEVR",
        "base_label": "base UEVR AFW release",
    },
    "reshade-afw-joeyhodge": {
        "name": "UEVR ReShade AFW + Joeyhodge",
        "upstream": "[PureDark/UEVR](https://github.com/PureDark/UEVR) `Joey-Merged`"
                    " \u2014 asynchronous frame warp on top of"
                    " [joeyhodge/UEVR](https://github.com/joeyhodge/UEVR)'s"
                    " UE 5.5-5.8 fixes",
        "upstream_repo": "PureDark/UEVR",
        "upstream_branch": "Joey-Merged",
        "tag_prefix": "uevr-reshade-afw-joeyhodge-release",
        "marker": "buildline3afwjoeyhodge",
        "published": True,
        # Same base release as the AFW line: PureDark ships two builds inside
        # each UEVR_AFW_* release, one nightly-based and one joeyhodge-based.
        # This line pairs with the joeyhodge-based one, and that zip is also
        # the only source of the proprietary PDAFWPlugin.dll it delay-loads.
        "base_repo": "PureDark/UEVR",
        "base_label": "base UEVR AFW release (joeyhodge-based build)",
    },
}

def build_line_header(variant: str, repo_slug: str, commit_sha: str) -> str:
    """Banner saying which build line this release is, and where the others are.

    Every build line shares one releases page and the releases are mixed
    together by date, so it is easy to land on the wrong one. This block goes
    at the top of every release body so people can find their way back.

    **Never put another line's marker here.** GitHub's releases filter matches
    body text, so one stray marker puts this release under that line's filter.
    This block links only to its own line, by marker rather than by tag prefix
    (see BUILD_LINES), and sends everyone else to INSTALL.md, which is not a
    release body and can name every line freely. check_keywords() enforces it.

    The marker line at the end is the only place the token appears, and is
    what makes the filter link work at all.
    """
    line = BUILD_LINES[variant]
    install_url = (
        f"https://github.com/{repo_slug}/blob/{commit_sha}"
        "/docs/reference/INSTALL.md"
    )
    own_filter = f"https://github.com/{repo_slug}/releases?q={line['marker']}"

    # Named so people recognise the variant they want; "build line" on its own
    # means nothing to a reader. Derived from BUILD_LINES so a new line shows up
    # here automatically. The common "UEVR " prefix carries no information.
    others = ", ".join(
        other["name"].removeprefix("UEVR ")
        for other_id, other in BUILD_LINES.items() if other_id != variant
    )

    return f"""# {line['name']}

This release patches **{line['upstream']}**. The core zip, the shaders zip, and
your installed shader folder must all come from this same build line — you
cannot mix them. [All builds of this version]({own_filter})

Want a different version ({others})? See
[How to Install]({install_url}#choose-your-build-line).

<sub>build line ID: {line['marker']}</sub>
"""


def check_keywords(variant: str, notes: str) -> None:
    """Fail if the body carries a build-line marker other than its own.

    The releases filter matches body text, so one stray marker makes this
    release appear under that line's filter -- the exact bug this design
    replaced. Catching it here is the only reason the filter can be trusted,
    so this is a hard failure, not a warning.

    Markers are opaque tokens precisely so this can never fire by accident
    from ordinary prose or a changelog subject.
    """
    for other_id, other in BUILD_LINES.items():
        if other_id == variant:
            continue
        marker = other["marker"]
        if marker.lower() in notes.lower():
            raise SystemExit(
                f"error: these notes for '{variant}' contain the marker"
                f" '{marker}', which identifies the '{other_id}' build line."
                f" This release would show up under that line's filter."
                f" Remove the mention and retry."
            )

    own = BUILD_LINES[variant]["marker"]
    if own.lower() not in notes.lower():
        raise SystemExit(
            f"error: the marker '{own}' is missing from these notes. It is the"
            f" only thing the releases filter for '{variant}' matches on, so"
            f" this release would be unreachable from its own filter link."
        )


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

## Changes

{changelog}

## Credits

See [INSTALL.md \u2192 Credits](https://github.com/{args.repo_slug}/blob/{args.commit_sha}/docs/reference/INSTALL.md#credits).
"""

    check_keywords(args.variant, notes)

    with open(args.out_file, "w", encoding="utf-8") as f:
        f.write(notes)

    count = changelog.count("\n- ") + (1 if changelog.startswith("- ") else 0)
    print(f"Release notes written to {args.out_file} ({count} changelog entries)")


if __name__ == "__main__":
    main()
