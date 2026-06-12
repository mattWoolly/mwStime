---
id: 008
title: mws::core::AutoCorr — normalized autocorrelation period estimator
status: todo
depends-on: [002]
component: dsp
estimated-size: S
---

## Objective
A deterministic normalized-autocorrelation pitch/period estimator over a lag range —
the primitive behind autC/AUTO-D (task 014) and the S950 MON1 per-grain cycle snap
(task 015).

## Context
Read first:
- docs/design/architecture.md §2.1 (`core/AutoCorr.h`)
- docs/design/dsp-engine.md §7.1 (auto cycle detection **(PI)**: lag range 20–2000,
  peak threshold 0.3, fallback 1000) and §5 MON1 row (snap C to nearest detected
  period)
- docs/design/testing-strategy.md §2 AutoCorr bullet (synthetic saw test, fallback)

TDD: write `tests/unit/test_autocorr.cpp` first.

## Scope
- `include/mws/core/AutoCorr.h` (+ src):
  - `bestLag(AudioView, lagMin, lagMax, threshold) -> std::optional<int>`: normalized
    autocorrelation, highest peak above threshold; deterministic tie-break (lowest lag).
  - `bestLagNear(AudioView, center, searchFraction, threshold)` for the MON1 "around C"
    search (dsp-engine.md §5).
- Tests (write first):
  - synthetic 100 Hz saw at 44.1 kHz detects lag 441 ± 1,
  - sub-threshold white noise returns nullopt (caller falls back to 1000),
  - `bestLagNear` finds the period of a 220 Hz sine when centered at 1.5× the true lag,
  - same input ⇒ same result (determinism, run twice).

## Out of scope
- The autC/AUTO-D feature itself (zone selection, fallback policy — task 014).

## Acceptance criteria
- [ ] Tests written first; tag `[autocorr]`; all pass.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R autocorr --no-tests=error
```
