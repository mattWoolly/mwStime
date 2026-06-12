---
id: 013
title: RepitchEngine — S900 varispeed (no timestretch), ZOH read
status: in-review
depends-on: [002]
component: dsp
estimated-size: S
---

## Objective
`mws::stretch::RepitchEngine`: the honest S900 mode — varispeed repitch where
`rate = 1/T`, transpose multiplies the same rate, read is zero-order hold with NO
interpolation. The variable-rate read IS the virtual DAC clock consumed by the §8.1
character chain.

## Context
Read first:
- plan/decisions/003-s900-mode-semantics.md (the whole ADR — option B decision)
- docs/design/dsp-engine.md §6 (pseudocode + semantics; semitone display formula
  `-12·log2(T)`; inert params), §8.1 (how the engine's clock feeds the chain)
- docs/research/deep-research-report.md Finding 4 (ZOH, no interpolation —
  service-manual grade)

TDD: write `tests/unit/test_repitch.cpp` first.

## Scope
- `include/mws/stretch/RepitchEngine.h` + src:
  - offline mono API: `render(AudioView src, double timeFactorPct,
    double transposeSemitones) -> RepitchResult { AudioBuffer out; double clockRatio; }`
    where `clockRatio = rate · 2^(transpose/12)` and `rate = 100/timeFactorPct`,
  - ZOH read: output sample = `src[floor(pos)]`, NO interpolation [DRR F4],
  - output length = `round(N / clockRatio)` derived from the read schedule,
  - helper `semitoneOffset(timeFactorPct)` returning `-12·log2(T)` for the LCD.
- Tests (write first):
  - T=200% ⇒ output length ≈ 2N (±1), every output sample equals some `src[i]`
    verbatim (ZOH ⇒ no new values),
  - T=200% ⇒ `semitoneOffset == -12.0` exactly; T=100% ⇒ 0,
  - transpose +12 at T=100% halves the length (rate 2.0) and reports clockRatio 2.0,
  - determinism (run twice, bit-identical).

## Out of scope
- The character chain itself (12-bit, tracking filter — task 016) — this engine only
  reports the clock ratio.
- FX-mode rate clamp (task 022 implements FREE-mode causality clamps).
- TIME SKEW (rejected for v1, ADR-003).

## Acceptance criteria
- [ ] Tests written first; tag `[repitch]`; all pass.
- [ ] No interpolation anywhere in the read path (ZOH only).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R repitch --no-tests=error
```
