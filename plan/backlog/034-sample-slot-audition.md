---
id: 034
title: SAMPLE mode — sample slot, GO render flow, SamplePlayer, zone preview
status: in-review
depends-on: [022, 028, 029, 030, 031]
component: plugin
estimated-size: M
---

## Objective
The authentic SAMPLE-mode pipeline works headlessly: loaded sample → GO request →
worker render with progress/abort/memory-cap → published RenderedSample → SamplePlayer
playback (PLAY / A-B source-vs-render), plus the ZONE live preview through
RealtimeStretcher.

## Context
Read first:
- docs/design/architecture.md §5.1 (SAMPLE-mode data flow diagram), §4 (SamplePlayer
  on the audio thread)
- plan/decisions/006-fx-vs-sample-mode.md (SAMPLE-mode workflow contract: GO,
  progress, hold-F8 abort, memory-cap refusal, audition/A-B)
- docs/design/ui-design.md §6.3 (audition/render flow — implement the engine side;
  soft keys arrive later)
- RealtimeStretcher (022 — ZONE preview is "a real-time preview through
  RealtimeStretcher", ui-design §6.3), worker/publication (030), FileLoader (031),
  Parameters (028)

## Scope
- `plugin/src/SamplePlayer.{h,cpp}`:
  - audio-thread playback of the published RenderedSample (copy-once shared_ptr per
    block, task-030 protocol); start/stop; play original (A) vs render (B) toggle
    state; fixed-rate playback at host rate via the core resampler tables
    (allocation-free),
  - stretch-zone state (start/end samples) lives in the state tree
    (`zoneStart`/`zoneEnd` fields owned by task 029's schema),
  - apply `outTrim` to SAMPLE playback output (dsp-engine §2: outTrim applies to
    "all", not just FX),
- EngineHost SAMPLE half:
  - GO: build ParamSnapshot + zone slice → render request to the worker; progress and
    NotEnoughMemory/Aborted events surfaced to the UI FIFO,
  - ZONE preview: loop the selected zone through RealtimeStretcher at current params
    (CYCLIC models only, ui-design §6.3) — toggled on/off,
  - mode switching FX↔SAMPLE swaps process paths click-free (short fade, one block)
    **(PI** — the design specifies a one-block cross-fade only for *model* switches
    (ui-design §6.5); applying it to mode switches is our invention; keep the fade
    length a named tunable and note it for the PI audit, task 034b**)**.
- Tests (`tests/plugin/test_sample_mode.cpp`):
  - load fixture → GO → wait worker → published render length matches OfflineRenderer
    direct call (same snapshot) bit-exactly,
  - abort mid-render leaves the previous render in place,
  - cap refusal surfaces the typed event,
  - A/B toggle switches buffers between blocks without touching the audio thread
    illegally (publication protocol respected),
  - zone preview produces stretched output of the zone (spot-check splice-comb
    presence vs dry),
  - outTrim −6 dB scales SAMPLE playback by ×0.501 (±1e-3).

## Out of scope
- MIDI-triggered audition (035), drag-out export (036), all UI.

## Acceptance criteria
- [ ] Headless GO→render→play round-trip test passes and is deterministic.
- [ ] No allocation on the audio thread in SamplePlayer/preview paths.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R sample_mode --no-tests=error
```
