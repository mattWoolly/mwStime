---
id: 039
title: SeriesLookAndFeel + FaceplateSpec + Faceplate component (cached vector chassis)
status: in-review
depends-on: [027]
component: ui
estimated-size: M
---

## Objective
The skeuomorphic foundation: `FaceplateSpec` data for the four v1 models (+ reserved
S3000 slot), a pure-vector `SeriesLookAndFeel`, and the `Faceplate` component drawing
the chassis/header/legends from spec, cached to an image per scale.

## Context
Read first:
- docs/design/ui-design.md §1 (layout + regions), §3 (FaceplateSpec struct + palette
  table — implement these colors verbatim), §4 (rendering approach: pure vector, layer
  caching, < 2 ms repaint budget, no OpenGL)
- plan/decisions/005-ui-rendering-approach.md (clean-room rule: no photo assets, no
  Akai logo, wordmarks as plain text)
- plan/decisions/004-fifth-model.md (fifth faceplate badge position reserved)
- Plugin skeleton (027)

## Scope
- `plugin/ui/FaceplateSpec.h`: the struct exactly as ui-design §3 (id, palette,
  wordmark, lcdLayout, showsModeRow, showsQualWidth, ParamVisibility) + a constexpr
  table of the four v1 specs with the §3 palette hex values; S3000 slot present but
  flagged not-shipping.
- `plugin/ui/lookandfeel/SeriesLookAndFeel.{h,cpp}`: base colors, button/knob/slider
  drawing primitives in period style (rounded-rect key caps, legend font choices —
  stock sans-serif, no font files).
- `plugin/ui/Faceplate.{h,cpp}`: draws chassis, edge bevel, header strip (power LED,
  product name, model wordmark, hamburger placeholder), screws/vents flavor, region
  frames for LCD/softkeys/waveform/selector per the §1 mockup geometry (1000×380
  base; geometry constants in one header so child components align); static layers
  cached to `juce::Image` per scale, redrawn only on resize/model change.
- A `tests/plugin/test_faceplatespec.cpp` headless test: four shipping specs present,
  palettes match the documented hex values, geometry constants sane (regions inside
  canvas, non-overlapping).
- Standalone visual check: screenshot at 1.0 scale attached to the PR.

## Out of scope
- LCD, soft keys, jog wheel, waveform, selector components (040–044).
- Model-switch cross-fade behavior (046). Screenshot regression harness (QA phase).

## Acceptance criteria
- [ ] Plugin builds; Standalone shows the S1000 faceplate at 1000×380.
- [ ] Spec table test passes; zero raster image assets in the repo.
- [ ] Repaint of cached static layer < 2 ms at 1.0 scale (log a timing in debug,
      note measurement in PR).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R faceplatespec --no-tests=error
```
