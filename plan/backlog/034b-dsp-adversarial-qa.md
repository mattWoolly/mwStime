---
id: 034b
title: Adversarial DSP QA — NaN/denormal hunts, host-rate matrix, FX soak, extremes, (PI) audit
status: in-review
depends-on: [033, 034]
component: qa
estimated-size: M
---

## Objective
The testing-strategy §7 Wave 1 adversarial fleet work, made into committed tests and
checklists: NaN/inf/denormal hunts per model, the host sample-rate matrix verifying
model-rate invariance, a long-run FX soak with flat memory/CPU, parameter extremes,
render-cap behavior on long files, and the repo-wide (PI)-constant audit.

## Context
Read first:
- docs/design/testing-strategy.md §7 Wave 1 (the spec for this task) and the exit
  criteria ("every (PI) constant either confirmed or ticketed")
- docs/design/dsp-engine.md §3.5 (host-rate processing: model-rate invariance is a
  hard commitment — engine-level test added in task 024; this task covers the
  processor level and the wider rate matrix), §3.4 edge rules (cycle 20, T extremes)
- docs/research/akaizer-analysis.md §2.1 (40 ms minimum-input floor)
- EngineHost FX (033), SAMPLE mode (034), OfflineRenderer cap (020 via 034)

## Scope
- `tests/plugin/test_dsp_adversarial.cpp` (label `qa-adversarial`):
  - NaN/inf/denormal hunt per model × mode: feed DC, ±1 FS squares, denormal tails
    through FX and SAMPLE paths — assert finite output and no denormal-driven CPU
    cliff (flush-to-zero policy verified),
  - extremes: cycle 20 @ 999% (S950), cycle 2000 @ 25%, T at both range ends,
    a 40 ms input (the documented input floor), zone of minimum length — no crash,
    no NaN, lengths still schedule-derived,
  - host sample-rate matrix: 44.1/48/88.2/96/192 kHz host rates, character ON —
    processor output is model-rate invariant (same underlying render within SRC
    tolerance; extends the 024 engine-level test to the plugin),
  - render-cap: 2000% on a synthetic long file refused with the typed event and no
    over-cap allocation (processor level).
- FX soak: a 30-minute streamed run (scripted, tag `[.soak]` — excluded from the
  default ctest run, invoked explicitly) asserting flat memory (no growth after
  warm-up) and bounded CPU; document how to run it in the test header.
- **(PI)-constant audit** (`docs/qa/pi-audit.md`): sweep every **(PI)** tag in
  docs/design/ and in code comments; table each constant → tuning note or open
  issue. Must include the three flagged late additions: 034 mode-switch fade,
  035 ~5 ms declick, 036 16-bit export default.

## Out of scope
- UI adversaries / screenshot gates (047b) and host smoke matrix (048b).
- Hardware-capture calibration (026b) and Akaizer cross-check (026c).
- Fixing found defects beyond trivial ones — file follow-up tasks with repro tests.

## Acceptance criteria
- [ ] `qa-adversarial` labeled tests pass; soak test exists, runs to completion
      locally (result + memory/CPU figures recorded in the PR).
- [ ] Host-rate matrix test pins model-rate invariance at all five rates.
- [ ] pi-audit.md lists every (PI) constant with a tuning note or issue link.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -L qa-adversarial --no-tests=error
# soak (explicit, ~30 min — run once and record):
./build/default/tests/plugin/test_dsp_adversarial "[.soak]" || true  # adjust to harness; result in PR
```
