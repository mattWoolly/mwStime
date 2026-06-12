---
id: 001
title: CMake project skeleton, presets, Catch2, mwstime-core stub
status: todo
depends-on: []
component: infra
estimated-size: M
---

## Objective
A clean clone configures, builds, and runs an (initially trivial) test suite on macOS
with three commands. The pure JUCE-free `mwstime_core` static library exists as a stub
so all DSP tasks can start. No JUCE yet (that is task 027).

## Context
Read first:
- docs/design/architecture.md §2 (module layout, core lib rules), §2.1 (header map), §7 (build system), §8 (directory tree)
- plan/decisions/001-dsp-engine-architecture.md (pure-core decision)
- plan/ORCHESTRATION.md (local verification rule)

## Scope
- Top-level `CMakeLists.txt`: project `mwStime`, VERSION 0.1.0, C++20, CMake ≥ 3.25.
- FP discipline: no `-ffast-math` anywhere; `-ffp-contract=off` on `mwstime_core`
  (architecture.md §7 comment block).
- `cmake/FetchCatch2.cmake` (FetchContent, pinned Catch2 v3 tag).
- `libs/mwstime-core/CMakeLists.txt`: static lib `mwstime_core`, zero deps beyond the
  C++20 stdlib, `include/mws/` + `src/` layout with one stub header/source
  (e.g. `include/mws/core/Version.h` exposing an engine version string/hash constant —
  needed later by render metadata, architecture.md §6).
- `tests/CMakeLists.txt` + `tests/unit/` with one smoke Catch2 test (`[smoke]`) that
  links `mwstime_core` and checks the version constant; registered via `catch_discover_tests`.
- `tools/` directory placeholder with a commented-out `add_subdirectory` (CLI is task 025).
- `plugin/` add_subdirectory guarded behind an option `MWS_BUILD_PLUGIN` (default ON,
  but the directory may be absent until task 027 — guard with `EXISTS`).
- `CMakePresets.json`: configure preset `default` (RelWithDebInfo, build dir
  `build/default`), build preset `default`, test preset `default` (output-on-failure).
- `.gitignore` for `build/`.
- AGPLv3 license-header convention (the repo ships AGPLv3 — plan/ORCHESTRATION.md): a
  short header template (`// SPDX-License-Identifier: AGPL-3.0-or-later` + one
  project/copyright line) documented in a `docs/CONTRIBUTING.md` stub, applied to
  every source file this task creates, plus `tools/check_license_headers.sh`
  registered as a ctest (`license_headers`) failing when any committed
  `libs/`/`plugin/`/`tools/`/`tests/` C++ source lacks the header — every later task
  inherits the check automatically.

## Out of scope
- JUCE / clap-juce-extensions fetch (task 027).
- Any real DSP code, WavIo, the CLI.
- CI workflows, Linux/Windows support beyond not breaking portability.

## Acceptance criteria
- [ ] `cmake --preset default` succeeds from a clean clone on macOS.
- [ ] `cmake --build --preset default` builds `mwstime_core` and the test binary.
- [ ] `ctest --preset default` runs the smoke test and passes.
- [ ] `mwstime_core` compiles with no JUCE/3rd-party includes; `-ffp-contract=off` is
      set on the target (visible in `CMakeLists.txt`).
- [ ] CMakePresets.json defines `default` configure/build/test presets.
- [ ] The `license_headers` ctest passes (header on every committed source file).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default
```
