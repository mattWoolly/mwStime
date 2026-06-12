---
id: 050
title: Windows build bring-up — MSVC, VST3/CLAP/Standalone, golden tolerance
status: todo
depends-on: [049]
component: infra
estimated-size: M
---

## Objective
The project builds and tests on Windows (MSVC, x64): core tests pass, integer-CLASSIC
goldens bit-exact, float goldens within tolerance, VST3 + CLAP + Standalone build, and
pluginval(VST3)/clap-validator scripts run. Windows is the locked third platform goal.

## Context
Read first:
- plan/ORCHESTRATION.md (platforms: "Windows third goal"; CI late because Windows
  slows iteration)
- docs/design/architecture.md §2 (bit-exactness scope — the integer path must hold on
  MSVC too), §3 (formats; AU is macOS-only, LV2 optional on Windows — VST3 + CLAP +
  Standalone are the Windows set)
- docs/design/testing-strategy.md §4 (off-reference tolerance policy)
- Linux bring-up patterns (task 049) and docs/BUILDING.md

## Scope
- MSVC portability fixes (no `-ffp-contract` equivalent needed for the integer path,
  but verify `/fp:precise`; no variable-length arrays, POSIX-isms in scripts →
  provide .ps1 or python equivalents where needed).
- Presets: `windows-default` (Visual Studio or Ninja+MSVC) configure/build/test.
- Build targets: VST3, CLAP, Standalone (skip AU; LV2 optional — note the decision in
  the PR).
- Golden comparer tolerance mode verified on Windows; integer-CLASSIC cases must be
  bit-exact (this validates the fixed-point discipline on a third compiler).
- pluginval(VST3) + clap-validator script variants for Windows.
- docs/BUILDING.md Windows section.

## Out of scope
- CI Windows job (task 051 marks it last per ORCHESTRATION).
- Installer/signing (post-v1 release engineering).

## Acceptance criteria
- [ ] On Windows: configure/build/ctest pass with the `windows-default` preset.
- [ ] Integer-CLASSIC goldens bit-exact; float cases within tolerance.
- [ ] VST3 passes pluginval strictness 10 on Windows.
- [ ] macOS + Linux builds unaffected.

## Verification commands
```
# on Windows
cmake --preset windows-default
cmake --build --preset windows-default
ctest --preset windows-default
# on macOS (no regression)
cmake --preset default && cmake --build --preset default && ctest --preset default
```
