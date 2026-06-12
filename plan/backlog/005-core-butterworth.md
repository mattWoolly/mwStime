---
id: 005
title: mws::core::Butterworth — 6th-order LP (3 cascaded biquads) with retunable cutoff
status: in-review
depends-on: [002]
component: dsp
estimated-size: M
---

## Objective
A 6th-order Butterworth low-pass (3 cascaded biquads, Q = 0.5176 / 0.7071 / 1.9319)
with closed-form, allocation-free coefficient recompute — the S900/S950 reconstruction
filter that must track a variable clock.

## Context
Read first:
- docs/design/architecture.md §2.1 (`core/Butterworth.h` spec line)
- docs/design/dsp-engine.md §8.1 (reconstruction: cutoff = clock / 2.5, cutoff TRACKS
  the clock; per-note retuning recomputes coefficients on the audio thread, no
  allocation)
- docs/design/testing-strategy.md §2 Butterworth bullet (exact required tests)
- docs/research/deep-research-report.md Finding 4 (clock-tracking SC filter — the
  hardware behavior this models)

TDD: write `tests/unit/test_butterworth.cpp` first.

## Scope
- `include/mws/core/Butterworth.h` + src:
  - `Butterworth6LP`: `setCutoff(hz, sampleRate)` (closed-form bilinear-transform
    biquad coefficients, the three standard Butterworth Q factors), `process(view)`,
    `reset()`, per-sample `processSample` for the variable-clock chain.
  - `setCutoff` is noexcept, allocation-free, callable per note-on (architecture.md §4.2).
- Tests (write first, per testing-strategy.md §2):
  - −3 dB point within 1% of requested cutoff,
  - ≈ −36 dB/oct slope verified at 2× cutoff,
  - magnitude at fc / 2fc / 4fc within ±0.5 dB of the analytic 6th-order response at
    **three clock rates** spanning 7.5–48 kHz equivalents,
  - stability (bounded output on noise) across the clock range and under repeated
    `setCutoff` recompute mid-stream.

## Out of scope
- The 3rd-order voice filter (task 006).
- The ZOH/oversampling chain that uses this filter (task 016).

## Acceptance criteria
- [ ] Tests written first; tag `[butterworth]`; all pass.
- [ ] Coefficient recompute does not allocate (no new/malloc in the call path).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R butterworth --no-tests=error
```
