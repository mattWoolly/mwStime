---
id: 012
title: Cyclic property suite — splice-comb signature, stereo coherence, determinism
status: todo
depends-on: [011]
component: qa
estimated-size: S
---

## Objective
The remaining research-pinned invariants of the cyclic core encoded as Catch2 property
tests: splice-comb spectral signature, stereo shared-hop-schedule coherence, and
bit-identical determinism.

## Context
Read first:
- docs/design/testing-strategy.md §3 items 5, 6, 8 (the exact property statements)
- docs/design/dsp-engine.md §3 intro (stereo = two linked instances, identical shared
  hop schedule), §3.4 (stereo edge rule)
- docs/research/akaizer-analysis.md §3 property 1 (splice-comb / "metallic ring")
- CyclicEngine (tasks 010–011)

This task is pure tests (plus a tiny in-test FFT helper if none exists — a simple
radix-2 FFT in the test tree is acceptable; do NOT add it to mwstime-core).

## Scope
- `tests/unit/test_cyclic_properties.cpp`:
  - **splice-comb signature** (testing-strategy §3.8): stretch a steady sine
    (e.g. 440 Hz, T=300%, C=1000) and assert FFT sidebands spaced at
    `modelRate / hop_out` within one bin of prediction,
  - **stereo coherence** (§3.5): two channels, identical params + identical content ⇒
    per-channel sample-identical output; with differing content, grain launch times
    (instrument the engine via a schedule-callback or expose the launch schedule for
    test) are identical across channels, CLASSIC and REVISED,
  - **determinism** (§3.6, core part): same (input, params) twice ⇒ bit-identical
    buffers; covers CLASSIC and REVISED (TSan/threading half lives in task 030).
- If the engine lacks a way to observe the hop schedule, add a minimal
  test-observation hook (e.g. optional `std::function<void(GrainLaunch)>` or a
  returned schedule vector) — keep it allocation-free in the default path.

## Out of scope
- FX contract tests (tasks 022–024), golden renders (task 026).

## Acceptance criteria
- [ ] All three property tests exist, tagged `[cyclic][properties]`, and pass.
- [ ] No production-code behavior change (observation hook only).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R properties --no-tests=error
```
