---
id: 016
title: Variable-clock chain (S900/S950) — oversampled ZOH + tracking Butterworth (early-risk prototype)
status: done
depends-on: [004, 005, 007]
component: dsp
estimated-size: M
---

## Objective
The §8.1 variable-clock 12-bit character path: ingest resample to `2.5 × bandwidth`,
12-bit quantize, virtual variable-clock ZOH playback at an internal oversampled rate,
clock-tracking 6th-order Butterworth, decimate to host rate. This is the heaviest,
least-oracle-backed DSP — explicitly the early-risk prototype (architecture.md §10
risk 3); validate CPU and properties before further chain work.

## Context
Read first:
- docs/design/dsp-engine.md §8.1 (the full chain, including the IMPLEMENTATION NOTE:
  oversample ≥ 2 × max(clock, hostRate), 4× host default (PI), images represented
  before filtering, decimate after the Butterworth; NO saturation layers — refuted)
- docs/research/deep-research-report.md Finding 4 (BA9221 DAC + MF6CN-50 SC filter,
  clock-controlled, rate = 2.5 × bandwidth) and Refuted #4 (no preamp saturation)
- docs/design/testing-strategy.md §3 item 9 (the 12-bit chain property tests — write
  these FIRST)
- mws::core: Resampler (004), Butterworth6LP (005), Quantizer (007)

TDD: write the §3.9 property tests first.

## Scope
- `include/mws/model/VarClockChain.h` + src (used by CharacterChain task 019):
  - `ingest(AudioView in, double inRate, double bandwidthKHz) -> AudioBuffer`:
    sinc-resample to `f_s = 2.5 × bandwidth`, 12-bit mid-tread quantize (no dither),
  - `playback(AudioView stretched, double f_s, double clockRatio, double hostRate)
    -> AudioBuffer`: ZOH at virtual clock `f_s × clockRatio` rendered into an internal
    oversampled rate (default 4× host (PI), ≥ 2 × max(clock, hostRate)), 6th-order
    Butterworth at cutoff `clock / 2.5` tracking the clock, then sinc-decimate to host
    rate,
  - stretch arithmetic stays 16-bit-precision capable ("12-bit sampling/16-bit
    processing", dsp-engine §8.1) — the chain quantizes values, not storage.
- A micro-benchmark Catch2 `[!benchmark]` case (or timed test with generous bound)
  documenting CPU cost of 1 s of audio — the early-risk datum.
- Tests written first (`tests/unit/test_varclock.cpp`, per testing-strategy §3.9):
  - distinct output values ≤ 4096 lattice pre-reconstruction (after ingest+quantize),
  - ZOH images present pre-filter (spectral peak near k·clock ± f0), attenuated
    ≥ 24 dB post-Butterworth (PI bound, tunable),
  - high-band energy tracks downward under pitch-down (clockRatio 0.5 ⇒ output energy
    above clock/2 drops vs clockRatio 1.0 — the clock-tracking property, DRR F4),
  - determinism.

## Out of scope
- Wiring into a per-model CharacterChain API (task 019).
- SC-filter clock-bleed whine (v2 idea, no oracle — dsp-engine §8.1).
- Real-time block-based variant optimizations (FX wiring happens in 022/033; keep the
  API offline-buffer based but allocation patterns sane).

## Acceptance criteria
- [ ] Property tests written first; tag `[varclock]`; all pass.
- [ ] No saturation/drive stage anywhere in the chain (refuted — DRR refuted #4).
- [ ] Benchmark case exists and its measured cost is recorded in the PR description.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R varclock --no-tests=error
```
