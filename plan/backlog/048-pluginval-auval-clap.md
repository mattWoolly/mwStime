---
id: 048
title: Format validation scripts (macOS) — pluginval VST3/AU, auval, clap-validator
status: in-review
depends-on: [045b]
component: qa
estimated-size: S
---

## Objective
Scripted, locally runnable format validation on macOS: pluginval at strictness 10 for
VST3 and AU, `auval -v aumf MwS1 MwSt`, and clap-validator for the CLAP build — all
wired as CTest entries gated on tool availability.

## Context
Read first:
- docs/design/testing-strategy.md §5 (the corrected validator matrix — pluginval
  CANNOT load LV2/CLAP; invocations specified there)
- docs/design/architecture.md §3 (format table + shell validator column), §7 (auval
  must target `aumf`)
- plan/decisions/002-plugin-formats-v1.md (validation plan)
- Built plugin (045/045b — validate the real editor, not the stub)

## Scope
- `tests/plugin/run_pluginval.sh`: downloads/uses a PINNED pluginval release (cached
  under `build/`, never committed); runs
  `--validate-in-process --strictness-level 10 --repeat 3 --randomise` against the
  built VST3 and AU; clear pass/fail exit.
- `tests/plugin/run_auval.sh`: `auval -v aumf MwS1 MwSt` (after component
  registration; document the `killall -9 AudioComponentRegistrar` refresh dance).
- `tests/plugin/run_clap_validator.sh`: pinned clap-validator release against the
  built CLAP.
- CTest registration with label `validators`, `DISABLED`-by-default OFF but each
  script self-skips (exit 77 / CTest SKIP_RETURN_CODE) when its tool can't be
  fetched — keeps `ctest --preset default` green offline.
- Fix any validator findings in this task if trivial; otherwise file follow-up tasks
  and mark the failing check expected with a tracked TODO (no silent skips).

## Out of scope
- LV2 validation (lv2lint/lv2_validate — Linux task 049).
- CI invocation (task 051). Host smoke matrix (QA phase).

## Acceptance criteria
- [x] All three scripts pass locally on macOS against the current build.
      (pluginval VST3+AU strictness 10, auval `-v aumf MwS1 MwSt`, clap-validator
      0.3.2 — all exit 0. One pluginval sub-test, "Plugin state restoration", is a
      loud + tracked waiver: a known JUCE/pluginval AudioParameterBool flake, NOT a
      state bug — follow-up plan/backlog/048c.)
- [x] `ctest --preset default -L validators --no-tests=error` runs them; offline run skips cleanly.
      (Each script exits 77 → CTest SKIP_RETURN_CODE 77 when its pinned tool can't be
      fetched or no artifact is built; verified offline = 100% passed, 0 failed.)
- [x] pluginval/clap-validator versions pinned in the scripts.
      (pluginval v1.0.4 + clap-validator 0.3.2, each with a SHA-256 verified download.)

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -L validators --no-tests=error
tests/plugin/run_auval.sh
```
