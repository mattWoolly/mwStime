---
id: 018
title: Transpose stage — post-stretch sinc repitch (S1000/S1100) + clock-modulation hook (S900/S950)
status: in-review
depends-on: [004, 009, 016]
component: dsp
estimated-size: M
---

## Objective
The separate transpose pass that runs AFTER the stretch: anti-aliased windowed-sinc
resample for S1000/S1100 models, and for S900/S950 a clock-modulation path that does
NOT resample but multiplies the VarClockChain's virtual clock so filter and imaging
track pitch.

## Context
Read first:
- docs/design/dsp-engine.md §7.2 (the whole section — sinc kernel is PI-tagged;
  S900/S950 modulate the virtual DAC clock instead; this is the product
  differentiator vs RX950), §8.1 (clock = f_s × 2^(transpose/12))
- docs/research/akaizer-analysis.md §2.4 (anti-aliasing on pitch shift; stretch first,
  repitch at playback), §10 (ratio p = 2^(−transpose/12) as output-length resample)
- docs/research/deep-research-report.md Finding 7 (RX950's missing clock tracking)
- mws::core::SincResampler (004); ModelId (009); VarClockChain (016)

TDD: write `tests/unit/test_transpose.cpp` first.

## Scope
- `include/mws/engine/Transpose.h` + src:
  - `transposeSinc(AudioView in, double semitones) -> AudioBuffer`: ratio
    `p = 2^(−semitones/12)` applied as output-length resample through the 16-tap
    Kaiser β=8 sinc with anti-alias band-limiting when pitching up,
  - `clockRatioFor(double semitones) -> double` = `2^(semitones/12)` — the multiplier
    handed to `VarClockChain::playback` for S900/S950 (no resampling here),
  - routing rule encoded in one function (`usesSincTranspose(ModelId)`-style predicate
    or equivalent constant table) so OfflineRenderer (020) can't mis-route.
- Tests (write first):
  - +12 semitones halves length (±1) and shifts a 440 Hz sine to 880 Hz (peak bin),
  - −12 doubles length, no imaging above the original band (≤ −50 dB),
  - 0 semitones = identity within sinc tolerance,
  - pitching up band-limits (no alias of a near-Nyquist tone, ≤ −50 dB),
  - clockRatioFor(+12) == 2.0 exactly.

## Out of scope
- The MIDI real-time repitch voice (task 035 — distinct path, architecture.md §4.2).
- Wiring into the renderer (task 020).

## Acceptance criteria
- [ ] Tests written first; tag `[transpose]`; all pass.
- [ ] S900/S950 path performs zero resampling in this stage (clock ratio only).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R transpose --no-tests=error
```
