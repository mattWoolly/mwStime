---
id: 017
title: S1000/S1100 fixed-rate 16-bit character chain (+ S1100 dither delta)
status: done
depends-on: [004, 006, 007]
component: dsp
estimated-size: M
---

## Objective
The §8.2 fixed-rate chain: ingest resample to 44.1/22.05 kHz + 16-bit quantize before
the stretch; after stretch+transpose, the fully-open 3rd-order voice filter and
sinc resample to host rate. S1100 delta: 16-bit output quantize with TPDF dither.

## Context
Read first:
- docs/design/dsp-engine.md §8 intro (ingest BEFORE stretch — TAL-validated order,
  DRR F8), §8.2 (the chain; voice filter fully open by default — hard-wiring it
  active would fabricate an artifact), §2 (`sampleRateSel` 44.1/22.05)
- docs/research/deep-research-report.md Finding 5 (S1000 fixed-rate interp playback),
  Finding 8 (downsample-then-process order)
- mws::core: Resampler (004), MovingLpf3 (006), Quantizer (007)

TDD: write `tests/unit/test_s1000chain.cpp` first.

## Scope
- `include/mws/model/FixedRateChain.h` + src:
  - `ingest(AudioView in, double inRate, double modelRate /*44100|22050*/)
    -> AudioBuffer`: sinc resample + 16-bit quantize,
  - `playback(AudioView in, double modelRate, double hostRate, bool s1100Dither,
    uint64 seed) -> AudioBuffer`: MovingLpf3 fully open (transparent) → sinc resample
    to host rate → (S1100 only) 16-bit quantize with seeded TPDF dither
    (the 20-bit-DAC noise-floor model, PI),
  - NO output-level offset between S1000 and S1100 (panel ruling, dsp-engine §2
    outTrim row).
- Tests (write first):
  - ingest at 22.05 kHz band-limits (energy above 11.025 kHz ≤ −50 dB) and output is
    on the 16-bit lattice,
  - playback with filter fully open is transparent (within resampler tolerance) for a
    1 kHz sine at 44.1→48 kHz,
  - S1100 dithered output differs from S1000 only at the ±1 LSB level (max abs diff
    ≤ 2/32768) and is deterministic per seed,
  - chain order: quantization happens before stretch would run (API-level: ingest
    output is already quantized — assert lattice).

## Out of scope
- Transpose stage itself (task 018).
- S3000 chain (v1.1 — ADR-004).
- Unified per-model CharacterChain API (task 019).

## Acceptance criteria
- [ ] Tests written first; tag `[s1000chain]`; all pass.
- [ ] Voice filter defaults fully open; nothing exposes a user filter control.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R s1000chain --no-tests=error
```
