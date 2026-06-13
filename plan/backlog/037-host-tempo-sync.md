---
id: 037
title: Host tempo sync — SYNC parameter wiring, source BPM, filename guess
status: in-review
depends-on: [021, 029, 033]
component: plugin
estimated-size: S
---

## Objective
`tempoSync = HOST` works end-to-end: host BPM read each block, effective timeFactor
derived via TempoMap (automation values preserved, sync applied at the engine), source
BPM stored in state with a filename `_174bpm`-style auto-guess that is always
overridable.

## Context
Read first:
- docs/design/dsp-engine.md §2 `tempoSync` row (formula, direction-corrected; CLASSIC
  integer quantization with achieved-length LCD hint)
- docs/design/ui-design.md §6.2 step 4 (SYNC interaction: typed/tap source BPM,
  filename auto-guess, LCD `174.0 -> 87.0 = 200%`)
- TempoMap (021), state tree `sourceBPM` (029), EngineHost FX (033)

## Scope
- EngineHost additions:
  - when tempoSync==HOST in FX mode: effective T = `TempoMap::syncedTimeFactor(
    sourceBPM, hostBPM)` clamped per ADR-006 FREE/SYNC rules; the `timeFactor`
    parameter's automation value is NOT overwritten (clamp/override at the engine,
    architecture.md §6 pattern),
  - host BPM from AudioPlayHead per block; last-known-tempo retained for the fallback,
  - `sourceBPM` state field with setter API; filename guess: parse
    `_(\d+(?:\.\d+)?)bpm` case-insensitive from the loaded file name on load, only
    when the user hasn't set a value (overridable, never clobbers),
  - expose computed sync readout (source, host, resulting %) for the LCD model (041).
- Tests (`tests/plugin/test_temposync.cpp`):
  - 174 source / 87 host ⇒ engine sees 200%; automation value unchanged,
  - host BPM change mid-stream updates effective T at the next grain boundary,
  - filename `amen_174bpm.wav` ⇒ sourceBPM 174 unless previously user-set,
  - CLASSIC: effective synced factor integer-quantized; readout exposes the achieved
    value.

## Out of scope
- Tap-tempo UI (task 045b) and LCD rendering (task 041).
- SAMPLE-mode sync (FX-mode feature per dsp-engine §2).

## Acceptance criteria
- [ ] Tests pass; the documented 174→87⇒200% example is asserted verbatim.
- [ ] Automation values never rewritten by sync.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R temposync --no-tests=error
```
