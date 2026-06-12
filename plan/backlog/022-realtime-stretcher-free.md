---
id: 022
title: RealtimeStretcher FREE mode — history ring, latency formula, T=100% null, clamps
status: todo
depends-on: [009, 011, 012, 013, 019]
component: engine
estimated-size: M
---

## Objective
`mws::engine::RealtimeStretcher` FREE-mode behavior per the ADR-006 causality
contract: preallocated history ring, same two-grain scheduler streamed, T=100% pure
delay (null-testable), T<100% clamped to 100%, T>100% lag with grain-boundary resync
at history exhaustion, and the exact latency formula. S900 varispeed FX with rate ≤ 1
clamp included.

## Context
Read first (the contract IS the spec — implement tests first):
- plan/decisions/006-fx-vs-sample-mode.md (decision table + contract terms)
- docs/design/dsp-engine.md §3.5 (engine-level summary incl. latency formula and
  host-rate/model-rate rule), §6 last paragraph (S900 FX rate clamp), §7.4 (latency)
- docs/design/architecture.md §5.2 (FREE column of the contract table; 30 s history
  (PI))
- docs/design/testing-strategy.md §3 item 4a/4c(FREE)/4d(FREE)/4e (tests to write
  FIRST)
- CyclicEngine (010/011), schedule-observation hook (012), RepitchEngine (013 —
  the S900 varispeed semantics this streams), CharacterChain + modelRateFor (019),
  ParamSnapshot (009)

TDD is mandatory: contract tests before implementation.

## Scope
- `include/mws/engine/RealtimeStretcher.h` + src:
  - `prepare(hostRate, maxBlock, ParamSnapshot)` — preallocates the 30 s (PI) history
    ring and all grain state; `process(inViews, outViews)` is allocation-free and
    **multichannel**: dual-mono with ONE shared grain/hop schedule across channels
    (architecture.md §5.2 stereo rule; S900/S950 mono-sum upstream per
    CharacterChain),
  - same two-grain cyclic scheduler as CyclicEngine (shared code, not a copy —
    refactor the grain loop into a shared header if needed; the task-010/012
    property tests must not change),
  - cycleLen interpreted in model-rate samples; ingest character runs the stretch at
    model rate (dsp-engine §3.5 host-rate rule), playback character to host rate,
  - `latencySamples()` = `ceil(2000 × hostRate / modelRate) + crossfadeLen + SRC group
    delay`; changes only with model/bandwidth/FS (non-automatable inputs),
  - FREE rules: effective T = max(T, 100%) with a `clampActive()` flag for the LCD
    (`FX MIN 100%`); T>100% read head lags; on history exhaustion jump read head to
    `writePos − latency` at the next grain boundary; parameter changes apply at grain
    boundaries only,
  - S900 model: variable-rate ZOH read head (semantics shared with RepitchEngine,
    task 013 — reuse, no fork); rate clamped ≤ 1 in FREE (ADR-003).
- Tests written first (`tests/unit/test_rtstretch_free.cpp`):
  - **4a null**: T=100%, character OFF ⇒ output equals input delayed by exactly
    `latencySamples()` — sample-exact over ≥ 5 s of streamed blocks of varying sizes,
  - **4c FREE clamp**: T=80% output identical to T=100%; `clampActive()` true,
  - **4d FREE resync**: with T=400% drive until history exhaustion; assert the read
    head jumps only at a grain boundary and only at exhaustion (instrument via
    schedule hook from task 012),
  - **4e latency**: formula matches for each model and S950 bandwidth 3.0/19.2 and
    S1000 FS 22.05 (≈ 12.8 k samples at BW 3.0 @ 48 kHz host — verify the documented
    extreme),
  - allocation-free `process` (allocation-counting hook or static assertion strategy),
  - **FX stereo coherence** (testing-strategy §3.5, FX half): two channels through
    one `process` — identical params + identical content ⇒ per-channel identical
    output; differing content ⇒ identical grain-launch schedule across channels
    (schedule hook from 012),
  - S900 FX: rate ≤ 1 enforced; T=100% null also holds for S900 with character OFF.

## Out of scope
- SYNC window mode + transport resync (task 023).
- Stream/offline equivalence (task 024).
- JUCE processBlock wiring + setLatencySamples (task 033).

## Acceptance criteria
- [ ] Contract tests written first; tag `[rtstretch][free]`; all pass.
- [ ] One grain-scheduler implementation shared with CyclicEngine (no fork).
- [ ] `process()` does not allocate after `prepare()`.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R rtstretch --no-tests=error
```
