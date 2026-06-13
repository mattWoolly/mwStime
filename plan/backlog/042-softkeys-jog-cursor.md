---
id: 042
title: SoftKeyBar (F1–F8 + cursor/ENT) and JogWheel components
status: in-review
depends-on: [039]
component: ui
estimated-size: M
---

## Objective
The hardware input cluster: an 8-key relabelable `SoftKeyBar` with cursor/ENT keys
(including hold-to-trigger for F8 ABORT), and an endless velocity-sensitive `JogWheel`
slider — both pure vector, styled by SeriesLookAndFeel.

## Context
Read first:
- docs/design/ui-design.md §1 (key row + jog/cursor cluster in the mockup), §2
  (SoftKeyBar and JogWheel rows: 8 DrawableButtons, labels from active page model;
  rotary endless, concentric ring + dimple, velocity-sensitive, Shift fine mode), §3
  region rule, §6.3 (hold F8 ≥ 600 ms (PI) abort), §7 (keyboard mirroring)
- Faceplate geometry constants (039)

## Scope
- `plugin/ui/SoftKeyBar.{h,cpp}`:
  - 8 buttons with caption labels settable at runtime (page model drives them later);
    disabled/greyed visual state (FX mode greys GO/PLAY/A-B, ui-design §6.4),
  - hold-gesture support: a key can be configured `requiresHold(ms)` — fires only
    after continuous press ≥ 600 ms with a visual progress cue (F8 ABORT),
  - cursor cluster (◄ ► ▲ ▼ + ENT) as part of the bar component; emits navigation
    events; keyboard arrows/Enter mirror them (focus handling, ui-design §7),
  - callback interface (`onSoftKey(index)`, `onCursor(dir)`, `onEnter()`) — no
    business logic inside the component.
- `plugin/ui/JogWheel.{h,cpp}`: endless rotary `juce::Slider` subclass or Component:
  drag-rotate + mouse-wheel; velocity-sensitive delta; Shift = fine mode; emits
  `onDelta(int steps, bool fine)`; Up/Down keys mirror (ui-design §7); drawn ring +
  dimple rotates with interaction.
- Headless tests (`tests/plugin/test_inputcluster.cpp`): hold-gesture timing (fake
  clock: 599 ms no fire, 600 ms fires); delta accumulation incl. fine mode scaling;
  disabled keys emit nothing.
- Accessibility: buttons/wheel have accessibility names (screen-reader labels,
  ui-design §7).

## Out of scope
- Binding keys to actual pages/actions (045b), LCD field focus logic (045).

## Acceptance criteria
- [ ] Tests pass; components render in Standalone in the mockup positions.
- [ ] No images; all drawing vector via SeriesLookAndFeel.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R inputcluster --no-tests=error
```
