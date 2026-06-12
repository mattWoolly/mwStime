---
id: 014
title: Auto cycle detection (autC / AUTO-D)
status: todo
depends-on: [008]
component: dsp
estimated-size: S
---

## Objective
The autC/AUTO-D helper: a deterministic function that proposes a cycle length from
audio content (normalized autocorrelation over the stretch zone or first 200 ms),
falling back to 1000 when no confident peak exists.

## Context
Read first:
- docs/design/dsp-engine.md §7.1 (full spec: lag range 20–2000, threshold 0.3 (PI),
  fallback 1000, first-200 ms default zone — all PI-tagged inference, ADR-001-flagged)
- docs/research/akai-manuals-specs.md §3 p.46 / §2 (the documented hardware feature:
  "software logic … like autolooping", "not always infallible")
- mws::core::AutoCorr (task 008)
- docs/design/testing-strategy.md §2 AutoCorr bullet

TDD: write `tests/unit/test_autocycle.cpp` first.

## Scope
- `include/mws/stretch/AutoCycle.h` (+ src):
  - `detectCycleLen(AudioView zone, double sampleRate) -> int`:
    analysis window = zone, or its first 200 ms if longer (PI); lag range 20–2000
    samples; returns best lag if normalized peak > 0.3, else 1000.
- Tests (write first):
  - 100 Hz saw @ 44.1 kHz ⇒ 441 ± 1,
  - white noise ⇒ exactly 1000 (fallback),
  - zone shorter than 200 ms still works (uses whole zone),
  - constants (200 ms, 0.3, fallback 1000) live in one named-constants block so QA can
    retune (every PI constant needs a tuning note — testing-strategy.md §7).

## Out of scope
- UI wiring of the F2 autC soft key (task 045b, via the 028 trigger parameter).
- S950 MON1 per-grain snapping (task 015, uses AutoCorr directly).

## Acceptance criteria
- [ ] Tests written first; tag `[autocycle]`; all pass.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R autocycle --no-tests=error
```
