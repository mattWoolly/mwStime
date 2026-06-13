---
id: 038
title: Factory presets — the four documented v1 validation presets
status: done
depends-on: [026, 029, 034]
component: plugin
estimated-size: S
---

## Objective
The four dsp-engine §9 presets ship as selectable factory presets ("Jungle Amen 300",
"S950 vocal 200", "S900 half-speed", "Dred vox"), implemented via the JUCE program
API + state layer, matching the golden-test cases parameter-for-parameter.

## Context
Read first:
- docs/design/dsp-engine.md §9 (the preset table — params + citations; S3000 preset
  explicitly v1.1, do NOT add)
- docs/design/architecture.md §6 (state model the presets write through)
- tests/golden/cases.json (task 026) — preset cases must stay in sync
- State (029), sample slot (034 — presets set params, never load audio)

## Scope
- `plugin/src/state/FactoryPresets.{h,cpp}`:
  - preset definitions as data (name → ParamSnapshot deltas + model + mode), exactly
    the §9 table: S1100 CYCLIC C1000 T300% (qual 20/width 10 stored but inert);
    S950 STRETCH 200 D-TIME 1000 POL2; S900 timeFactor 200 BW max; S1000 CYCLIC
    C200 T400%,
  - `getNumPrograms`/`setCurrentProgram`/`getProgramName` wired on the processor
    (hosts expect ≥ 1 program; keep a "Default" program 0),
  - preset selection updates APVTS + non-param state atomically (no partial states).
- Tests (`tests/plugin/test_presets.cpp`):
  - each preset's resulting ParamSnapshot equals the §9 values (assert per field),
  - preset params match the corresponding `tests/golden/cases.json` case entries
    (read the JSON in-test — drift between presets and goldens fails the build),
  - program API round-trip (set program → getState → setState → program restored).

## Out of scope
- User preset save/load browser (not in v1 design docs).
- S3000 factory default (arrives with the model, ADR-004).

## Acceptance criteria
- [ ] Tests pass incl. the cases.json cross-check.
- [ ] Preset names/values match dsp-engine §9 verbatim.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R presets --no-tests=error
```
