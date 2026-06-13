---
id: 053
title: FX streaming character chain — allocation-free host<->model resampler + per-block 12/16-bit character
status: todo
depends-on: [033]
component: engine
estimated-size: L
---

## Objective
The CHARACTER-ON FX path realizes the dsp-engine.md §3.5 host-rate rule for real:
an allocation-free, lock-free STREAMING resampler with continuous cross-block phase
runs the ingest character (host -> model rate, 12/16-bit quantize) BEFORE the
RealtimeStretcher and the playback character (model -> host rate) AFTER it, so the
streamed character-ON output matches the offline SAMPLE-mode render and the
dsp-engine.md §7.4 latency (incl. the §7.2 SRC group-delay term) is actually
realized — closing the task-033 deviation.

## Context
This task removes the documented deviation in `plugin/src/FxEngine.h` (the DEVIATION
header) and the honest-latency workaround it forced (review item 2a on task 033 PR #39).

Read first:
- `plugin/src/FxEngine.h` — the DEVIATION header + `realizedLatencyOf()` (the
  character-ON honest-PDC workaround this task deletes; once the SRC term is realized
  the path reports `RealtimeStretcher::latencySamples()` unconditionally).
- docs/design/dsp-engine.md §3.5 (host-rate rule — ingest before / playback after the
  stretcher), §7.2 (windowed-sinc resampler), §7.4 (FX latency = `ceil(2000 × hostRate
  / modelRate) + crossfadeLen + SRC group delay` — the SRC term this task makes real),
  §8.1/§8.2 (per-model 12-bit / 16-bit ingest+playback character chains).
- docs/design/architecture.md §4 (audio-thread rules: snapshot → process → never
  allocate/lock), §5.1 (character placement: ingest BEFORE, playback AFTER the engine),
  §5.2 (FX causality + latency).
- `libs/mwstime-core/include/mws/core/Resampler.h` — the existing OFFLINE allocating
  `SincResampler` / `LinearResampler`; this task adds the RT-safe STREAMING variant
  (preallocated state, continuous phase across `process()` calls, no allocation/lock).
- `libs/mwstime-core/include/mws/engine/RealtimeStretcher.h` (§55-63 processing-domain
  note — process() streams MODEL-rate audio), `latencySamples()` vs
  `realizedDelaySamples()` (the latter exists only to make the deviation honest).
- `libs/mwstime-core/include/mws/model/CharacterChain.h` — the OFFLINE per-model chain
  whose stages (resample, quantize, varispeed ZOH + Butterworth reconstruction, voice
  filter) the streaming chain must reproduce per block with continuous state.
- RealtimeStretcher (022/023), CharacterChain stages (008–016), EngineHost FX (033).

## Scope
- A new allocation-free STREAMING resampler (engine/core layer, JUCE-free so it builds
  into the core test binary and runs under the `tsan` preset like FxEngine):
  - preallocated sinc-kernel / ring state sized at prepare() for the supported host and
    model rates and max block; continuous fractional phase carried across blocks (no
    discontinuity at block boundaries — the bug a one-shot offline SincResampler causes);
  - both directions: host -> model (ingest, "downsample then process" [DRR F8]) and
    model -> host (playback); group delay matches `core::SincResampler::groupDelaySamples`
    so the §7.4 latency term it adds is the delay actually realized.
- A per-block STREAMING character chain wrapping the §8 stages with continuous state
  (12-bit mid-tread quantize / 16-bit quantize, S900/S950 varispeed ZOH + clock-tracking
  Butterworth reconstruction, S1000/S1100 voice filter), feeding model-rate audio to
  `RealtimeStretcher::process` and converting the model-rate output back to host rate.
- Wire it into `plugin/src/FxEngine.h` / `EngineHost`: when CHARACTER is ON, run ingest
  -> stretcher -> playback at model rate; delete the DEVIATION header, the
  `realizedLatencyOf()` deviation branch (report `latencySamples()` unconditionally),
  and the now-unneeded `RealtimeStretcher::realizedDelaySamples()` accessor.
- S900/S950 mono-sum runs for real in the streaming ingest (FxEngine::monoSummed() then
  reflects an actual sum, not just a flag).

## Out of scope
- New character ARTIFACTS beyond the §8 chain (no preamp/filter saturation — refuted,
  DRR #4; no SC-filter clock-bleed whine — v2, no oracle).
- SAMPLE-mode playback (034) and tempo-sync writes (037).
- UI.

## Acceptance criteria
- [ ] Streaming character-ON FX output matches the offline render of the same history
      sample-for-sample within the documented resampler tolerance (testing-strategy
      §3.4b equivalence contract extended to the character-ON path).
- [ ] FX latency for character-ON == `RealtimeStretcher::latencySamples()` (the SRC term
      is realized); a delayed-dry/character null/equivalence test at the reported latency
      passes; PDC is honest WITHOUT the task-033 workaround.
- [ ] processBlock stays allocation-free and lock-free (debug allocator hook + review);
      the streaming-resampler/character handoff carries the `tsan` CTest label.
- [ ] The FxEngine.h DEVIATION header and `realizedLatencyOf()` deviation branch are
      removed.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R "enginehost|character|resampler" --no-tests=error
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -R enginehost --no-tests=error
```
