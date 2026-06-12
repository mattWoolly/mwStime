---
id: 003
title: mws::core::WavIo — JUCE-free WAV reader/writer
status: in-review
depends-on: [002]
component: dsp
estimated-size: M
---

## Objective
A minimal, dependency-free WAV reader/writer supporting 16/24/32-bit int and 32-bit
float, mono/stereo — the IO layer for the `mwstime-render` CLI and the golden tests.

## Context
Read first:
- docs/design/architecture.md §2 ("tools/mwstime-render links only mwstime-core plus a
  minimal WAV reader/writer (mws::core::WavIo) … same envelope as Akaizer")
- docs/research/akaizer-analysis.md §2.1 (supported-format envelope)
- docs/design/testing-strategy.md §2 (WavIo test requirement: round-trip all formats)
- libs/mwstime-core/include/mws/core/Buffer.h (task 002)

TDD: write `tests/unit/test_wavio.cpp` round-trip tests first.

## Scope
- `include/mws/core/WavIo.h` + `src/core/WavIo.cpp`:
  - `WavIo::read(path) -> AudioBuffer` (converts to float32, keeps source sample rate;
    errors via a `Result`/expected-style return, no exceptions across the API).
  - `WavIo::write(path, AudioBuffer, BitDepth)` for Int16/Int24/Int32/Float32.
  - RIFF parsing tolerant of extra chunks; rejects unsupported formats with a clear error.
- Deterministic conversion rules (define and test the int↔float scaling, round-to-nearest).
- Tests: round-trip each bit depth × mono/stereo; truncated-file and wrong-magic errors;
  16-bit values survive a write/read cycle bit-exactly.

## Out of scope
- AIFF/FLAC/MP3 (plugin file loading uses JUCE readers — task 031).
- Streaming IO; everything is whole-file.

## Acceptance criteria
- [ ] Tests written first; tag `[wavio]`; all pass.
- [ ] Round-trip is bit-exact for int formats and exact for float32.
- [ ] No dependencies beyond the C++20 stdlib.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R wavio --no-tests=error
```
