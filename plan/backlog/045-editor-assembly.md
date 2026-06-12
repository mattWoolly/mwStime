---
id: 045
title: PluginEditor assembly — layout, LCD page binding, field cursor/jog/double-click editing
status: todo
depends-on: [033, 034, 040, 041, 042, 043, 044]
component: ui
estimated-size: M
---

## Objective
The assembled editor shell: all components instantiated and laid out per the mockup,
a 30 Hz UI-FIFO poll driving LcdPageModel → LcdDisplay, and the LCD field-editing
system (cursor keys move the field cursor, jog wheel edits with hardware steps,
double-click for direct text entry). Soft-key *actions*, drop-in/drag-out, FX-mode
greying, and accessibility land in task 045b.

## Context
Read first:
- docs/design/ui-design.md §1 (layout), §6.2 steps 1–2 (field focus + jog/typing
  editing), §2 (LcdPageModel as single source of truth)
- docs/design/architecture.md §4 (UI gets engine feedback via lock-free FIFO →
  timer poll; render-done/progress/LCD updates)
- Components/feeds: EngineHost FX + scope FIFO (033), sample slot/GO events (034),
  LcdDisplay (040), LcdPageModel (041), SoftKeyBar/JogWheel (042), WaveformView
  (043), ControlPanel (044)

## Scope
- `plugin/src/PluginEditor.{h,cpp}` rewrite:
  - instantiate/lay out Faceplate, LcdDisplay, SoftKeyBar, JogWheel, WaveformView,
    ModelSelector/ControlPanel per the geometry constants (039),
  - a 30 Hz (PI) timer polls the UI FIFO: progress, render-done, typed load errors,
    clamp flags, embed/path-only state → LcdPageModel refresh → LcdDisplay cells
    (the 041 page model formats everything, including the hardware-idiom error and
    embed-status lines),
  - field editing: cursor keys (SoftKeyBar cursor events) move the LCD field cursor
    using the 041 field map; jog wheel edits the focused field with hardware steps
    (fine with Shift); double-click a field for direct text entry (no numeric
    keypad — ADR-005); ENT commits,
  - soft-key captions render from the active page model; key *actions* are stubbed
    no-ops here (wired in 045b) except cursor/ENT field editing.
- Tests (`tests/plugin/test_editor_wiring.cpp`, headless where possible): field
  cursor traversal order matches the 041 field map; jog delta on timeFactor writes
  the param with the correct step (fine mode included); double-click entry
  commit/cancel round-trip; FIFO event → LCD cell refresh.
- Manual check in Standalone: assembled layout matches the §1 mockup; LCD live-edits
  params via cursor/jog/double-click — noted in the PR with a screenshot.

## Out of scope
- Soft-key actions, drop-in/drag-out, click-to-audition wiring, FX-mode greying,
  keyboard-only operation/accessibility (task 045b).
- Model-switch cross-fade/clamp-memory (046), resize (047).

## Acceptance criteria
- [ ] Wiring tests pass; layout matches the mockup in Standalone.
- [ ] LCD content comes only from LcdPageModel (no ad-hoc strings in the editor).
- [ ] Field editing (cursor/jog/double-click) works on every editable field of the
      S1000 TIME page.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R editor --no-tests=error
```
