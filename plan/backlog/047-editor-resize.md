---
id: 047
title: Resizable editor — fixed aspect 0.6×–2.0×, scale persisted in state
status: done
depends-on: [029, 045b]
component: ui
estimated-size: S
---

## Objective
The editor resizes from the 1000×380 base with fixed aspect ratio between 0.6× and
2.0×, all vector layers re-cache per scale, and the scale factor persists in plugin
state across sessions.

## Context
Read first:
- docs/design/ui-design.md §5 (sizing spec), §4 (per-scale layer caching)
- docs/design/architecture.md §6 (UI state: zoom/scale factor in the state tree)
- plan/decisions/005-ui-rendering-approach.md (resolution independence)
- Editor assembly + interactions (045/045b), state tree (029)

## Scope
- `setResizable(true, true)` + `ResizableCornerComponent`; fixed aspect via
  `getConstrainer()->setFixedAspectRatio(1000.0/380.0)`; min/max 600×228 / 2000×760.
- Resize → re-derive the geometry constants scale → Faceplate (and other cached
  layers) re-render at the new scale; child bounds proportional.
- Scale factor written to the state tree on resize end; restored on editor open.
- Hamburger menu fully populated per ui-design §1 region 1: "about" (version,
  AGPLv3 notice, credits), "manual" (opens the project manual/URL), and "scale"
  entries (75/100/150/200%).
- Tests (`tests/plugin/test_resize.cpp`): constrainer enforces aspect + limits; scale
  round-trips through getState/setState; reopening an editor restores the size.
- Manual: visual check at 0.6× and 2.0× on a HiDPI display (crispness — vector), PR
  screenshots.

## Out of scope
- Per-platform DPI quirks beyond JUCE defaults (QA-phase UI adversaries cover
  0.6×/2.0× on Linux X11).

## Acceptance criteria
- [ ] Tests pass; aspect locked; limits enforced.
- [ ] Scale survives host session save/reload.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R resize --no-tests=error
```
