---
id: 019
title: CharacterChain — unified per-model ingest/playback API + bypass + mono-sum rule
status: in-review
depends-on: [009, 016, 017]
component: dsp
estimated-size: M
---

## Objective
`mws::model::CharacterChain`: one facade selecting the right ingest/playback character
per ModelId (VarClockChain for S900/S950, FixedRateChain for S1000/S1100), the global
`character = OFF` bypass, and the S900/S950 stereo→mono sum rule.

## Context
Read first:
- docs/design/architecture.md §2.1 (`model/CharacterChain.h`), §5.1 (placement in the
  render pipeline: stages [1] and [4]), §9 (authenticity table: global CHARACTER
  bypass; mono engines)
- docs/design/dsp-engine.md §8 (intro + §8.1/§8.2 stage definitions), §8.4 (bypass:
  engine runs at host rate on unquantized audio — basis for engine-only null tests)
- ModelSpec (009), VarClockChain (016), FixedRateChain (017)

TDD: write `tests/unit/test_characterchain.cpp` first.

## Scope
- `include/mws/model/CharacterChain.h` + src:
  - `ingest(AudioBuffer in, ModelId, ParamSnapshot) -> AudioBuffer` — dispatches per
    model; computes model rate from ModelSpec (2.5 × bandwidth for S900/S950;
    sampleRateSel for S1000/S1100); sums stereo to mono FIRST for S900/S950
    (authentic, dsp-engine §5) and reports a `monoSummed` flag for the LCD,
  - `playback(AudioBuffer stretched, ModelId, ParamSnapshot, double clockRatio,
    double hostRate) -> AudioBuffer` — per-model output stage (S1100 dither delta),
  - `character == OFF` ⇒ both calls are identity (no resample, no quantize) and the
    engine model rate is the host rate (dsp-engine §8.4),
  - `modelRateFor(ModelId, ParamSnapshot) -> double` — single authority used by the
    renderer and the FX latency formula (dsp-engine §7.4).
- Tests (write first):
  - dispatch: S950 ingest output is on the 12-bit lattice; S1000 on the 16-bit lattice,
  - bypass: character OFF in/out bit-identical for all four models,
  - mono sum: stereo into S950 yields 1 channel flagged `monoSummed`; S1000 keeps 2,
  - modelRateFor: S950 BW 19.2 ⇒ 48000; S900 BW 16.0 ⇒ 40000; S1000 FS sel honored.

## Out of scope
- S3000 chain (v1.1; the ModelId case must `static_assert`/fail loudly, not silently
  alias another model).
- Realtime block streaming wrappers (022/033 adapt as needed).

## Acceptance criteria
- [ ] Tests written first; tag `[characterchain]`; all pass.
- [ ] One switch point for per-model dispatch (no duplicated model logic elsewhere).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R characterchain --no-tests=error
```
