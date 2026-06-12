---
id: 020
title: OfflineRenderer — full authentic pipeline with norm, memory cap, progress/abort
status: todo
depends-on: [009, 011, 013, 015, 018, 019]
component: engine
estimated-size: M
---

## Objective
`mws::engine::OfflineRenderer`: the hardware-faithful render-to-new-sample — ingest
character → per-model stretch engine → transpose → playback character → optional
normalize — with the 10-minute memory cap, progress callback, and abort flag. This is
the buffer the golden tests pin.

## Context
Read first:
- docs/design/architecture.md §5.1 (the five-stage pipeline diagram + render memory
  cap + NOT ENOUGH MEMORY idiom), §4.1 (why offline exists), §6 (determinism rule)
- docs/design/dsp-engine.md §7.3 (normalization: default OFF; ON = peak-normalize to
  source peak), §1 (engine per model), §3.4 edge rules, §5/§6 (S950/S900 specifics)
- docs/design/testing-strategy.md §3 item 10 (render-cap property test — write first)
- plan/decisions/006-fx-vs-sample-mode.md (SAMPLE-mode workflow contract)
- Engines/chains: CyclicEngine (010/011), RepitchEngine (013), S950Engine (015),
  Transpose (018), CharacterChain (019), ModelSpec/ParamSnapshot (009)

TDD: write `tests/unit/test_offline_renderer.cpp` first.

## Scope
- `include/mws/engine/OfflineRenderer.h` + src:
  - `render(AudioBuffer source, ParamSnapshot, Callbacks{progress(float),
    shouldAbort()->bool}) -> RenderResult { AudioBuffer out; RenderInfo info; }`,
  - params clamped via `ModelSpec::clamp` first (single authority),
  - model dispatch: S900 → RepitchEngine; S950 → S950Engine; S1000/S1100 →
    CyclicEngine (CLASSIC/REVISED per hopMode); transpose routed per task-018 rule,
  - stereo: per-channel render with the identical shared hop schedule (dsp-engine §3
    stereo rule); S900/S950 mono-summed by CharacterChain ingest,
  - `norm == ON`: peak-normalize output to source peak (dsp-engine §7.3); OFF default,
  - **memory cap**: predicted output length (schedule-derived) > 10 min @ model rate
    per channel ⇒ refuse BEFORE allocating, returning
    `RenderError::NotEnoughMemory` (architecture.md §5.1),
  - abort: `shouldAbort()` polled at grain/stage boundaries ⇒ `RenderError::Aborted`,
  - `RenderInfo`: achieved output length, achieved (quantized) stretch ratio, engine
    version hash (from 001), monoSummed flag — feeds LCD + state metadata.
- Tests (write first):
  - end-to-end: S1000 CLASSIC C=1000 T=300% on a 1 s sine renders with the
    schedule-derived length (independent recompute, not round(N·T)),
  - cap: 2000% on a synthetic long input is refused with NotEnoughMemory and no
    large allocation (cap math unit-tested directly),
  - abort: shouldAbort()==true after first progress call ⇒ Aborted, no crash,
  - norm ON ⇒ output peak == source peak (±1e-6); OFF ⇒ untouched gain,
  - determinism: same (source, ParamSnapshot) twice ⇒ bit-identical (incl. dither
    seed fixed from params),
  - each of the four models renders without error on a 0.5 s noise burst.

## Out of scope
- The render worker thread / plugin glue (task 030).
- Golden-render corpus (task 026) — only unit/property tests here.

## Acceptance criteria
- [ ] Tests written first; tag `[renderer]`; all pass.
- [ ] Refusal happens before allocation (assert via allocation-counting test hook or
      capped predicted-length check unit test).
- [ ] RenderInfo carries everything the LCD readout needs (achieved length/ratio).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R renderer --no-tests=error
```
