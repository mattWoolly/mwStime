---
id: 011
title: CyclicEngine REVISED — fractional hop with linear-interp reads
status: done
depends-on: [010]
component: dsp
estimated-size: S
---

## Objective
REVISED hop mode on the existing CyclicEngine: fractional (double) input hop, exact
timing, 2-point linear interpolation on fractional grain-start reads — the "modern
timing" option.

## Context
Read first:
- docs/design/dsp-engine.md §3.2 (REVISED line), §3.4 (REVISED notes: fractional
  B.off ⇒ linear-interp reads — the sole source of its slight pitch drift)
- docs/research/akaizer-analysis.md §2.2 (CLASSIC vs REVISED semantics)
- docs/design/testing-strategy.md §3 item 3 (REVISED timing-exactness property test)
- libs/mwstime-core CyclicEngine from task 010

TDD: write the timing-exactness test first.

## Scope
- Implement `HopMode::REVISED` in `CyclicEngine`: `hop_in = hop_out / T` (double);
  grain offsets fractional; source reads use 2-point linear interpolation; timeFactor
  accepts 0.01% steps (dsp-engine §2 table).
- Tests written first (`tests/unit/test_cyclic_revised.cpp`):
  - **timing exactness**: output length within ±1 sample of `N·T` for fractional time
    factors (e.g. 137.50%, 66.67%) across several N/C combinations,
  - CLASSIC results unchanged (re-run one CLASSIC case, compare against task-010
    expectation — guard against regression),
  - REVISED with an integer T that divides exactly matches CLASSIC output (degenerate
    equivalence where hop_in is integral).

## Out of scope
- Any change to CLASSIC arithmetic or SpliceCal semantics.

## Acceptance criteria
- [ ] Tests written first; tag `[cyclic][revised]`; all pass.
- [ ] CLASSIC property tests from task 010 still pass untouched.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R cyclic --no-tests=error
```
