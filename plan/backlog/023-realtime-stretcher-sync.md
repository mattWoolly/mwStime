---
id: 023
title: RealtimeStretcher SYNC mode — window capture, boundary resync, compression+silence
status: done
depends-on: [021, 022]
component: engine
estimated-size: M
---

## Objective
SYNC mode on RealtimeStretcher: transport-aligned fxWindow capture ("stretch the last
bar"), hard resync at every window boundary, T<100% allowed as
compressed-then-silence, and the no-transport wall-clock fallback.

## Context
Read first:
- plan/decisions/006-fx-vs-sample-mode.md (SYNC column + no-transport term)
- docs/design/architecture.md §5.2 (SYNC semantics, WINDOW 1/4…8 bars default 1 bar)
- docs/design/dsp-engine.md §3.5 (SYNC bullet), §2 `fxWindow` row
- docs/design/testing-strategy.md §3 items 4c(SYNC), 4d(SYNC) — tests FIRST
- TempoMap (021), RealtimeStretcher FREE (022)

TDD: write the SYNC contract tests first, driven by a scripted fake transport.

## Scope
- Extend `RealtimeStretcher`:
  - `setTransport(TransportInfo{playing, ppq, bpm, timeSig})` called per block (plain
    data, host glue in 033/037),
  - SYNC ON: read head hard-resyncs to the window start at every transport-aligned
    window boundary (boundaries from TempoMap); window length follows fxWindow,
  - T<100% SYNC: captured window plays compressed, then **silence to the window
    boundary** (PI),
  - tempo changes mid-play: next boundary recomputed from the new tempo (host smoke
    expectation, testing-strategy §6 REAPER row),
  - no transport / not playing: wall-clock window via TempoMap fallback (last-known
    tempo or 120 BPM),
  - resyncs occur only at window boundaries (never mid-window except FREE-rule
    exhaustion, which SYNC supersedes).
- Tests written first (`tests/unit/test_rtstretch_sync.cpp`):
  - scripted transport at 120 BPM, 1-bar window, T=200%: resync events land exactly
    on bar boundaries (instrumented schedule hook); output between boundaries is the
    stretched window content,
  - T=50% SYNC: first half of the window is the compressed capture, the rest is
    exactly silence to the boundary,
  - tempo change 120→90 BPM mid-stream: subsequent boundary spacing matches 90 BPM,
  - transport absent: boundaries spaced at the 120 BPM wall-clock fallback,
  - window size sweep 1/4…8 bars: boundary spacing scales correctly.

## Out of scope
- Reading real host transports (JUCE) — tasks 033/037.
- Loop-fill alternative for compression (noted later option, not v1).

## Acceptance criteria
- [ ] Contract tests written first; tag `[rtstretch][sync]`; all pass.
- [ ] FREE-mode tests from 022 still pass unchanged.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R rtstretch --no-tests=error
```
