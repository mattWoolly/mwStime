---
id: 056
title: Fix sample export — bridge completed render to the WaveformView + add Save-As fallback
status: todo
depends-on: []
component: plugin
estimated-size: M
---

## Objective
After a GO render completes in SAMPLE mode, the user can get the stretched audio OUT of
the plugin: the WaveformView shows the rendered result (A/B overlay + drag-out enabled),
the body drag-out gesture actually fires, AND a discoverable, host-independent
"Export rendered sample…" menu item writes the stretched WAV to disk. Today there is NO
working export path (user-reported on macOS).

## Context
QA/user bug: in SAMPLE mode the modified sample cannot be exported in any way. Verified
root cause (read these first):
- `plugin/src/PluginEditor.cpp` `pollEngine()` (~:340-367) drains the worker FIFO via
  `applyWorkerEvent` (`plugin/ui/LcdPageBinding.cpp:28-52`) which ONLY updates LCD
  progress/notEnoughMemory — it NEVER calls `waveform.setRenderedSample(...)`. So the
  WaveformView's `renderedSample` stays null forever.
- `plugin/ui/WaveformView.cpp:447-456` — the body `DragOut` gesture fires `onDragExport()`
  ONLY `if (!dragOutFired && hasRender())`; `hasRender()` (`WaveformView.h:209`) is
  `renderedSample != nullptr`. Permanently false ⇒ drag-out inert.
- `plugin/ui/WaveformView.cpp:281` `setRenderedSample(...)` is called ONLY from tests
  (`tests/plugin/test_waveformview.cpp:544,623`), never from PluginEditor.
- `plugin/src/PluginEditor.cpp:164` wires `waveform.onDragExport = [this]{ exportService.startDrag(waveform); }`
  — the sole export entry point. There is no Save-As/menu/button path: the hamburger menu
  (`PluginEditor.cpp:769-829`) has only About/Manual/Scale, and `ExportService::saveAs(...)`
  is never called anywhere.
- `plugin/src/ExportService.*` reads its buffer from `EngineHost::currentRender()`
  (`ExportService.cpp:107-112`), NOT from the WaveformView — so once invoked it correctly
  exports the STRETCHED render. The published render exists: `EngineHost.cpp:193/200/216/240`
  `published_.publish(...)`. The gap is purely that the editor never tells the view a render
  exists, and Save-As is unwired.
- Mirror the existing source-buffer bridge: `pollFileLoader` already does the analogous
  `waveform.setRenderedSample(...)`-style publish for the SOURCE buffer at
  `PluginEditor.cpp:312-316`.
Design refs: `plan/backlog/036-drag-out-export.md`, `plan/backlog/045b-editor-interactions.md`,
ADR-006 (FX vs sample mode). `docs/QA-REPORT.md` (this is the export gap).

## Scope
- In `PluginEditor::pollEngine` (or the worker-event drain it calls), when a render
  Finished with outcome Completed, copy the published render into the view:
  `waveform.setRenderedSample(std::make_shared<const mws::core::AudioBuffer>(host.currentRender()->audio))`
  (guard for null / zero frames). This re-enables `hasRender()` ⇒ drag-out + A/B overlay.
  Clear it when a new source is loaded / params invalidate the render, so a stale render
  can't be dragged out.
- Add a discoverable, host-independent fallback: a "Export rendered sample…" item in the
  hamburger menu (`showHamburgerMenu`) that calls `ExportService::saveAs(...)` (FileChooser
  → write the stretched render). Disable/grey the item when there is no completed render.
- Keep the drag-out gesture working (now that `hasRender()` can be true).
- Use the authentic deterministic filename policy already in ExportService.

## Out of scope
- Soft-key feedback/greying fixes (task 057).
- Changing the FX-first default or the FX-mode greying policy.
- New export formats beyond what ExportService already supports.

## Acceptance criteria
- [ ] After a completed render is published, the WaveformView reports `hasRender() == true`
      and the body drag-out gesture invokes `onDragExport` (test the real seam, not a manual
      `setRenderedSample`).
- [ ] A new source load / render-invalidation resets `hasRender()` to false.
- [ ] The hamburger menu has an enabled "Export rendered sample…" item only when a render
      exists; selecting it routes to `ExportService::saveAs` and writes the STRETCHED audio.
- [ ] New/extended test under tests/plugin proves the render→view bridge end-to-end
      (a published Finished/Completed render ⇒ `hasRender()` true) — closing the gap that
      `test_editor_wiring.cpp` left (it asserted only LCD progress, never the audio buffer).
- [ ] Full build + ctest green; no regression in existing export/waveform tests.

## Verification commands
```
cmake --preset default -DFETCHCONTENT_BASE_DIR=$HOME/.cache/mwstime-fc
cmake --build --preset default -j 6
ctest --preset default -R "export|waveform|editor" --no-tests=error
ctest --preset default
```
