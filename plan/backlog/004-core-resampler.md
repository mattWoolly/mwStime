---
id: 004
title: mws::core::Resampler — windowed-sinc and linear-interp resamplers
status: todo
depends-on: [002]
component: dsp
estimated-size: M
---

## Objective
Two offline resamplers in `mws::core`: a 16-tap Kaiser(β=8) windowed-sinc resampler
(quality path: ingest SRC, transpose, host-rate output) and a 2-point linear-interp
resampler (cheap path), both with reported group delay.

## Context
Read first:
- docs/design/architecture.md §2.1 (`core/Resampler.h`)
- docs/design/dsp-engine.md §7.2 (16-tap windowed-sinc Kaiser β=8 **(PI)** for
  S1000/S1100 transpose), §7.4 + §3.5 (SRC group delay feeds the FX latency formula),
  §8.1/§8.2 (ingest resample + decimate-to-host stages)
- docs/design/testing-strategy.md §2 Resampler bullet (required tests)

TDD: write `tests/unit/test_resampler.cpp` first.

## Scope
- `include/mws/core/Resampler.h` + `src/core/Resampler.cpp`:
  - `SincResampler`: arbitrary-ratio offline resample of an `AudioView` into an
    `AudioBuffer`; 16-tap Kaiser β=8 kernel; exposes `groupDelaySamples(ratio)`.
  - `LinearResampler`: 2-point linear interpolation, same API shape.
  - Deterministic: precomputed kernel tables, double-precision phase accumulator, no
    platform-dependent math shortcuts.
- Tests (write first):
  - identity at ratio 1.0 (sinc path: output equals input within 1e-6 after group-delay
    alignment; linear path exact),
  - sine SNR bound: 440 Hz @ 44.1k→48k, SNR ≥ 60 dB for sinc (PI bound, tunable),
  - impulse response symmetric; measured delay matches `groupDelaySamples` within ±0.5
    samples,
  - downsampling band-limits (energy above the new Nyquist ≤ −50 dB for sinc).

## Out of scope
- Real-time/streaming resampling (RealtimeStretcher handles its own ring usage).
- The ZOH/variable-clock playback stage (task 016).

## Acceptance criteria
- [ ] Tests written first; tag `[resampler]`; all pass.
- [ ] Group-delay report matches measurement (testing-strategy.md §2).
- [ ] Pure C++20, no deps.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R resampler --no-tests=error
```
