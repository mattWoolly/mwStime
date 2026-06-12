---
id: 027
title: JUCE 8 plugin skeleton — FetchContent, all v1 formats, passthrough processor
status: in-review
depends-on: [001]
component: infra
estimated-size: M
---

## Objective
The `plugin/` target exists and builds VST3, AU (`aumf`), LV2, Standalone, and CLAP on
macOS, with a passthrough PluginProcessor and an empty PluginEditor — the shell every
plugin/ui task builds on.

## Context
Read first:
- docs/design/architecture.md §3 (format table), §7 (build system — juce_add_plugin
  arguments, aumf rationale, clap-juce-extensions pinning), §8 (plugin/ tree)
- plan/decisions/002-plugin-formats-v1.md (the format decision + category minefield)
- plan/backlog/001-project-cmake-skeleton.md (presets, guarded add_subdirectory)

## Scope
- `cmake/FetchJUCE.cmake` (JUCE 8.x pinned tag) and `cmake/FetchClapExtensions.cmake`
  (clap-juce-extensions pinned tag; a comment notes that bumping the tag is an
  explicit backlog task, never incidental — architecture.md §3).
- `plugin/CMakeLists.txt`: `juce_add_plugin` with `COMPANY_NAME mattWoolly`,
  `PLUGIN_MANUFACTURER_CODE MwSt`, `PLUGIN_CODE MwS1`,
  `FORMATS AU VST3 LV2 Standalone`, `IS_SYNTH FALSE`, `NEEDS_MIDI_INPUT TRUE`
  (⇒ AU exports as `aumf` music effect) + `clap_juce_extensions_plugin(...)`;
  `COPY_PLUGIN_AFTER_BUILD TRUE` on macOS so the AU lands in
  `~/Library/Audio/Plug-Ins/Components` and `auval` can actually find it (if a stale
  registration lingers, `killall -9 AudioComponentRegistrar` refreshes it).
- `plugin/src/PluginProcessor.{h,cpp}`: stereo in/out passthrough processBlock, MIDI
  input accepted (ignored for now), no allocation in processBlock.
- `plugin/src/PluginEditor.{h,cpp}`: empty 1000×380 editor with a placeholder paint.
- mwstime_core linked into the plugin target (proves the layering compiles together).
- AGPLv3 license headers per the task-001 convention on every new file (the
  task-001 `license_headers` ctest must keep passing).

## Out of scope
- Parameters, state, threading, UI components (tasks 028+).
- pluginval/auval/clap-validator scripts (task 048) — manual auval spot check only.
- Linux/Windows builds (tasks 049/050).

## Acceptance criteria
- [ ] `cmake --build --preset default` produces VST3, AU component, LV2 bundle,
      Standalone app, and CLAP on macOS.
- [ ] `auval -v aumf MwS1 MwSt` passes (the type is `aumf`, not `aufx`).
- [ ] Standalone app launches and passes audio through (manual check noted in PR).
- [ ] `ctest --preset default` still passes (core tests unaffected).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default
# COPY_PLUGIN_AFTER_BUILD has installed the AU; refresh registration if needed:
killall -9 AudioComponentRegistrar 2>/dev/null || true
auval -v aumf MwS1 MwSt
```
