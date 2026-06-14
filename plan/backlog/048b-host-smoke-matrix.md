---
id: 048b
title: Host smoke test matrix — host-matrix.md + scripted/manual macOS host runs
status: done
depends-on: [046, 048]
component: qa
estimated-size: M
---

## Objective
The testing-strategy §6 host smoke matrix exists as a living checklist file with the
macOS rows executed and recorded: REAPER (automation/state/latency/tempo checks),
Logic (aumf MIDI routing, reload, bounce, autosave-stall check), Bitwig CLAP, and
Standalone no-transport SYNC fallback. Linux rows are defined here and executed in
task 049 / the QA phase.

## Context
Read first:
- docs/design/testing-strategy.md §6 (the host/checks table — transcribe it; checks
  map 1:1 to ui-design §6 flows)
- docs/design/architecture.md §5.2 (latency re-report on model switch = the REAPER
  PDC check), §6 (cached FLAC blob = the Logic autosave-stall check)
- plan/decisions/006 (no-transport SYNC fallback — the Standalone row)
- Model switching/PDC (046), validator scripts pattern (048)

## Scope
- `tests/plugin/host-matrix.md`: every §6 row (auval, REAPER macOS+Linux, Bitwig,
  Ardour, Carla, Logic, Standalone) with its exact checks as checkbox items, a
  results column (date/version/pass-fail/notes), and which rows are scripted vs
  manual. Linux rows marked "executed in task 049 (Carla/Ardour) / QA phase".
- Scripted where possible (per the design: "scripted where possible, checklist
  otherwise"): a REAPER headless/batch script driving insert + automation write/read
  of timeFactor + state save/reload + offline-render-vs-realtime compare + T=100%
  PDC null + latency re-report on model switch + mid-play tempo change (SYNC window
  follows) + transport stop/loop while windowed — self-skips if REAPER is absent
  (exit 77 pattern from 048).
- Manual rows executed on macOS and recorded in the matrix: Logic (aumf load, MIDI
  reaches the plugin in a music-effect slot, project reload, bounce, autosave with
  ~16 MB embedded state — no message-thread stall), Bitwig (CLAP load, sandbox
  survival, tempo-sync follows BPM change), Standalone (FX SYNC fallback with no
  transport).
- Findings filed as follow-up tasks with repro steps; the matrix links them.

## Out of scope
- Linux host execution (Ardour/Carla land with task 049; full Linux matrix is QA
  phase).
- Fixing non-trivial host bugs (file follow-ups).
- CI (hosts are not CI-runnable; 051 must not reference this).

## CRITICAL execution constraints (headless-safe — read before running anything)
- **NEVER install a DAW** (no brew/cask/apt installs, no downloads of REAPER/Logic/
  Bitwig/Ardour) and **NEVER launch a GUI host interactively** — launching a GUI DAW
  blocks forever in this non-interactive environment and is what previously hung this
  task. Probe for a host ONLY via non-blocking PATH/`/Applications` checks
  (`command -v`, `ls /Applications`), never by starting it.
- If a host binary is absent (or only launchable as a GUI), do NOT attempt to run it:
  record that matrix row as `not available on this host — deferred to QA` with the
  date, and have any scripted runner for it **self-skip via exit 77** (the CTest
  SKIP_RETURN_CODE pattern from 048). A clean skip is a PASS for this task.
- The ONLY rows that must actually EXECUTE here are: (a) `auval -v aumf <ids>` (already
  available via the AU toolchain), and (b) a **headless Standalone** check of the
  no-transport SYNC fallback that runs without opening a window (drive the audio
  processor directly / `--headless`-style, or a unit-style harness — never a visible
  app window). Everything else (REAPER/Logic/Bitwig/Ardour) is scripted-but-self-
  skipping + recorded as deferred.
- The REAPER batch script must be written and wired to CTest label `host-smoke`, but
  it must self-skip cleanly (exit 77) when REAPER is not on PATH — do not block waiting
  for it.

## Acceptance criteria
- [ ] host-matrix.md committed with every testing-strategy §6 row and check.
- [ ] REAPER script runs locally (or self-skips cleanly when REAPER absent);
      registered with CTest label `host-smoke`.
- [ ] All macOS rows executed once with results recorded in the matrix + PR.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -L host-smoke --no-tests=error
cat tests/plugin/host-matrix.md   # all §6 rows present, macOS results filled in
```
