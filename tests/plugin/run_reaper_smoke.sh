#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_reaper_smoke.sh — the REAPER row of the host smoke matrix
# (plan/backlog/048b-host-smoke-matrix.md; testing-strategy.md §6 REAPER row),
# scripted-but-self-skipping. REAPER ships a batch/headless runner (`reaper
# -batchconvert` / a ReaScript driven by `-new -nosplash`), so the REAPER row is
# the one DAW row that CAN be scripted "where possible" (the §6 policy). The
# scripted run inserts the FX and exercises:
#
#   1. insert mwStime as a track FX
#   2. automation write/read of timeFactor
#   3. project state save + reload (with the embedded sample) — architecture.md §6
#   4. offline render == realtime output (offline-vs-realtime null)
#   5. PDC null at T=100% (latency compensation) — testing-strategy.md §3.4a
#   6. latency RE-REPORT on model switch is honoured — architecture.md §5.2 PDC
#   7. tempo change mid-play → the SYNC window follows — ADR-006 SYNC
#   8. transport stop / loop while the FX is windowed
#
# The actual ReaScript that performs 1–8 is filed as a follow-up (it needs a
# REAPER install to author + a blessed reference render); this runner is the
# CTest wiring + the headless-safe HOST PROBE. Per the task's CRITICAL execution
# constraints it NEVER installs REAPER and NEVER launches a GUI host — it probes
# for the binary via non-blocking PATH / /Applications checks ONLY. When REAPER
# is absent (the norm in this and in CI environments) it SELF-SKIPS.
#
# Behaviour (CTest contract — mirrors run_auval.sh / run_pluginval.sh):
#   - exit 77 : REAPER is not available (not on PATH and not in /Applications),
#               or only launchable as a GUI — a SKIP (CTest SKIP_RETURN_CODE=77),
#               so `ctest -L host-smoke` stays GREEN on a REAPER-absent box. A
#               clean skip is a PASS for task 048b.
#   - exit 0  : the headless REAPER smoke run completed and all checks passed
#               (only reachable where REAPER + the ReaScript are present).
#   - exit 1  : REAPER ran the smoke script and a check FAILED.

set -euo pipefail

SKIP_EXIT=77

log()  { printf 'run_reaper_smoke: %s\n' "$*"; }
warn() { printf 'run_reaper_smoke: %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Headless-safe host probe. We must NEVER start REAPER to detect it (launching a
# GUI DAW blocks forever in a non-interactive environment — the task's documented
# hang). Probe ONLY via PATH and the macOS /Applications bundle path. We do NOT
# execute the located binary; finding it merely records that a future
# fully-authored ReaScript COULD run here.
# ---------------------------------------------------------------------------
reaper_bin=""

# 1. PATH (Linux installs, or a symlinked macOS CLI).
if command -v reaper >/dev/null 2>&1; then
    reaper_bin="$(command -v reaper)"
fi

# 2. macOS application bundle (do NOT `open` it — just stat the executable).
if [ -z "${reaper_bin}" ] && [ "$(uname -s)" = "Darwin" ]; then
    for app in \
        "/Applications/REAPER.app/Contents/MacOS/REAPER" \
        "${HOME}/Applications/REAPER.app/Contents/MacOS/REAPER"; do
        if [ -x "${app}" ]; then
            reaper_bin="${app}"
            break
        fi
    done
fi

# 3. Common Linux install locations not on PATH.
if [ -z "${reaper_bin}" ] && [ "$(uname -s)" = "Linux" ]; then
    for cand in /opt/REAPER/reaper "${HOME}/opt/REAPER/reaper"; do
        if [ -x "${cand}" ]; then
            reaper_bin="${cand}"
            break
        fi
    done
fi

if [ -z "${reaper_bin}" ]; then
    warn "REAPER not found on PATH or in /Applications — SKIPPING (exit ${SKIP_EXIT})."
    warn "this row is deferred to QA on a box with REAPER (host-matrix.md REAPER row)."
    exit "${SKIP_EXIT}"
fi

# ---------------------------------------------------------------------------
# REAPER IS present. Locate the scripted-smoke ReaScript; until it is authored
# (follow-up task — it needs a REAPER install to write + a blessed reference),
# self-skip so the row stays a clean SKIP rather than a false failure. Authoring
# the ReaScript here would require launching REAPER, which the constraints forbid.
# ---------------------------------------------------------------------------
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
reascript="${script_dir}/reaper_smoke.lua"

log "found REAPER: ${reaper_bin}"
if [ ! -f "${reascript}" ]; then
    warn "REAPER is present but the smoke ReaScript (${reascript}) is not yet"
    warn "authored (needs a REAPER install to write + bless) — SKIPPING (exit ${SKIP_EXIT})."
    warn "see host-matrix.md REAPER row / the filed follow-up for the script spec."
    exit "${SKIP_EXIT}"
fi

# ---------------------------------------------------------------------------
# Drive REAPER HEADLESS (batch). `-new -nosplash` + a ReaScript that exits the
# app is non-interactive; we never open the editor GUI. The ReaScript performs
# checks 1–8 above and returns 0/1 via its own exit-status file.
# ---------------------------------------------------------------------------
build_dir="${MWS_BUILD_DIR:-${script_dir}/../../build/default}"
log "running headless REAPER smoke: ${reaper_bin} -new -nosplash <script>"

status_file="$(mktemp -t mws_reaper_smoke)"
set +e
MWS_BUILD_DIR="${build_dir}" MWS_REAPER_STATUS="${status_file}" \
    "${reaper_bin}" -new -nosplash -ignoreerrors "${reascript}"
reaper_rc=$?
set -e

# The ReaScript writes PASS/FAIL into MWS_REAPER_STATUS; REAPER's own process
# exit code is unreliable for batch scripts, so trust the status file.
result="$(cat "${status_file}" 2>/dev/null || true)"
rm -f "${status_file}"

if [ "${result}" = "PASS" ]; then
    log "REAPER smoke PASSED (checks 1-8)."
    exit 0
fi

warn "REAPER smoke FAILED (script result='${result}', reaper rc=${reaper_rc})."
exit 1
