<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly. -->

# Host smoke-test matrix

Manual / scripted host smoke tests (testing-strategy.md §6). Scripted validators
(pluginval / auval / clap-validator / lv2lint+Carla) run from
`ctest -L validators`; the rows below are the **manual DAW load/reload** checks
that can't be fully scripted. Record the result, date, platform, and host
version inline when a row is run.

> **Scope note (task 049):** this file was created by task 049 to record the
> Linux LV2/Ardour smoke row it owns. Task **048b** owns the full cross-platform
> matrix (REAPER/Bitwig/Logic/Standalone rows on macOS+Linux) and will expand
> the table below; 049 only fills the Linux rows in its scope.

## Status legend

- `PASS` — performed, all listed checks green (with date/host/platform).
- `TODO` — not yet performed.
- `N/A`  — not applicable on this platform.

## Matrix

| Host | Platform | Checks (testing-strategy.md §6) | Result |
|---|---|---|---|
| Ardour | Linux | LV2 load + session reload | TODO — manual; run on a Linux box with Ardour (see below) |
| Carla / lv2lint | Linux | LV2 manifest validity + headless load smoke | Scripted: `ctest --preset linux-default -L validators` (`run_lv2_checks.sh`) |
| REAPER | macOS, Linux | insert FX, automation r/w of timeFactor, state save/reload (embedded sample), offline=realtime, latency null at T=100%, latency re-report on model switch, tempo change mid-play (SYNC), transport stop/loop while windowed | TODO (task 048b) |
| Bitwig | Linux, macOS | CLAP load, per-track sandbox survival, tempo-sync follows host BPM | TODO (task 048b) |
| Logic | macOS | AU (aumf) load, MIDI routing, project reload, bounce, autosave under 16 MB embedded state | TODO (task 048b) |
| auval | macOS | `auval -v aumf MwS1 MwSt`; MIDI reaches the plugin in Logic | Scripted: `ctest --preset default -L validators` (`run_auval.sh`) |
| Standalone | all | FX SYNC fallback with no transport (ADR-006) | TODO (task 048b) |

## Ardour LV2 smoke procedure (Linux — task 049 row)

The LV2 exporter is exercised automatically by `run_lv2_checks.sh` (lv2lint +
lv2_validate/sord_validate + a **required** headless Carla load smoke). The
Ardour row below is the manual DAW reload check (testing-strategy.md §6 Ardour
row) — record the outcome here and in the task PR.

1. Build the plugin: `cmake --build --preset linux-default -j 6`.
2. Make the bundle discoverable:
   `export LV2_PATH="$PWD/build/linux-default/plugin/mwStime_artefacts/RelWithDebInfo/LV2"`.
3. Launch Ardour, create a stereo track, insert **mwStime** (LV2).
4. Load a sample, set a stretch, audition; confirm audio passes.
5. Save the session, close, reopen → confirm the plugin reloads with its state
   (embedded sample + parameters) intact and audio still plays.
6. Record: `PASS/FAIL`, date, Ardour version, distro/kernel.

**Result (Linux Ardour LV2 load + reload):** TODO — not yet run. This task
(049) was developed on macOS arm64 (no Linux box in the dev environment); the
LV2 bundle builds cleanly on macOS via the JUCE exporter (so the exporter is NOT
broken — ADR-002 drop-LV2 escape hatch not triggered) and the scripted
lv2lint/Carla gate is wired and self-skipping. The manual Ardour reload row must
be performed on a Linux box and recorded here before the row flips to `PASS`.
