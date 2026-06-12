---
id: 010
title: CyclicEngine CLASSIC — integer-hop two-grain scheduler + SpliceCal
status: todo
depends-on: [002]
component: dsp
estimated-size: M
---

## Objective
The heart of the plugin: `mws::stretch::CyclicEngine` in CLASSIC mode — the
[AKZ §4.2] two-grain overlap scheduler with integer input hop, integer/fixed-point
arithmetic on the stretch path, and all splice constants in a `SpliceCal` calibration
struct.

## Context
Read first (in this order):
- docs/design/dsp-engine.md §3.0 (scheduling-model decision — adopt [AKZ §4.2], NEVER
  the §10 splice formula), §3.1 (definitions, SpliceCal), §3.2 (CLASSIC hop arithmetic
  + the three MUST-hold properties), §3.3 (linear crossfade), §3.4 (reference
  pseudocode + edge rules — implement this verbatim)
- docs/research/akaizer-analysis.md §4.1–§4.2 (OpenMPT arithmetic: 32.32 positions,
  15-bit fraction lerp, integer-position grain starts), §2.2 (CLASSIC semantics)
- plan/decisions/001-dsp-engine-architecture.md (resolution 3: scheduler choice;
  resolution 4: bit-exactness scope)
- docs/design/testing-strategy.md §3 items 1–2 (the property tests this task must
  write FIRST)

TDD is mandatory: write the two property tests before the engine.

## Scope
- `include/mws/stretch/CyclicEngine.h` + `src/stretch/CyclicEngine.cpp`:
  - `struct SpliceCal { float overlapF = 0.20f; FadeShape shape = Linear;
    HopRounding rounding = RoundNearest; }` — constructor parameter (dsp-engine §3.1).
  - Offline mono API: `render(AudioView src, int cycleLenSamples, double timeFactorPct,
    HopMode mode) -> AudioBuffer` (CLASSIC in this task; REVISED stub returns
    unimplemented — task 011).
  - CLASSIC: timeFactor coerced to integer %; `hop_in = round(hop_out / T)`, ≥ 1;
    integer 32.32 fixed-point positions; crossfade = rounded integer lerp (15-bit
    fraction) so the path is float-free (cross-platform bit-exactness claim,
    architecture.md §2).
  - Edge rules from §3.4: reads past N−1 return 0; clamp C to N if file shorter;
    T < 1 skips material; loop-exit conditions exactly as pseudocoded.
  - `expectedOutputLength(...)` helper derived from the scheduler (used by LCD later) —
    but tests must compute the length INDEPENDENTLY (testing-strategy.md §3.2).
- Tests written first (`tests/unit/test_cyclic_classic.cpp`):
  - **verbatim-copy property** (testing-strategy §3.1): with `src[n] = n`
    index-encoding, every output sample is `src[i]` at an integer offset or, inside a
    crossfade region only, the rounded convex combination of exactly two such samples,
  - **length quantization** (testing-strategy §3.2): output length equals the
    independently computed `(G−1)·hop_out + C` with `hop_in = round(hop_out/T)`, and
    differs from `round(N·T)` for at least one tested (N, C, T),
  - T < 100% (compression) length follows the same schedule,
  - hop_in clamps to ≥ 1 at extreme T; short-input C-clamp behaves.

## Out of scope
- REVISED mode (task 011), splice-comb/stereo/determinism suite (task 012).
- Transpose, character, normalization, realtime streaming.

## Acceptance criteria
- [ ] Property tests 1–2 written first and pass; tag `[cyclic][classic]`.
- [ ] Stretch path contains no floating-point arithmetic (code review criterion:
      integer/fixed-point only per dsp-engine §3.2).
- [ ] SpliceCal is the only place overlap/fade/rounding constants live.
- [ ] Running the same render twice yields bit-identical output.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R cyclic --no-tests=error
```
