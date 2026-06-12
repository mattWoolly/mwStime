---
id: 043
title: WaveformView — peaks cache, drop zone, stretch-zone handles, A/B overlay, FX scope
status: in-review
depends-on: [039]
component: ui
estimated-size: M
---

## Objective
The waveform region: cached-peak rendering of the loaded sample, drag-and-drop target,
draggable stretch-zone handles, original-vs-render A/B overlay, and the live input
scope used in FX mode.

## Context
Read first:
- docs/design/ui-design.md §2 WaveformView row (cached peaks, zone handles,
  render-overlay A/B, live input scope), §1 (region in mockup), §6.1 (drop flow),
  §6.3/§6.4 (A/B + FX scope behavior)
- docs/design/architecture.md §4 (message-thread component reads published buffers
  via shared_ptr copy; meter/scope data via lock-free FIFO → timer poll)
- Faceplate geometry (039)

## Scope
- `plugin/ui/WaveformView.{h,cpp}`:
  - peaks cache built off the published SourceSample/RenderedSample
    (min/max per pixel column, recomputed on buffer change/resize on a background
    call or time-sliced on the message thread — never the audio thread),
  - `juce::FileDragAndDropTarget`: hover highlight ("DROP SAMPLE HERE" idle text),
    accept wav/aiff/flac, callback to the file-load path (wired in 045b),
  - stretch-zone handles: two draggable edges mapping to zoneStart/zoneEnd with
    pixel↔sample conversion; emits zone-change events,
  - A/B overlay: draw original and render peak layers, toggle which is prominent,
  - play head cursor during audition; FX mode: rolling input scope fed by a
    lock-free FIFO from the processor (decimated), timer-polled,
  - drag-out gesture initiation (delegates to ExportService — hook only, wired 045b),
  - click-to-audition: a plain click on the waveform (not on a zone handle, not a
    drag) emits an audition event — "audition is always available from the UI (PLAY
    soft key / waveform click)", architecture.md §7; wired to SamplePlayer in 045b.
- Tests (`tests/plugin/test_waveformview.cpp`, headless logic parts): pixel↔sample
  zone math at several widths/zooms; peaks cache correctness on a known ramp buffer;
  drop-file filter accepts/rejects extensions; click vs handle-drag disambiguation
  emits the right event.

## Out of scope
- Actual wiring to FileLoader/ExportService/processor FIFO (045/045b).
- Zoom UI beyond fit-to-width (v1 keeps it simple; zone handles only).

## Acceptance criteria
- [ ] Logic tests pass; view renders a loaded fixture in Standalone with handles.
- [ ] Peaks rebuild never blocks audio (no audio-thread access).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R waveformview --no-tests=error
```
