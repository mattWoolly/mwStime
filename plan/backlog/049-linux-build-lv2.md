---
id: 049
title: Linux build bring-up — toolchain fixes, LV2 validators, golden tolerance policy
status: todo
depends-on: [026, 048]
component: infra
estimated-size: M
---

## Objective
The full project configures, builds, and tests on Linux (x86_64, GCC and/or Clang):
core tests pass, integer-CLASSIC goldens match bit-exactly, float-stage goldens pass
the tolerance compare, all plugin formats build, and LV2 validation scripts run.

## Context
Read first:
- docs/design/architecture.md §2 (cross-platform bit-exactness scope), §3 (Linux
  formats), §7 (FP discipline flags)
- docs/design/testing-strategy.md §4 (comparison policy off-reference: max abs ≤ 1e-6
  + spectral checks), §5 (lv2lint + lv2_validate + Carla smoke; xvfb for headless)
- plan/decisions/002-plugin-formats-v1.md (LV2-at-v1 rationale and the drop-LV2
  escape hatch)
- plan/ORCHESTRATION.md (platforms: macOS + Linux required)
- Golden comparer, cases.json, and the macOS-blessed set (026 — this task modifies
  the comparer and compares against that blessed set)

## Scope
- Fix portability issues across `libs/`, `tools/`, `tests/`, `plugin/` (endianness
  assumptions in WavIo, filesystem paths, compiler warnings as needed); document
  required apt packages (JUCE Linux deps: ALSA, X11, freetype, curl, webkit not
  needed) in `docs/BUILDING.md`.
- Presets: add `linux-default` configure/build/test presets (same flags, Ninja).
- Golden comparer: implement/enable the off-reference tolerance mode keyed by
  platform — integer CLASSIC cases stay bit-exact (hard assert), float-stage cases
  use max abs ≤ 1e-6 + diagnostics (testing-strategy §4 policy).
- `tests/plugin/run_lv2_checks.sh`: lv2lint + lv2_validate (sord_validate) on the
  built LV2 bundle; **Carla load smoke REQUIRED** (testing-strategy §5 LV2 row —
  self-skips only when Carla is absent, the same pattern as the other validators,
  never "optional"); xvfb-run wrapper for headless boxes; CTest label `validators`
  (self-skip when tools absent).
- Linux host smoke rows from `tests/plugin/host-matrix.md` (task 048b): Ardour LV2
  load + session reload (testing-strategy §6) — manual run, result recorded in the
  matrix file and the PR.
- Verify clap-validator + pluginval(VST3) scripts from 048 also run on Linux; adjust
  download logic per-OS.
- If the JUCE LV2 exporter is broken: do NOT hack around it silently — record
  findings; dropping LV2 is a one-line owner decision per ADR-002.

## Out of scope
- Windows (task 050). CI (task 051). UI screenshot gates (macOS-only by design).

## Acceptance criteria
- [ ] On a Linux machine/VM: `cmake --preset linux-default && cmake --build --preset
      linux-default && ctest --preset linux-default` passes (validators may skip if
      tools absent, goldens must run).
- [ ] Integer-CLASSIC goldens bit-exact vs the macOS-blessed set; float cases within
      tolerance.
- [ ] LV2 bundle passes lv2lint + lv2_validate.
- [ ] docs/BUILDING.md covers Linux prerequisites.

## Verification commands
```
# on Linux
cmake --preset linux-default
cmake --build --preset linux-default
ctest --preset linux-default
ctest --preset linux-default -L validators --no-tests=error
# on macOS (no regression)
cmake --preset default && cmake --build --preset default && ctest --preset default
```
