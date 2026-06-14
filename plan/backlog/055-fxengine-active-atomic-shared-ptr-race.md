---
id: 055
title: Fix data race on FxEngine::active_ (atomic shared_ptr handoff) + LCD-snapshot + TSan coverage
status: in-review
depends-on: [033]
component: plugin
estimated-size: M
---

## Objective
The audio-thread ↔ message-thread handoff of the live FX engine is race-free and
ThreadSanitizer-covered, as `architecture.md` §4 and `testing-strategy.md` §6 promise:
`FxEngine::active_` is published/consumed atomically, the message-thread LCD accessors read
a snapshot (never the live mutating pointer or the per-block bool), and the `[tsan]` test
actually exercises those accessors concurrently.

## Context
QA finding F2 (HIGH) in `docs/QA-REPORT.md`. TSan-proven, reproduced with a standalone
`clang++ -fsanitize=thread` harness built against the real (JUCE-free) `FxEngine.h` + core
sources.

Verified facts (read these first):
- `plugin/src/FxEngine.h:462` — `active_` is a PLAIN `std::shared_ptr<PreparedFx>` (the
  comment at `:461-462` even claims it is "never touched by the message thread" — false).
- `plugin/src/FxEngine.h:272-273` (and `:300-301`) — the AUDIO thread reassigns `active_`
  (`graveyard_.retire(active_)` → `ptr.reset()` at `:477`, then `active_ = std::move(...)`).
  Note `pending_` is correctly handled with `std::atomic_*_explicit`; the `active_` handoff
  is the half that is not atomic.
- `plugin/src/FxEngine.h:325-328` `clampActive()` and `:336-344` `monoSummed()` dereference
  `active_` on the MESSAGE thread, reached from the 30 Hz UI timer:
  `PluginEditor.cpp:282` `timerCallback` → `pollEngine()` → `host.fxClampActive()`
  (`PluginEditor.cpp:350` → `EngineHost.h:242`) and `refreshLcd()` → `host.fxMonoSummed()`
  (`PluginEditor.cpp:377` → `EngineHost.h:246`).
- Production trigger is real: `PluginProcessor::parameterChanged` (`:169,180`) →
  `EngineHost::reconfigureFxIfNeeded` → `fx_.requestReconfigure`, adopted on the audio
  thread in `processBlock` — i.e. any model/bandwidth/FS/character change while audio plays
  and the editor is open races the LCD read.
- Torn read: `clampActive_` is a plain `bool` (`RealtimeStretcher.h:317`) written every
  block on the audio thread in `applyParams` (`RealtimeStretcher.cpp:142`) and read on the
  message thread.
- TSan reported exactly two races: size-8 `shared_ptr` write (`FxEngine.h:477` via `:272`)
  vs read by the LCD-poll thread (use-after-free class), and size-1 torn `clampActive_`
  read.
- Test gap: `tests/unit/test_enginehost_fx_reconfig.cpp:181-245` spawns audio + message
  threads but the message thread calls only `requestReconfigure`/`consumeLatencyDirty`/
  `collectGarbage` — never `clampActive()`/`monoSummed()`/`modelRate()`, so it structurally
  cannot catch this. The `tsan` preset sets `MWS_BUILD_PLUGIN=OFF`
  (`CMakePresets.json:59`), but `FxEngine.h` is JUCE-free and TSan-reproducible standalone.

Design contract (no PI/ADR carve-out covers cross-thread `active_` access):
- `architecture.md` §4 L161-168 — all cross-thread buffer handoffs (explicitly incl. "FX
  history reconfiguration") are held by `std::shared_ptr` and are ThreadSanitizer-tested.
- `testing-strategy.md` §6 L92-95 — "ThreadSanitizer run over … the FX history
  reconfiguration."

## Scope
- Make the `active_` handoff atomic on BOTH sides: store/load via
  `std::atomic<std::shared_ptr<PreparedFx>>` (or `std::atomic_store_explicit`/
  `std::atomic_load_explicit` on a plain `shared_ptr`), matching the existing `pending_`
  discipline. The audio thread publishes; the message thread `atomic_load`s a local copy
  before dereferencing.
- Give the LCD accessors a race-free read path: `clampActive()` / `monoSummed()` /
  `modelRate()` must operate on an atomically-loaded snapshot (and the bool/flags they
  read must be published atomically, not read live). Have the audio thread publish a small
  POD LCD snapshot (clampActive, monoSummed, modelRate) the message thread reads, OR make
  `clampActive_` atomic — pick the approach that keeps `processBlock` allocation/lock-free.
- Update the `FxEngine.h:461-462` and `:94-96` comments to match reality.
- Extend the `[tsan]` test (`tests/unit/test_enginehost_fx_reconfig.cpp`) so the message
  thread ALSO calls `clampActive()` / `monoSummed()` (and `modelRate()`) concurrently with
  audio-thread reconfigure adoption — the regression that would have caught this.

## Out of scope
- The character-streaming rework (task 053).
- Any change to the RCU `Published<T>` source-sample mechanism (verified correct).
- Plugin-only refactors that would force `MWS_BUILD_PLUGIN=ON` under the `tsan` preset —
  keep the reproduction in the JUCE-free core/FxEngine layer.

## Acceptance criteria
- [ ] `active_` is published/consumed atomically on both audio and message sides; no plain
      `shared_ptr` read of `active_` remains on the message thread.
- [ ] LCD accessors read an atomically-published snapshot (no live deref of a mutating
      `shared_ptr`, no torn-bool read).
- [ ] The `[tsan]` reconfig test exercises `clampActive()`/`monoSummed()`/`modelRate()`
      concurrently and runs CLEAN under ThreadSanitizer (no races reported).
- [ ] `processBlock` remains allocation-free and lock-free.
- [ ] Full ctest green; validators still pass.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R enginehost --no-tests=error
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -R "enginehost|fx_reconfig" --no-tests=error
```
