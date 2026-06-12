---
id: 006
title: mws::core::MovingLpf — S1000/S1100 3rd-order Butterworth voice filter
status: todo
depends-on: [002]
component: dsp
estimated-size: S
---

## Objective
The S1000/S1100 −18 dB/oct voice filter: a true 3rd-order Butterworth (one real pole
+ complex pair, Q = 1.0), transparent when fully open — NOT three identical one-poles.

## Context
Read first:
- docs/design/dsp-engine.md §8.2 (VOICE FILTER paragraph — panel-corrected: per-voice
  keygroup filter, FULLY OPEN by default; 3rd-order Butterworth, Q = 1.0)
- docs/design/architecture.md §2.1 (`core/MovingLpf.h`)
- docs/design/testing-strategy.md §2 MovingLpf bullet (analytic −18 dB/oct check +
  transparency-when-open test)

TDD: write `tests/unit/test_movinglpf.cpp` first.

## Scope
- `include/mws/core/MovingLpf.h` + src:
  - `MovingLpf3`: 3rd-order Butterworth LP (first-order section + biquad with Q=1.0),
    `setCutoff(hz, sampleRate)` closed-form/allocation-free, `process(view)`, `reset()`,
    `setFullyOpen()` (default state: transparent passthrough).
- Tests (write first):
  - analytic −18 dB/oct response check (magnitude at fc/2fc/4fc vs the analytic
    3rd-order Butterworth, ±0.5 dB),
  - one real pole + Q=1.0 pair, not three identical one-poles (assert the measured
    response differs from a cascaded-one-pole reference by > 1 dB at 2fc),
  - transparency when fully open: output equals input within 1e-6 across 20 Hz–20 kHz
    test tones.

## Out of scope
- S3000 −12 dB/oct resonant SVF (v1.1, ADR-004 — slot only, no code).
- Exposing a FILTER control in the UI (not user-exposed at v1).

## Acceptance criteria
- [ ] Tests written first; tag `[movinglpf]`; all pass.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R movinglpf --no-tests=error
```
