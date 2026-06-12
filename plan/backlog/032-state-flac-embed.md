---
id: 032
title: FLAC state-blob cache — embed sample audio in plugin state without message-thread stalls
status: todo
depends-on: [029, 031]
component: plugin
estimated-size: M
---

## Objective
Embedded-audio session persistence: the loaded sample is FLAC-encoded on the file
loader thread when it loads/changes and cached, so `getStateInformation` only memcpys
the cached blob; reload restores the sample (or falls back to path + hash) and
re-renders deterministically.

## Context
Read first:
- docs/design/architecture.md §6 (non-parameter state bullets: embedAudio default on
  ≤ 16 MB encoded; pre-encoded cached blob; render metadata → deterministic re-render
  instead of storing renders)
- docs/design/testing-strategy.md §6 Logic row (autosave under 16 MB embedded state,
  no message-thread stall — the failure mode this design avoids)
- State layer (029), FileLoader + sample-changed hook (031)

## Scope
- `plugin/src/state/StateBlobCache.{h,cpp}`:
  - on sample load/change: encode FLAC on the loader thread; cache
    `juce::MemoryBlock`; respect `embedAudio` and the 16 MB encoded cap (over-cap ⇒
    path-only persistence, flag for the UI),
  - `getStateInformation`: memcpy cached blob + params + state tree (never encodes),
  - `setStateInformation`: decode embedded FLAC → publish as SourceSample (via 030/031
    paths); if absent, resolve `sourceFile` path and verify content hash; then fire
    the deterministic re-render request (render metadata from 029),
  - threading: setState may arrive on the message thread — decode is dispatched to the
    loader thread; state remains consistent if the host immediately getStates again.
- Tests (`tests/plugin/test_state_embed.cpp`):
  - load fixture → getState → fresh processor → setState ⇒ sample restored (hash
    equal) and a re-render request fired with the saved ParamSnapshot,
  - embedAudio off ⇒ blob absent, path+hash present,
  - over-16 MB synthetic sample ⇒ path-only + flag,
  - getState called repeatedly while a load is in flight does not block (time-bound
    assertion) and returns a coherent blob.

## Out of scope
- UI surfacing of the embed/over-cap states (editor tasks).
- Storing rendered output (explicitly NOT stored — architecture.md §6).

## Acceptance criteria
- [ ] Tests pass; encode provably never runs inside getStateInformation (thread
      assert in debug builds).
- [ ] Round-trip restores audition-identical sample content.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R state_embed --no-tests=error
```
