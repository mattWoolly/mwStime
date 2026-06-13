---
id: 036
title: Drag-out export — render to WAV, drag to host/desktop
status: in-review
depends-on: [003, 034]
component: plugin
estimated-size: S
---

## Objective
The rendered sample exports as a WAV by dragging out of the plugin (to DAW timeline or
desktop), plus a fallback "save as…" path — the modern half of the
render-to-new-sample workflow.

## Context
Read first:
- docs/design/architecture.md §5.1 (data flow: RenderedSample → drag-out / save as
  WAV)
- docs/design/ui-design.md §6.3 step 4 (drag the waveform out to export), §1 (floppy
  slot easter egg: drag-out render)
- mws::core::WavIo (003 — the round-trip test reads exports back through it)
- SamplePlayer/RenderedSample (034)

## Scope
- `plugin/src/ExportService.{h,cpp}`:
  - write the published RenderedSample to a temp WAV (juce AudioFormatWriter,
    16/24-bit per source depth policy: 16-bit default (PI)); deterministic file
    naming `<sample>_<model>_<timeFactor>.wav` (hardware-flavored `*ST` suffix
    optional in the display name only, ui-design mockup),
  - `performExternalDragDropOfFiles` initiation API for the UI (component hookup in
    045b; expose a `startDrag(Component&)` entry),
  - "save as…" file chooser path (message thread, async),
  - guard: no render published ⇒ no-op with a typed event for the LCD.
- Tests (`tests/plugin/test_export.cpp`): exported WAV round-trips via WavIo and is
  bit-faithful to the RenderedSample at the chosen depth; filename pattern; no-render
  guard event.

## Out of scope
- The WaveformView/floppy-slot mouse handling (tasks 043/045b call into this).
- Export of FX-mode audio (SAMPLE renders only).

## Acceptance criteria
- [ ] Export tests pass; file content verified against the render buffer.
- [ ] Temp files cleaned up on plugin destruction.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R export --no-tests=error
```
