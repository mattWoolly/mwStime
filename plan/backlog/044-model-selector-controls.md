---
id: 044
title: ModelSelector + right-panel controls (MODE, TIMING, CHARACTER, WINDOW, OUTPUT)
status: todo
depends-on: [028, 039]
component: ui
estimated-size: S
---

## Objective
The right-hand control panel: rack-badge model selector (four models + reserved blank
S3000 badge), FX/SAMPLE mode switch, CLASSIC/REVISED timing, CHARACTER on/off, FX
WINDOW selector, and the OUTPUT trim knob — APVTS-attached.

## Context
Read first:
- docs/design/ui-design.md §1 (panel contents in mockup), §2 ModelSelector row
  (latching rack-badge tabs, 4 + reserved slot), §3 (reserved fifth badge — ADR-004)
- docs/design/dsp-engine.md §2 (which of these are non-automatable: model,
  pluginMode, sampleRateSel; bandwidth/FS selectors live here too for S900/S950 and
  S1000/S1100 respectively)
- Parameters (028), LookAndFeel/geometry (039)

## Scope
- `plugin/ui/ModelSelector.{h,cpp}`: latching badge buttons S900/S950/S1000/S1100 +
  a blank disabled fifth badge position (ADR-004); selection writes the
  non-automatable `model` parameter; visual latching from spec accent colors.
- `plugin/ui/ControlPanel.{h,cpp}` (or individual controls):
  - MODE FX/SAMPLE radio, TIMING CLASSIC/REVISED radio, CHARACTER toggle, WINDOW
    selector (1/4–8 bars/FREE, enabled only in FX mode), OUTPUT rotary (−24…+12 dB),
  - context-dependent rows: BANDWIDTH (S900/S950 models only) and FS 44.1/22.05
    (S1000/S1100) appear per model visibility rules,
  - APVTS attachments (Button/Combo/SliderAttachment) for automatable params; manual
    listeners for non-automatables,
  - tooltips show hardware unit + normalized value (ui-design §7); accessibility
    names set.
- Test (`tests/plugin/test_controlpanel.cpp`): attachment round-trip (UI change ⇒
  param change ⇒ UI reflects), per-model visibility of BANDWIDTH/FS rows, fifth badge
  disabled.

## Out of scope
- Model-switch cross-fade/clamp-restore behavior (046).
- LCD feedback of these values (041/045).

## Acceptance criteria
- [ ] Tests pass; panel renders and operates in Standalone.
- [ ] Fifth badge present, blank, disabled (reserved slot only).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R controlpanel --no-tests=error
```
