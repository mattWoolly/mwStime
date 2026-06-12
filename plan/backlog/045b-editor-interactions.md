---
id: 045b
title: Editor interactions — soft-key actions, drop-in/drag-out, FX-mode greying, keyboard & accessibility
status: todo
depends-on: [036, 037, 045]
component: ui
estimated-size: M
---

## Objective
The editor's interaction flows complete (ui-design §6.1–§6.4): the eight TIME-page
soft keys execute their actions, drop-in/drag-out work, click-to-audition works, FX
mode greys/re-pages the right things, and the editor is keyboard-only operable with
accessibility handlers. Builds on the assembled shell from task 045.

## Context
Read first:
- docs/design/ui-design.md §6.1–§6.4 (the four interaction flows — this task
  completes them end-to-end), §7 (keyboard/accessibility), §1 (soft-key TIME set)
- docs/design/architecture.md §7 (audition always available from the UI: PLAY soft
  key / waveform click)
- Editor shell + field editing (045), ExportService (036), source-BPM/tap entry
  (037), sample slot/GO/ZONE preview (034 via 045), autoCycle trigger param (028),
  WaveformView events incl. click-to-audition (043), FileLoader (031 via 034)

## Scope
- Soft keys (TIME page set, wired through the 042 callback interface):
  - F1 TIME page focus; F2 autC/AUTO-D — fires the 028 `autoCycle` trigger
    parameter (engine runs the task-014 detector, result lands in cycleLen; key
    reads `AUTO-D` on S950); F3 ZONE preview toggle (034); F4 GO render request
    (034); F5 PLAY; F6 A/B; F7 SYNC — source-BPM entry, typed + tap tempo (037
    setter API; LCD readout via the 041 sync line); F8 ABORT (hold ≥ 600 ms → abort
    flag, 042 hold gesture).
- Drop-in: WaveformView drop → FileLoader; drag-out: WaveformView/floppy-slot →
  ExportService `startDrag`.
- Click-to-audition: the 043 waveform click event triggers SamplePlayer play
  (architecture.md §7).
- FX mode: GO/PLAY/A-B greyed, WINDOW selector active, waveform becomes the live
  input scope (033 FIFO), LCD shows the "TIME-STRETCH (REALTIME)" page
  (ui-design §6.4).
- Keyboard-only operability + JUCE accessibility handlers (screen-reader names =
  LCD field labels, ui-design §7).
- Tests (`tests/plugin/test_editor_actions.cpp`, headless where possible): soft-key
  enable/disable matrix per mode; ABORT fires only after the hold; F2 writes
  cycleLen via the trigger parameter; tap-tempo averaging math; drop-file path
  invokes the loader; click-to-audition triggers play; FX mode swaps the LCD page.
- Manual flow checklist run in Standalone (load → set params → ZONE → GO → abort →
  GO → PLAY → A/B → drag-out; FX mode clamp display) — recorded in the PR.

## Out of scope
- Model-switch cross-fade/clamp-memory (046), resize (047).
- Host smoke matrix (048b/QA phase).

## Acceptance criteria
- [ ] Action tests pass; manual checklist completed and noted in PR.
- [ ] Every ui-design §6.1–§6.4 step works in Standalone on macOS.
- [ ] Keyboard-only run of the manual checklist succeeds (no mouse).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R editor --no-tests=error
```
