---
description: Knowledge management conventions — LLM Wiki v2 adapted for UEVR-build
applyTo: '**'
---

# Knowledge Management

This project uses an adapted LLM Wiki v2 pattern for agent memory. Knowledge
compounds across sessions instead of being re-derived. Full rationale in
`/memories/repo/llm-wiki-conventions.md`.

## Always do

1. **Date every fact you write.** `[YYYY-MM-DD]` prefix. Undated facts are
   untrustable. When updating a fact, update its date.

2. **Confidence by exception.** Don't annotate working things. Only mark
   status when it deviates:
   - `[PROPOSED]` — plan only, not implemented
   - `[BROKEN]` — known issue, include what's wrong
   - `[HISTORICAL]` — completed or superseded, kept for context

3. **Supersede, never delete.** When a fact is replaced, ~~strikethrough~~ the
   old text and add `→ superseded by [new section]`. History explains "why."

4. **Check dates for staleness.** On session start, note any memory dates that
   are significantly old relative to current date — signal to re-verify, not
   auto-discard.

5. **Build on existing knowledge.** Read `/memories/repo/` files relevant to
   the task. Don't re-derive what's already recorded.

6. **Update plans when work lands.** If a docs/ plan phase completed during
   this session, update its status in the same turn. Don't wait for a separate
   "update the plan" request.

## docs/ folder structure

```
docs/
  active/       — plans currently being worked on
  reference/    — stable architecture docs, guides, how-things-work
  historical/   — completed plans, post-mortems, iteration logs
```

Active plans move to `historical/` when their work ships. Reference docs stay
and get `(last confirmed: YYYY-MM-DD)` on section headers when reviewed.

## What NOT to write

- Transient debugging observations (unless they reveal a reusable pitfall)
- Things obvious from reading the code
- Duplicate facts already in another memory file
- Hardcoded counts of things that will grow
