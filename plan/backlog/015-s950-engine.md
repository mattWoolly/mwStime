---
id: 015
title: S950Engine — 999% stretch, D-TIME, MON1/POL2, AUTO-D
status: done
depends-on: [010, 014]
component: dsp
estimated-size: M
---

## Objective
`mws::stretch::S950Engine`: the S950 stretch semantics as a configuration/extension of
CyclicEngine — integer-% CLASSIC by default, timeFactor clamped to 999, D-TIME mapped
to cycle length, POL2 fixed-cycle vs MON1 per-grain pitch-period snap, AUTO-D.

## Context
Read first:
- docs/design/dsp-engine.md §5 (the whole section — the mapping table and its
  deviation-until-calibrated flags are the spec)
- docs/research/akai-manuals-specs.md §2 (S950 STRETCH page 14, D-TIME, Mon1/Pol2,
  mono machine)
- docs/design/dsp-engine.md §3 (the shared cyclic core this builds on)
- plan/decisions/001-dsp-engine-architecture.md (engines = CyclicEngine + config)
- mws::core::AutoCorr (008), AutoCycle (014), CyclicEngine (010)

TDD: write `tests/unit/test_s950.cpp` first.

## Scope
- `include/mws/stretch/S950Engine.h` + src:
  - wraps CyclicEngine; `render(AudioView monoSrc, S950Params)`:
    - stretch % integer, engine-clamped ≤ 999,
    - D-TIME = cycle length C in samples, 20–2000 (PI, deviation-until-calibrated —
      keep the mapping in ONE function with a comment citing dsp-engine §5),
    - POL2: fixed C exactly as set,
    - MON1: per-grain, snap C to nearest detected pitch period via
      `AutoCorr::bestLagNear` around the set C (PI),
    - AUTO-D: run task-014 `detectCycleLen` and use the result as C,
  - mono only: the engine asserts mono input (stereo summing happens in
    OfflineRenderer/CharacterChain per dsp-engine §5 — not here).
- Tests (write first):
  - timeFactor 1500 ⇒ behaves identically to 999 (clamp at the engine),
  - POL2 on a steady sine reproduces CyclicEngine CLASSIC output bit-exactly for the
    same C/T (degenerate equivalence),
  - MON1 on a 220 Hz sine snaps C to the detected period (assert effective C within
    ±1 of 200.5 rounded at 44.1 kHz) and output length follows the snapped schedule,
  - AUTO-D on a 100 Hz saw selects C = 441 ± 1,
  - determinism (bit-identical re-run).

## Out of scope
- The S950 character chain (12-bit/variable clock — tasks 016/019).
- Hardware-capture calibration of D-TIME (QA-phase v1-freeze gate,
  testing-strategy.md §7 Wave 2).

## Acceptance criteria
- [ ] Tests written first; tag `[s950]`; all pass.
- [ ] All PI mappings carry source-comment citations to dsp-engine.md §5.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R s950 --no-tests=error
```
