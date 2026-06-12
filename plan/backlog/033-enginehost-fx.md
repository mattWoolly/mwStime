---
id: 033
title: EngineHost FX path — RealtimeStretcher in processBlock + latency reporting
status: todo
depends-on: [022, 023, 028, 030]
component: plugin
estimated-size: M
---

## Objective
FX mode is live: processBlock snapshots params, runs RealtimeStretcher (FREE + SYNC)
with the character chain, and the processor reports/recomputes latency exactly per the
ADR-006 formula on prepareToPlay and model/bandwidth/FS changes.

## Context
Read first:
- docs/design/architecture.md §4 (audio-thread rules: snapshot → process → never
  allocate/lock), §5.2 (latency paragraph — recompute triggers, PDC documentation)
- plan/decisions/006-fx-vs-sample-mode.md (contract terms — latency, grain-boundary
  param application)
- docs/design/dsp-engine.md §7.4 (latency incl. character-only case), §3.5
- RealtimeStretcher (022/023), Parameters/makeSnapshot (028), EngineHost threading
  (030)

## Scope
- `plugin/src/EngineHost.{h,cpp}` (FX half) + PluginProcessor wiring:
  - prepareToPlay: `RealtimeStretcher::prepare`, compute + `setLatencySamples`,
  - processBlock (FX mode): `makeSnapshot` → pass transport info (AudioPlayHead →
    TransportInfo plain struct; SYNC math is engine-side) → `process` → outTrim,
  - non-automatable changes (model/bandwidth/FS) arrive via a message-thread path
    that reconfigures the stretcher using the task-030 publication protocol (history
    reconfiguration handoff — the TSan-tested path) and re-reports latency,
  - stereo handled as dual-mono with shared schedule (engine-side; host glue passes
    both channels), S900/S950 mono-sum + LCD flag plumbed,
  - character-only case (T locked 100%) reports only SRC + filter group delay
    (dsp-engine §7.4),
  - pluginMode SAMPLE ⇒ FX path bypassed (SamplePlayer is task 034; pass silence or
    dry per mode default until then — explicitly dry passthrough),
  - **FX input-scope feed**: push decimated input samples into a lock-free FIFO
    (architecture.md §4 FIFO→timer-poll pattern) — the producer for WaveformView's
    live scope in FX mode (ui-design §2 WaveformView row, §6.4; consumer is 043/045b).
- Tests (`tests/plugin/test_enginehost_fx.cpp`, headless processor harness):
  - render a buffer through the processor at T=100%, character OFF ⇒ null against
    delayed dry at the reported latency (testing-strategy §3.4a at processor level),
  - latency re-report fires exactly on model/bandwidth/FS change and never on
    timeFactor/cycleLen automation (testing-strategy §3.4e),
  - automation spam: 1000 timeFactor changes across the FX clamp boundary in one
    second of blocks — no allocation (debug allocator hook), no NaN/inf in output,
  - **TSan over FX history reconfiguration**: the model/bandwidth/FS reconfiguration
    handoff test carries CTest label `tsan` and runs under the task-030 `tsan` preset
    (testing-strategy §3.6 names this path explicitly).

## Out of scope
- SAMPLE-mode playback (034), MIDI voice (035), tempo-sync parameter writes (037).
- UI.

## Acceptance criteria
- [ ] Processor-level null test passes; latency tests pass.
- [ ] processBlock is allocation/lock-free (debug assertions + review).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R enginehost --no-tests=error
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -R enginehost --no-tests=error
```
