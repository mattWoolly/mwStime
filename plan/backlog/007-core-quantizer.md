---
id: 007
title: mws::core::Quantizer — 12/16-bit mid-tread quantization + TPDF dither
status: todo
depends-on: [002]
component: dsp
estimated-size: S
---

## Objective
Bit-depth quantization for the character chains: mid-tread 12-bit and 16-bit
quantizers (values quantized, storage stays float32) with an optional seeded TPDF
dither (S1100 output stage).

## Context
Read first:
- docs/design/architecture.md §2.1 (`core/Quantizer.h`)
- docs/design/dsp-engine.md §8 intro ("bit depth stages quantize values, they do not
  change storage type"), §8.1 (12-bit mid-tread, no dither (PI)), §8.2 (S1100: 16-bit
  quantize with TPDF dither)
- docs/design/testing-strategy.md §2 Quantizer bullet, §3 item 6 (fixed rng seed for
  dithered paths — determinism)

TDD: write `tests/unit/test_quantizer.cpp` first.

## Scope
- `include/mws/core/Quantizer.h` (+ src):
  - `Quantizer{bits}`: mid-tread, full scale ±1.0, `process(view)`; `quantizeSample`.
  - Optional TPDF dither: explicit `uint64 seed` parameter; deterministic xorshift/PCG
    rng implemented in-repo (no `std::rand`).
- Tests (write first, per testing-strategy.md §2):
  - 12-bit step size exact (`1/2048` full scale),
  - idempotent on already-quantized input,
  - distinct-value count ≤ 4096 on arbitrary input (12-bit),
  - dithered path bit-identical across two runs with the same seed.

## Out of scope
- Where in the chain quantizers sit (tasks 016/017).

## Acceptance criteria
- [ ] Tests written first; tag `[quantizer]`; all pass.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R quantizer --no-tests=error
```
