---
id: 059
title: Fix AU crash — FxEngine::prepare() races processBlock (prepareToPlay not audio-thread-safe)
status: in-review
depends-on: []
component: plugin
estimated-size: M
---

## Objective
Calling `prepareToPlay` concurrently with `processBlock` (as AU/ausdk hosts do) is race-free.
`FxEngine::prepare()` no longer mutates non-atomic state the audio thread reads, and a
torn/half-prepared engine can never reach `RealtimeStretcher::process`. The deterministic
pluginval crash (AU, strictness 10, seed 0x26d1e76 — "Parameter thread safety") is gone, and
a `[tsan]` test races `prepare()` vs `processBlock` to lock it in.

## Context
A pluginval strictness-10 SEGFAULT (AU only; VST3 fine) was traced — deterministically
reproduced and confirmed under ThreadSanitizer — to a real data race. NOT a flake; NOT from
tasks 055/056/057 (pre-existing from the original FX engine, commit cca84618 / tasks 033/046).
Read these first:
- `plugin/src/FxEngine.h:186-218` `prepare()` mutates NON-ATOMIC shared state —
  `hostRate_`/`maxBlock_`/`channels_`/`activeKey_` (:189-192), `fadeScratch_`/`fadeViews_`
  (:198-200) — then republishes `active_` (:215) with a stretcher still mid-
  `RealtimeStretcher::prepare()`. It does so under a FALSE comment (:206-207): "prepare() is
  prepareToPlay only — the audio thread is stopped — so the freshly built engine becomes the
  active one directly (no handoff race)." AU/ausdk calls `prepareToPlay` from threads that
  overlap `Render`/`processBlock`, so this assumption is wrong.
- Crash interleaving (caught in lldb): `processBlock` fade branch (`FxEngine.h:318`) →
  `runEngine` (:458) → `RealtimeStretcher::process` → null store at
  `RealtimeStretcher.cpp:228` (`ring[(written_+i)%historyLen_]=in[i]`) because the stretcher
  is half-prepared (`historyLen_==0`, `history_` unallocated) — a torn read from the
  concurrent `prepare()` writer.
- TSan (project `tsan` preset) pinpoints: WRITE `FxEngine.h:191` (`channels_=numChannels`) in
  `prepare` vs READ `FxEngine.h:414` (`channels_`) in `computeLcd` ← `publishLcd` (:423) ←
  `publishActive` (:434) ← `processBlock` (:362). `SUMMARY: ThreadSanitizer: data race
  FxEngine.h:414 in FxEngine::computeLcd`.
- The `requestReconfigure()` ↔ `processBlock` RCU/graveyard handoff (tasks 055/046) is
  CORRECT and TSan-clean — `prepare()` simply fails to use it. The editor
  `pollEngine`/`EngineHost::currentRender()` path is correctly RCU-guarded (`Published<T>`),
  not implicated.
Reproduction for verification: AU, `pluginval --strictness-level 10 --random-seed 0x26d1e76
--validate <...>/mwStime.component` crashes 8/8 today; must pass after the fix. pluginval is
at `build/validator-tools/pluginval-*/` (or fetched by `tests/plugin/run_pluginval.sh`).
Design contract: `architecture.md` §4 (processBlock lock-free; all cross-thread handoffs via
the atomic/RCU publish path).

## Scope
- Route `FxEngine::prepare()` through the SAME publish/adopt handoff as `requestReconfigure()`:
  build the new `PreparedFx` (heavy alloc off the audio thread), publish via
  `std::atomic_store(&pending_, ...)`, and let `processBlock` adopt it — the path already
  proven race-free by task 055. Do NOT write `active_` directly while audio may run.
- Eliminate the unsynchronized non-atomic member writes the audio thread reads
  (`hostRate_`/`maxBlock_`/`channels_`/`activeKey_`/`fadeScratch_`/`fadeViews_`): fold them
  into the published `PreparedFx`, OR make them atomic, OR size `fadeScratch_`/`fadeViews_`
  once at construction (max channels × max block) so they are never reallocated under a live
  `processBlock`. Result: `computeLcd`/`runEngine` must never read a field a concurrent
  `prepare()` is mid-write on.
- Belt-and-braces: `PluginProcessor::processBlock` (and/or `EngineHost::processFxBlock`)
  outputs silence / early-returns if the FX engine is not fully prepared (e.g. zero-sized /
  `!isPrepared()`), so a torn engine can never reach `RealtimeStretcher::process`.
- Delete the false "prepareToPlay only — audio thread is stopped" comment (`FxEngine.h:206-207`)
  and document the real concurrency contract.
- Keep behavior identical for the normal (non-racing) prepare case; no audio-output change.

## Out of scope
- Re-architecting `requestReconfigure` (it is correct).
- Editor/export/UI changes (056/057 are fine).
- Windows-specific work.

## Acceptance criteria
- [ ] New `[tsan]` test in `tests/unit/test_enginehost_fx_reconfig.cpp` (or a sibling) races a
      thread that storms `FxEngine::prepare()` (varying sampleRate/blockSize/channels) against
      a thread running `processBlock`, plus the LCD poll — passes clean under
      `ctest --preset tsan -L tsan` (this test FAILS today under TSan).
- [ ] Existing `[tsan]` FX tests still pass.
- [ ] Deterministic repro fixed: AU validated with
      `pluginval --strictness-level 10 --random-seed 0x26d1e76` passes (run it ≥20× in a loop;
      0 crashes). Also spot-check a couple of other seeds and the VST3.
- [ ] Full macOS build + `ctest --preset default` green; no audio-output regression
      (golden renders unchanged).
- [ ] processBlock remains allocation-free / lock-free on the audio thread.

## Verification commands
```
cmake --preset default -DFETCHCONTENT_BASE_DIR=$HOME/.cache/mwstime-fc
cmake --build --preset default -j 6
ctest --preset default
ctest --preset tsan -L tsan --no-tests=error
# deterministic AU repro (must be 0 crashes):
AU=build/default/plugin/mwStime_artefacts/RelWithDebInfo/AU/mwStime.component
PV=$(ls build/validator-tools/pluginval-*/pluginval.app/Contents/MacOS/pluginval 2>/dev/null | head -1)
for i in $(seq 1 20); do "$PV" --strictness-level 10 --random-seed 0x26d1e76 --validate "$AU" >/dev/null 2>&1 || { echo "CRASH on iter $i"; break; }; done; echo "repro loop done"
```
