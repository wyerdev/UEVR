# UEVR-build docs/

Agent-maintained knowledge base. These files are excluded from git (via
`.git-exclude-list`) — they're working documents, not public documentation.

## Folder layout

| Folder | Purpose | When things move out |
|---|---|---|
| `active/` | Plans currently being worked on | → `historical/` when work ships |
| `reference/` | Stable architecture docs, guides, install instructions | Stays; gets date reviews |
| `historical/` | Completed plans, post-mortems, old iteration logs | Stays forever |

## Conventions

Based on LLM Wiki v2 (Karpathy → Rohitg00 → Mattia83it), adapted for a
single-developer C++ project. Full agent rules in
`.github/instructions/knowledge-management.instructions.md`.

### Dates

Every fact gets a date: `[2026-05-10]`. Section headers get
`(last confirmed: YYYY-MM-DD)` when reviewed. Stale dates are a signal to
re-check, not auto-discard.

### Status markers

- **(unmarked)** — Current truth. Working as described at the dated time.
- **[PROPOSED]** — Plan only. Not implemented.
- **[BROKEN]** — Known issue. Includes what's wrong.
- **[HISTORICAL]** — Completed or superseded. Kept for context.

Default is unmarked. Only annotate deviations.

### Supersession

Old facts get ~~strikethrough~~ and a `→ superseded by [section]` pointer.
Nothing is deleted — history explains "why was it like that?"
