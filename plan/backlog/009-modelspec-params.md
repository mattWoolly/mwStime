---
id: 009
title: mws::model — ModelId, ModelSpec tables, ParamSnapshot + engine-side clamping
status: done
depends-on: [001]
component: dsp
estimated-size: M
---

## Objective
The data layer that makes models "data, not code": `ModelId` (with the reserved S3000
slot), per-model `ModelSpec` tables (ranges/defaults/character config), and the POD
`ParamSnapshot` validated/clamped per ModelSpec.

## Context
Read first:
- docs/design/dsp-engine.md §2 (the full unified parameter table — ranges, defaults,
  units, per-model applicability; this table IS the spec), §1 (engine inventory)
- docs/design/architecture.md §2.1 (`model/ModelId.h`, `model/ModelSpec.h`,
  `engine/Params.h`), §6 (fixed superset ranges; engine clamps; LCD shows clamped value)
- plan/decisions/004-fifth-model.md (S3000 enum slot reserved, NO behavior at v1)
- plan/decisions/003-s900-mode-semantics.md (S900 stretch-only params inert)
- docs/design/testing-strategy.md §3 item 7 (model-clamping property test)

TDD: write `tests/unit/test_modelspec.cpp` first.

## Scope
- `include/mws/model/ModelId.h`: `enum class ModelId { S900, S950, S1000, S1100,
  S3000 /* reserved, v1.1 — ADR-004 */ }` + name strings.
- `include/mws/engine/Params.h`: `ParamSnapshot` POD mirroring the dsp-engine.md §2
  table (timeFactor, cycleLen, stretchMode, hopMode, transpose, qual, width,
  material, bandwidth, sampleRateSel, character, norm, tempoSync, fxWindow, outTrim,
  model, pluginMode). `qual`/`width` are int 1–99, **inert at v1** (INTELL deferred —
  dsp-engine §2/§4, ADR-001) but must exist so presets/state round-trip them (the §9
  "Jungle Amen 300" preset stores qual 20 / width 10 inert).
  Plain trivially-copyable struct (audio-thread snapshot, architecture.md §4).
- `include/mws/model/ModelSpec.h` + src: per-model table — superset range, engine
  clamp range, defaults, model rate rules (S900 rate ≤ 40 kHz ⇒ BW ≤ 16.0; S950
  rate = 2.5 × bandwidth; S1000/S1100 FS 44.1/22.05), mono-sum flag (S900/S950),
  which params are inert per model, character-chain selector.
- `ModelSpec::clamp(ParamSnapshot) -> ParamSnapshot` — pure function, the single
  clamping authority used by engines, LCD model, and FX.
- Tests (write first):
  - S950: timeFactor 1500 in ⇒ 999 out; superset range untouched,
  - S900: stretch-only params flagged inert; clamp leaves them unchanged,
  - S900 bandwidth clamps at 16.0; S950 allows 19.2,
  - defaults match the §2 table exactly (timeFactor 100, cycleLen 1000, CLASSIC,
    qual 10, width 10, character ON, norm OFF, model S1000, pluginMode FX,
    fxWindow 1 bar),
  - S3000 id exists but `ModelSpec::isShipping(S3000) == false`.

## Out of scope
- APVTS / host parameter exposure (task 028).
- CharacterChain implementation (tasks 016/017/019).
- Any S3000 ModelSpec behavior beyond the reserved id (v1.1, ADR-004).

## Acceptance criteria
- [ ] Tests written first; tag `[modelspec]`; all pass.
- [ ] Every default/range in the test matches dsp-engine.md §2 (review against table).
- [ ] Pure C++20; no JUCE.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R modelspec --no-tests=error
```
