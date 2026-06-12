---
id: 029
title: Non-parameter state tree, stateVersion, migrations scaffolding
status: todo
depends-on: [028]
component: plugin
estimated-size: S
---

## Objective
The non-parameter ValueTree state under the APVTS root — mode, sourceFile
(path + content hash), embedAudio flag, render metadata, UI state — with an explicit
`stateVersion` and a migrations mechanism, round-trip tested.

## Context
Read first:
- docs/design/architecture.md §6 (state model — every field listed there; versioning
  rule; determinism rule for re-render-on-load)
- docs/design/dsp-engine.md §2 (`embedAudio` default ON ≤ 16 MB encoded)
- Parameters layer (028)

## Scope
- `plugin/src/state/` additions:
  - state tree schema: `mode`, `sourceFile` {path, contentHash}, `embedAudio`
    (default on), render metadata {engineVersionHash, paramsUsed}, UI state
    {scaleFactor, lcdPage}, `sourceBPM` (consumed by task 037), `zoneStart`/`zoneEnd`
    (stretch zone, consumed by task 034), `clampMemory` per-model pre-clamp value map
    (consumed by task 046; persisting it is **(PI)** — an extension beyond the
    architecture.md §6 field list, owned here so the schema has one authority),
    `stateVersion` = 1,
  - `Migrations.{h,cpp}`: `migrate(ValueTree, fromVersion) -> ValueTree` with explicit
    per-version functions; version 1 ⇒ identity; unknown future version ⇒ safe
    defaults + flag,
  - `getStateInformation`/`setStateInformation` serialize APVTS + state tree
    (audio blob embedding is task 032 — leave a clearly named extension point),
  - re-render-on-load hook point: on setState, if render metadata exists, request a
    deterministic re-render rather than storing rendered output (stub the request —
    worker arrives in 030; keep it a no-op callback here).
- Tests (`tests/plugin/test_state.cpp`): full round-trip of params + state tree;
  stateVersion written/read; migration function dispatch (fake v0 tree upgraded);
  defaults when fields missing.

## Out of scope
- FLAC blob caching/embedding (task 032).
- Editor scale persistence wiring (task 047).

## Acceptance criteria
- [ ] Round-trip and migration tests pass.
- [ ] stateVersion present in serialized state; migrations are explicit functions in
      `plugin/src/state/Migrations.cpp` (architecture.md §6).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R state --no-tests=error
```
