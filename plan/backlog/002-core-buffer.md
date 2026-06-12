---
id: 002
title: mws::core Buffer — owning AudioBuffer + non-owning AudioView
status: in-review
depends-on: [001]
component: dsp
estimated-size: S
---

## Objective
`mws::core::AudioBuffer` (owning, float32, mono/multichannel) and
`mws::core::AudioView` (non-owning span view) exist, unit-tested, as the common audio
container for every core module.

## Context
Read first:
- docs/design/architecture.md §2.1 (`core/Buffer.h` role), §2 (core lib rules: pure C++20)
- docs/design/dsp-engine.md §3 intro (engines process mono float32; stereo = two
  linked instances)
- plan/backlog/001-project-cmake-skeleton.md (build layout)

TDD: write the Catch2 tests first (`tests/unit/test_buffer.cpp`).

## Scope
- `libs/mwstime-core/include/mws/core/Buffer.h` (+ src if needed; header-mostly OK):
  - `AudioBuffer`: channels × frames float32 storage, contiguous per channel; ctor
    (channels, frames, zero-init), `channel(i)` returning `AudioView`/mutable span,
    `numChannels()`, `numFrames()`, `sampleRate` field (double).
  - `AudioView`: pointer + length over one channel; const and mutable variants.
  - No allocation in any accessor; explicit `resize` only.
- Tests: construction/zero-init, channel views alias the same memory, resize,
  const-correctness compiles, sampleRate round-trip.

## Out of scope
- WavIo, resampling, any DSP.
- Lock-free / shared_ptr publication (plugin-layer concern, task 030).

## Acceptance criteria
- [ ] Tests written first; `tests/unit/test_buffer.cpp` exists with tag `[buffer]`.
- [ ] All buffer tests pass; no non-stdlib includes in the header.
- [ ] Used types compile from a plain `clang++ -std=c++20` TU (no JUCE).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R buffer --no-tests=error
```
