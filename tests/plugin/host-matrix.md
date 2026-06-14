<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly. -->

# Host smoke-test matrix

Living checklist for the host smoke tests of **testing-strategy.md §6** (the
host/checks table is transcribed below, one row per host, each §6 check as a
checkbox). Soft gate during development → hard gate at release (§1 layer 6).
The smoke checks map 1:1 to the **ui-design.md §6 interaction flows** (load
sample, set stretch, audition/render, FX mode, model switch).

**Policy** (§6): *scripted where possible, checklist otherwise.* This file
records, per row, whether it is **scripted** (a self-skipping CTest under label
`host-smoke` / `validators`) or **manual** (a human DAW run), and the result
(date / plugin version / pass-fail / notes).

## How to run the scripted rows

```
cmake --preset default
cmake --build --preset default
ctest --preset default -L host-smoke --no-tests=error   # Standalone + REAPER rows
ctest --preset default -L validators --no-tests=error   # auval / pluginval / clap / lv2
```

Every scripted runner **self-skips with exit 77** (CTest `SKIP_RETURN_CODE 77`)
when its host/tool is absent — a clean skip is GREEN. **No runner ever installs
a DAW or launches a GUI host** (task 048b CRITICAL execution constraints): hosts
are probed via non-blocking PATH / `/Applications` checks only.

## Status legend

- `PASS` — performed, all listed checks green (date / host / platform recorded).
- `SKIP` — scripted row self-skipped cleanly (host/tool absent on this box);
  exit 77. A clean skip is a PASS for the development gate.
- `DEFERRED` — must be performed on a box/host not available in the dev
  environment (recorded here when it flips to `PASS`).
- `TODO` — not yet attempted.
- `N/A` — not applicable on this platform.

## Scripted vs manual + execution

| Host | Platform | Scripted? | Runner / harness | Label |
|---|---|---|---|---|
| auval | macOS | **scripted** | `run_auval.sh` (`auval -v aumf MwS1 MwSt`) | `validators` |
| REAPER | macOS, Linux | **scripted** (batch/ReaScript) | `run_reaper_smoke.sh` | `host-smoke` |
| Standalone | all | **scripted** (headless engine harness) | `test_host_smoke.cpp` | `host-smoke` |
| Bitwig | macOS, Linux | manual (GUI; sandboxed) | checklist below | — |
| Logic | macOS | manual (GUI) | checklist below | — |
| Ardour | Linux | manual (GUI) | checklist below (task 049) | — |
| Carla / lv2lint | Linux | **scripted** | `run_lv2_checks.sh` (lv2lint + lv2_validate + Carla load) | `validators` |

---

## §6 rows and checks

### auval — macOS (scripted: `run_auval.sh`, label `validators`)

- [x] `auval -v aumf MwS1 MwSt` passes (note `aumf` — **music effect**, not
      `aufx`; architecture.md §7).
- [x] AU loads as a music-effect slot so MIDI can reach the plugin (the Logic
      MIDI-routing precondition; the MIDI-reaches-the-plugin behaviour itself is
      the Logic manual row).

**Result:** `PASS` — 2026-06-13, macOS 15.3.1 (24D70), plugin 0.1.0 (aumf
MwS1 MwSt). `auval` reported **AU VALIDATION SUCCEEDED** (format/parameter/
MIDI/render-quality checks all PASS). Re-runnable: `ctest --preset default -L
validators --no-tests=error`.

### REAPER — macOS, Linux (scripted: `run_reaper_smoke.sh`, label `host-smoke`)

Insert mwStime as a track FX, then:

- [ ] automation **write/read** of `timeFactor`.
- [ ] project **state save / reload** with the embedded sample (architecture.md
      §6 cached FLAC blob — the autosave-stall concern).
- [ ] **offline render == realtime** output (offline-vs-realtime null;
      testing-strategy.md §3.4b stream/offline equivalence in a real host).
- [ ] **PDC null at T=100%** — latency compensation gives a sample-exact null
      against the dry signal (testing-strategy.md §3.4a).
- [ ] **latency re-report on model switch honoured** (architecture.md §5.2 —
      model switch is a documented PDC-updating action; ui-design.md §6.5).
- [ ] **tempo change mid-play** → the SYNC window follows the new BPM (ADR-006
      SYNC column; the core math is unit-tested in `test_rtstretch_sync.cpp`,
      this row confirms a real host drives it).
- [ ] **transport stop / loop** while the FX is windowed (no stall, window
      re-arms on loop).

**Result:** `SKIP` (scripted self-skip) — 2026-06-13, macOS 15.3.1, REAPER
7.72 present at `/Applications/REAPER.app`. `run_reaper_smoke.sh` probes for
REAPER **without launching it** (the GUI must never be opened — task 048b
constraint) and self-skips with exit 77 because the smoke **ReaScript**
(`reaper_smoke.lua`) is not yet authored: authoring/blessing it requires an
interactive REAPER session (forbidden here). Filed as the follow-up below;
**deferred to QA** on a box where REAPER may be driven. The CTest wiring + the
headless-safe host probe are in place and GREEN.

### Bitwig — macOS, Linux (manual; GUI, sandboxed)

- [ ] **CLAP load** (Bitwig is the CLAP reference host; format validated by
      `run_clap_validator.sh`, label `validators`).
- [ ] **per-track sandbox survival** (plugin keeps running / recovers in
      Bitwig's per-plugin sandbox).
- [ ] **tempo-sync follows host BPM change** (SYNC window tracks Bitwig's
      transport tempo).

**Result:** `DEFERRED` — 2026-06-13, macOS 15.3.1. Manual GUI row; launching a
GUI DAW blocks forever in this non-interactive dev environment (task 048b
constraint). The CLAP *format* is validated headlessly (`clap-validator`, label
`validators`). The Bitwig in-host load + sandbox + tempo-follow checks must be
performed interactively in the QA phase and recorded here.

### Logic — macOS (manual; GUI)

- [ ] **AU (aumf) load** in a music-effect slot.
- [ ] **MIDI routing** — MIDI notes reach the plugin (root C3 audition path,
      architecture.md §4.2).
- [ ] **project reload** restores plugin state (params + embedded sample).
- [ ] **bounce** (offline render through the host) succeeds.
- [ ] **autosave under ~16 MB embedded state — no message-thread stall**: the
      FLAC blob is pre-encoded + cached on the loader thread, so
      `getStateInformation` only memcpys it (architecture.md §6 cached blob /
      panel host-autosave critique). Logic autosaves aggressively — confirm no
      audible dropout / spinner during autosave with a large embedded sample.

**Result:** `DEFERRED` — 2026-06-13, macOS 15.3.1. Manual GUI row (cannot
launch Logic headlessly). The `aumf` format + MIDI-effect categorisation are
already proven headlessly by the **auval PASS** above (music-effect slot, MIDI
test green); the state-blob caching that prevents the autosave stall is
unit-tested in `tests/plugin/test_state_embed.cpp`. The in-Logic load / reload /
bounce / live-autosave checks must be performed interactively in the QA phase
and recorded here.

### Standalone — all platforms (scripted: `test_host_smoke.cpp`, label `host-smoke`)

- [x] **FX SYNC fallback with no transport** (ADR-006 "No transport /
      Standalone": SYNC falls back to a wall-clock window at the last-known
      tempo, else 120 BPM (PI); architecture.md §5.2). The Standalone host has
      no bar grid, so a SYNC-mode insert must fall back — not stall, not
      free-run.
- [x] the no-transport fallback **keeps producing audio** (an FX insert that
      goes silent in Standalone would be a bug) and emits **no NaN/inf**.
- [x] a Standalone host with a **stopped** play head (no tempo) uses the same
      wall-clock fallback.

**Result:** `PASS` — 2026-06-13, macOS 15.3.1, plugin 0.1.0. Verified
**headlessly** (the task's required executing row): `test_host_smoke.cpp` drives
the JUCE-free `RealtimeStretcher` — the exact engine the Standalone build runs —
with **no window and no play head**, asserting wall-clock window boundaries
(1 bar = 96000 samples @ 48 kHz / 88200 @ 44.1 kHz), audio liveness, and
finiteness. 3 cases, all green. Re-runnable: `ctest --preset default -L
host-smoke --no-tests=error`.

### Ardour — Linux (manual; GUI — owned by task 049)

- [ ] **LV2 load** (manifest validity is scripted by `run_lv2_checks.sh`).
- [ ] **session reload** restores plugin state.

**Result:** `DEFERRED` to task 049 / QA — Linux GUI row, no Linux box in the
macOS dev environment. The LV2 manifest + a headless Carla load smoke are
scripted (`run_lv2_checks.sh`, label `validators`). See the Ardour LV2 procedure
below (task 049 row).

### Carla / lv2lint — Linux (scripted: `run_lv2_checks.sh`, label `validators`)

- [ ] **LV2 manifest validity** (lv2lint + lv2_validate / sord_validate).
- [ ] headless **Carla load smoke** (xvfb-wrapped on headless boxes).

**Result:** `DEFERRED` to task 049 / QA (Linux). Scripted + self-skipping;
runs on a Linux box via `ctest --preset linux-default -L validators
--no-tests=error`.

---

## Filed follow-ups (findings link back here)

- **FOLLOW-UP — author `tests/plugin/reaper_smoke.lua`** (the REAPER ReaScript
  performing checks 1–8 of the REAPER row above, plus a blessed reference render
  for the offline-vs-realtime null). Needs an interactive REAPER install to
  author + bless; out of scope for task 048b (which must not launch a GUI). Once
  landed, `run_reaper_smoke.sh` runs it headlessly and the REAPER row flips from
  `SKIP` to `PASS`. *No host bug was found while wiring this row — REAPER 7.72
  is present and the format validators (auval/pluginval/clap) are all green.*

> No host bugs were discovered during the macOS rows executed in task 048b
> (auval PASS, Standalone PASS). Any future finding from a manual/scripted row
> is filed as its own backlog task with repro steps and linked from the row's
> Result line above.

---

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

**Result (Linux Ardour LV2 load + reload):** `DEFERRED` — not yet run (task
049 was developed on macOS arm64; no Linux box in the dev environment). The LV2
bundle builds cleanly on macOS via the JUCE exporter (so the exporter is NOT
broken — ADR-002 drop-LV2 escape hatch not triggered) and the scripted
lv2lint/Carla gate is wired and self-skipping. The manual Ardour reload row must
be performed on a Linux box and recorded here before the row flips to `PASS`.
