# Backlog

Atomic, agent-executable task files. One file = one task = one branch = one PR.

## File naming

`NNN-short-slug.md` — NNN is a zero-padded ordinal that encodes rough dependency order
(lower numbers generally unblock higher ones). The `meta.status` field is the source of
truth for state, not the filename.

## Task file format

```markdown
---
id: NNN
title: One-line task title
status: todo | in-progress | in-review | done | blocked
depends-on: [list of task ids that must be `done` first]
component: dsp | engine | plugin | ui | infra | qa | docs
estimated-size: S | M | L   # S: <200 LOC, M: <600 LOC, L: needs splitting consideration
---

## Objective
What exists when this task is done, in one or two sentences.

## Context
The minimum a fresh agent needs: which files/docs to read first (paths), relevant
research citations (docs/research/...), relevant ADRs (plan/decisions/...).

## Scope
- Bullet list of exactly what to build/change.

## Out of scope
- Explicit non-goals to prevent scope creep.

## Acceptance criteria
- [ ] Checkable criteria. Tests that must exist and pass. Build targets that must succeed.

## Verification commands
Exact commands the agent must run and see pass before opening a PR.
```

## Workflow per task

1. Claim: set `status: in-progress` (committed on your branch, not main).
2. Branch from latest main: `task/NNN-short-slug` (use a git worktree if working in parallel).
3. Implement within scope. TDD for DSP/engine code.
4. Run the task's verification commands; all must pass.
5. Push branch, open PR titled `NNN: <title>` with `gh pr create`, body summarizing
   what/why/verification output. Set `status: in-review` in the task file in the PR.
6. Reviewer agent reviews; merge squashes to main and sets `status: done`.
