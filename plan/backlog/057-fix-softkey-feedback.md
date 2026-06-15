---
id: 057
title: Fix soft keys appearing dead — visible disabled state, live-key feedback, real-click test
status: todo
depends-on: []
component: ui
estimated-size: M
---

## Objective
The F1–F8 soft keys visibly respond to clicks. Mode-disabled keys are obviously greyed
(so the user sees they are mode-gated, not broken), and every always-live key gives
immediate visible/audible feedback on a single click. A test drives REAL clicks on the
soft keys in both FX and SAMPLE modes, closing the gap that let this ship looking dead.

## Context
User bug (macOS): "I couldn't get the function buttons to work." Verified findings
(read these first):
- The click plumbing is mechanically CORRECT and must NOT be rewired: callbacks connected
  at `plugin/src/PluginEditor.cpp:61-91`; `onSoftKey`→`handleSoftKey` dispatch at
  `PluginEditor.cpp:544-567`; `SoftKeyBar` is added/visible/on-top with correct
  `setInterceptsMouseClicks(false,true)` (`SoftKeyBar.cpp:56,79`), bounds in `resized()`
  (`PluginEditor.cpp:696-701`), click→`onStateChange`→`pressKey`→`onSoftKey`
  (`SoftKeyBar.cpp:71-77,157-177`). getComponentAt resolves to each F-key.
- Why it LOOKS dead on a fresh instance (FX mode default, `Parameters.cpp:131-133` MODE=0):
  `mws::ui::softKeyEnabled` (`plugin/ui/EditorActions.cpp:69-96`) disables F3 ZONE / F4 GO /
  F5 PLAY / F6 A-B in FX mode (`SoftKeyBar::pressKey` early-returns for disabled keys,
  `SoftKeyBar.cpp:159`). The disabled keys are NOT visibly greyed, so they look broken.
- The remaining live keys give no single-click feedback: F1 TIME only `grabKeyboardFocus()`
  (`PluginEditor.cpp:548`); F2 autC with no sample writes a fallback cycleLen silently
  (`PluginEditor.cpp:569-595`); F7 SYNC tap-tempo needs ≥2 taps (`EditorActions.cpp:102-118`);
  F8 ABORT is a 600 ms hold by design (`SoftKeyBar.cpp:84,163-177`).
- Test gap: NO test compiles the real `PluginEditor` or drives a real mouse click;
  `test_inputcluster.cpp`/`test_editor_actions.cpp` call `pressKey(i)` directly, bypassing
  the visible-button path and the default-state enablement.
Design refs: `plan/backlog/042-softkeys-jog-cursor.md`, `plan/backlog/045b-editor-interactions.md`
(F1 TIME page · F2 autC · F3 ZONE · F4 GO · F5 PLAY · F6 A/B · F7 SYNC · F8 ABORT-hold),
`docs/design/ui-design.md` §6.4 (FX greys GO/PLAY/A-B/ZONE — this greying is INTENDED).

## Scope
- VISIBLE DISABLED STATE: render mode-disabled soft keys clearly greyed/dimmed (LookAndFeel
  or SoftKeyBar paint), driven by `softKeyEnabled(...)`, so users see they are mode-gated.
  Keep them disabled (do not enable sample-only actions in FX) — just make the state legible.
- LIVE-KEY FEEDBACK (visible/audible on a single press; do NOT change the action semantics
  that are correct):
  - F1 TIME: in addition to focus, navigate/refresh the LCD to the TIME (stretch) page so a
    press has a visible result.
  - F2 autC with no sample loaded: show a brief LCD hint (e.g. authentic-style
    "** LOAD SAMPLE **" notice) instead of silently writing a fallback.
  - F7 SYNC: show tap-tempo progress/feedback on the LCD on each tap (so one tap visibly
    registers even before the 2nd).
  - Keep F8 as a 600 ms hold but give a visible "holding…/ABORT" affordance during the hold.
- DISCOVERABILITY: ensure the FX/SAMPLE MODE toggle (`ControlPanel.cpp:73-99`) and the fact
  that GO/PLAY/ZONE/A-B are SAMPLE-mode keys is communicated (greyed look + an LCD hint when
  a greyed key is pressed in FX mode is acceptable).
- CLOSE THE TEST GAP: add a test that assembles the real soft-key path and drives REAL clicks
  (via the button's mouse path / getComponentAt, not direct `pressKey`) for F1–F8 in BOTH FX
  and SAMPLE modes — asserting enabled keys fire their action exactly once and disabled keys
  are visibly disabled and emit nothing. If compiling the full PluginEditor into the test is
  too heavy, assert at the SoftKeyBar+EditorActions+LcdPageModel level that (a) disabled keys
  carry the greyed style and (b) F1/F2/F7 produce their LCD feedback.

## Out of scope
- Export/render bridge (task 056).
- Implementing INTELL or changing the FX-first default (FX-first is locked, ADR-006).
- Rewiring the click path (it works) — this is feedback/legibility + tests only.

## Acceptance criteria
- [ ] Mode-disabled soft keys are visibly greyed (assert the disabled style in a test).
- [ ] F1 press makes the LCD show the TIME page; F2 with no sample shows the load hint;
      F7 tap shows tap feedback — each asserted by a test.
- [ ] A test drives real clicks (not direct `pressKey`) over F1–F8 in FX and SAMPLE modes:
      enabled keys fire once, disabled keys fire nothing and are visibly disabled.
- [ ] Existing `[inputcluster]`/`[editor]` tests still pass; full build + ctest green.

## Verification commands
```
cmake --preset default -DFETCHCONTENT_BASE_DIR=$HOME/.cache/mwstime-fc
cmake --build --preset default -j 6
ctest --preset default -R "inputcluster|editor|softkey|lcd" --no-tests=error
ctest --preset default
```
