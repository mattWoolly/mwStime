---
id: 028
title: APVTS parameter layout — superset ranges, hardware-unit strings, non-automatables
status: todo
depends-on: [009, 027]
component: plugin
estimated-size: M
---

## Objective
The full `juce::AudioProcessorValueTreeState` layout implementing dsp-engine.md §2:
fixed superset ranges (never remapped at runtime), hardware-unit string conversions,
correct non-automatable set, and a ParamSnapshot bridge for the audio thread.

## Context
Read first:
- docs/design/dsp-engine.md §2 (THE parameter table — IDs, ranges, defaults, units,
  steps, automatability; implement it row-by-row)
- docs/design/architecture.md §6 (fixed superset ranges rationale — VST3 host
  parameter-info caching; engine clamps; normalized 0–1 host exposure), §4 (audio
  thread snapshots params via plain atomic loads)
- mws ParamSnapshot/ModelSpec (009), plugin skeleton (027)

## Scope
- Create `tests/plugin/CMakeLists.txt`: a JUCE-linked Catch2 test target (plugin
  code compiled in or linked as an object/static helper lib) registered with ctest —
  this is the harness every later `tests/plugin/` task reuses (no other task creates
  it; task 001 only created `tests/unit`).
- `plugin/src/state/Parameters.{h,cpp}`:
  - `createParameterLayout()` with every §2 row: `timeFactor` 25.00–2000.00 default
    100 (step 0.01 — CLASSIC integer coercion happens at the engine), `cycleLen`
    20–2000 default 1000, `stretchMode` (INTELL choice present but the parameter is
    constrained/greyed at v1 — selecting it is prevented at the UI layer, value
    reserved), `hopMode`, `transpose` ±24.00 step 0.01, `material`, `bandwidth`
    3.0–19.2 step 0.1, `character`, `norm`, `tempoSync`, `fxWindow`, `outTrim`
    −24…+12 step 0.1; `qual`/`width` 1–99 default 10 (inert/greyed at v1);
    `autoCycle` (dsp-engine §2 trigger row): a momentary bool parameter that fires
    the auto-cycle detection and self-resets after the engine consumes it — the
    F2 autC/AUTO-D soft key writes it (045b), so "every §2 row covered" holds,
  - non-automatable (properties/choice attributes per JUCE 8): `model`, `pluginMode`,
    `sampleRateSel`, `embedAudio`, `bandwidth` non-automatable in FX (dsp-engine §2
    bandwidth row: it changes latency — make bandwidth non-automatable globally,
    documented),
  - String conversions: hardware units exactly as the LCD shows them ("300%",
    "1000", "+12.00 st", "19.2kHz", "1 BAR"),
  - `makeSnapshot(apvts) -> mws::ParamSnapshot` — lock-free reads of cached
    `std::atomic<float>*` raw parameter pointers; no ValueTree access on the audio
    thread (architecture.md §4).
- Tests (`tests/plugin/test_parameters.cpp`, JUCE unit-test harness or Catch2 with
  JUCE linked): every ID exists; ranges/defaults match §2; non-automatable set
  correct; string round-trips for representative values; snapshot reflects parameter
  changes.

## Out of scope
- Engine clamping (009 owns it; LCD clamp feedback is task 041).
- Non-parameter state tree (task 029); editor attachments (task 045).

## Acceptance criteria
- [ ] Parameter tests pass; every dsp-engine §2 row is covered by an assertion.
- [ ] Host-facing ranges are the superset; nothing remaps ranges at runtime.
- [ ] `makeSnapshot` does no allocation/locking (review criterion).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R parameters --no-tests=error
```
