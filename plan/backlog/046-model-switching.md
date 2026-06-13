---
id: 046
title: Model switching behavior — faceplate cross-fade, clamp memory, audio cross-fade, PDC re-report
status: done
depends-on: [029, 045b]
component: ui
estimated-size: M
---

## Objective
Switching models is complete per ui-design §6.5: FaceplateSpec/ModelSpec swap with a
150 ms palette cross-fade and LCD re-layout, out-of-range params clamped at the engine
with pre-clamp values remembered and restored, mono-sum notice, one-block FX audio
cross-fade, and a latency re-report.

## Context
Read first:
- docs/design/ui-design.md §6.5 (the spec for this task, both steps)
- docs/design/architecture.md §6 (ranges never change; clamp at engine; LCD shows
  clamped value), §5.2 (model switch = documented PDC-updating action)
- docs/design/testing-strategy.md §6 REAPER row ("latency re-report on model switch
  honored")
- State tree `clampMemory` field (029), editor assembly + interactions (045/045b),
  EngineHost (033), LcdPageModel (041), Faceplate (039)

## Scope
- Editor: model change → cross-fade cached faceplate images over 150 ms (PI) →
  LcdDisplay layout swap (S950_2LINE ↔ S1000_PAGE) → LcdPageModel rebuild; S950/S900
  selection shows the mono-sum notice line.
- Pre-clamp memory: per-model map of user-entered values; switching to a model that
  clamps (e.g. timeFactor 1500 → S950 shows 999) remembers 1500 and restores it when
  switching back (ui-design §6.5 (PI)). Session persistence of the map is a further
  **(PI)** extension: store it in the task-029 `clampMemory` state-tree field (the
  schema is owned by 029 — do not invent new tree fields here).
- Processor: model switch in FX mode cross-fades old/new engine output over one block
  (PI), reconfigures via the task-030 publication path, recomputes + re-reports
  latency.
- Tests:
  - `tests/plugin/test_modelswitch.cpp`: clamp-memory round-trip (S1000 T=1500 →
    S950 → back ⇒ 1500 restored; LCD showed 999 meanwhile via LcdPageModel);
    latency re-report observed on switch; state tree carries the memory map,
  - audio: one processBlock containing a switch produces no discontinuity > −40 dBFS
    step (cross-fade present), no allocation on the audio thread.
- Manual: switch through all four models in Standalone; screenshot each faceplate
  (PR attachment).

## Out of scope
- S3000 (badge stays disabled — ADR-004).
- Host PDC behavior itself (documented inconsistency; QA-phase smoke).

## Acceptance criteria
- [ ] Tests pass; all four faceplates render with correct palettes and LCD layouts.
- [ ] Clamp memory survives save/reload.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R modelswitch --no-tests=error
```
