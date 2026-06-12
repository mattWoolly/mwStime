---
id: 021
title: TempoMap — tempo-synced time factor + window boundary math
status: todo
depends-on: [009]
component: engine
estimated-size: S
---

## Objective
`mws::engine::TempoMap`: pure, host-agnostic math for tempo sync — the
`timeFactor = 100 × sourceBPM / hostBPM` mapping (direction panel-corrected), CLASSIC
integer quantization of the synced factor, and transport-aligned fxWindow boundary
computation with the no-transport fallback.

## Context
Read first:
- docs/design/dsp-engine.md §2 `tempoSync` row (formula + direction correction +
  CLASSIC quantization note) and `fxWindow` row
- docs/design/architecture.md §5.2 (SYNC window semantics; no-transport fallback:
  wall-clock window from last-known tempo or 120 BPM (PI))
- plan/decisions/006-fx-vs-sample-mode.md (window/resync contract)

TDD: write `tests/unit/test_tempomap.cpp` first.

## Scope
- `include/mws/engine/TempoMap.h` + src:
  - `syncedTimeFactor(sourceBPM, hostBPM) -> double` (= 100 × source/host),
  - `quantizeClassic(double pct) -> int` + achieved-length hint helper (LCD shows the
    achieved value — ui-design §6.2),
  - `windowBoundary(ppqPosition, bpm, timeSigNumerator, timeSigDenominator,
    WindowSize) -> {windowStartPpq, samplesToNextBoundary(hostRate)}` for 1/4…8-bar
    windows (bar length needs the numerator — 3/4 vs 4/4 differ),
  - fallback: `windowFromWallClock(lastKnownBpm /*or 120*/, hostRate, WindowSize)`.
- Tests (write first):
  - 174 BPM source into an 87 BPM host ⇒ 200% (the documented example — longer),
  - 174 → 174 ⇒ 100%; 87 → 174 ⇒ 50% (compression — FX clamping handled elsewhere),
  - boundary math: at 120 BPM 4/4, a 1-bar window is exactly 2 s of host samples;
    boundaries land on bar lines for non-zero transport offsets,
  - non-4/4: at 120 BPM 3/4, a 1-bar window is exactly 1.5 s of host samples,
  - no-transport fallback uses 120 BPM when no tempo was ever seen.

## Out of scope
- Reading the host transport (JUCE AudioPlayHead — task 037).
- The RealtimeStretcher resync logic itself (task 023 consumes this math).

## Acceptance criteria
- [ ] Tests written first; tag `[tempomap]`; all pass.
- [ ] Pure C++20, no JUCE.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R tempomap --no-tests=error
```
