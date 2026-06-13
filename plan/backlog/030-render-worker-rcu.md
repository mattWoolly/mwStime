---
id: 030
title: Threading — render worker, lock-free queues, RCU publication + graveyard FIFO
status: done
depends-on: [020, 027]
component: plugin
estimated-size: M
---

## Objective
The plugin's threading backbone: a single render worker thread running
OfflineRenderer with progress/abort, the lock-free render-request and UI-feedback
FIFOs, and the immutable-shared_ptr RCU publication protocol with audio-thread
graveyard — ThreadSanitizer-tested.

## Context
Read first:
- docs/design/architecture.md §4 (the entire threading diagram + the
  ownership/publication protocol paragraph — this protocol is a HARD requirement on
  every cross-thread buffer handoff)
- docs/design/testing-strategy.md §3 item 6 (TSan over render-publish/swap and FX
  history reconfiguration)
- OfflineRenderer (020) — the worker invokes it; plugin skeleton (027)

## Scope
- `plugin/src/EngineHost.{h,cpp}` (threading half; FX wiring is task 033):
  - `RenderWorker`: one `std::thread`/juce::Thread; consumes render requests from a
    lock-free queue (juce::AbstractFifo or equivalent); runs
    `OfflineRenderer::render` with progress posts to a UI FIFO and an atomic abort
    flag (hold-F8 semantics arrive in UI tasks); publishes
    `std::shared_ptr<const RenderedSample>` on completion,
  - publication protocol: published buffers immutable; audio thread copies the
    shared_ptr once per block; released pointers go into a graveyard lock-free FIFO
    drained on the message thread; deallocation NEVER on the audio thread,
  - a small reusable `Published<T>` template implementing copy-once/graveyard so the
    loaded sample (031), render result, and FX history reconfig all use one audited
    mechanism,
  - render-cap refusal and abort surface as typed worker results on the UI FIFO
    (LCD strings are UI-layer; pass enums).
- Tests:
  - `tests/plugin/test_publication.cpp`: producer/consumer stress — N publisher swaps
    against a fake audio thread copying per block; assert no use-after-free
    (ASan-clean) and graveyard drains; deterministic shutdown,
  - TSan: add `tsan` configure/build/test presets to `CMakePresets.json`
    (`-fsanitize=thread`) — an explicit deliverable of this task; the publish/swap
    and abort tests carry CTest label `tsan` and run under that preset, locally on
    macOS (the verification block below runs it unconditionally),
  - worker integration: request → progress events → published result; abort mid-render
    → Aborted event, no leak.

## Out of scope
- processBlock FX wiring (033), SamplePlayer (034), FileLoader (031).
- Any UI.

## Acceptance criteria
- [ ] Publication stress test passes under ASan; TSan preset/test passes locally.
- [ ] Audio-thread code paths contain no allocation/locks/file IO (review criterion +
      assertions where practical).
- [ ] One shared `Published<T>` mechanism (no ad-hoc atomics elsewhere).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R publication --no-tests=error
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -R publication --no-tests=error
```
