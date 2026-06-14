---
id: 051
title: GitHub Actions CI — macOS + Linux (+ Windows last), mirrors local presets
status: done
depends-on: [026, 049, 050]
component: infra
estimated-size: M
---

## Objective
CI exists and runs exactly what agents run locally: configure → build → ctest →
format validators on macOS and Linux, with a Windows job added last, plus artifact
upload. Explicitly the FINAL infra task — CI was deferred by the locked
ORCHESTRATION decision and nothing may land here earlier.

## Context
Read first:
- plan/ORCHESTRATION.md (CI deferred — phase 6; "No test logic may live only in CI")
- docs/design/testing-strategy.md §8 (CI section: mirror local presets; Akaizer
  cross-check EXCLUDED from CI — closed binary)
- Presets and scripts from tasks 001, 026, 048, 049, 050

## Scope
- `.github/workflows/ci.yml`:
  - macOS job (arm64 runner): `cmake --preset default` → build → `ctest --preset
    default` (unit + property + golden) → `ctest -L validators` (pluginval/auval/
    clap-validator via the 048 scripts) → upload plugin artifacts,
  - Linux job (ubuntu): apt deps from docs/BUILDING.md, xvfb-run for validator steps,
    `linux-default` preset, golden tolerance mode, lv2 checks,
  - Windows job (windows-latest, `windows-default` preset) marked `continue-on-error:
    false` but added in the SAME PR as the last commit, per "Windows last",
  - concurrency cancellation, ccache where easy; pinned action versions,
  - golden-bless guard: CI fails if `tests/golden/blessed/` changed without a
    MANIFEST.json change in the same diff (testing-strategy §4 review policy aid).
- No new test logic in YAML — every step is a preset or committed script
  (testing-strategy §8 rule). The Akaizer cross-check must not appear.
- Release artifact step: zip VST3/AU/LV2/CLAP/Standalone per OS on tag builds.

## Out of scope
- Code signing/notarization, release automation beyond artifact zips.
- New tests of any kind.

## Acceptance criteria
- [ ] CI green on a PR touching all three layers (demonstration run linked in PR).
- [ ] Every CI step maps 1:1 to a local command documented in docs/BUILDING.md.
- [ ] Windows job present and green (or explicitly marked allow-fail with an owner
      sign-off recorded in the PR).
- [ ] No reference to Akaizer or research-cache anywhere in workflows.

## Verification commands
```
# local equivalence check before pushing:
cmake --preset default && cmake --build --preset default && ctest --preset default
ctest --preset default -L validators --no-tests=error
# then: open PR, observe the Actions run green; link it in the PR body
```
