#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_lv2_checks.sh — LV2 bundle validation of the built LV2 (task
# plan/backlog/049-linux-build-lv2.md; testing-strategy.md §5 LV2 row,
# architecture.md §3, plan/decisions/002). pluginval CANNOT load LV2 — LV2 needs
# its own validators (the corrected v1 validator matrix):
#
#   1. lv2lint              — LV2 spec / Turtle metadata lint of the bundle.
#   2. lv2_validate         — RDF/Turtle validity of the bundle's *.ttl
#                             (provided by the `lilv-utils` package; it shells
#                             out to sord_validate, the SPARQL/RDF validator).
#                             If lv2_validate is absent we fall back to calling
#                             sord_validate directly on every .ttl in the bundle.
#   3. Carla load smoke      — REQUIRED (testing-strategy.md §5 LV2 row): actually
#                             LOAD the plugin headlessly with carla-single, the
#                             same gate-not-optional pattern as the other
#                             validators. It self-skips ONLY when Carla is absent.
#
# Headless boxes: every GUI-touching step is wrapped in xvfb-run when no DISPLAY
# is set and xvfb-run exists (testing-strategy.md §5 "Headless Linux runs use
# xvfb").
#
# Behaviour (CTest contract — matches the other validator scripts):
#   - exit 0  : the located LV2 bundle passed every AVAILABLE validator and at
#               least one validator actually ran.
#   - exit 1  : an available validator FAILED on the bundle.
#   - exit 77 : NONE of the validators are installed, OR no built LV2 bundle was
#               found — a SKIP (CTest SKIP_RETURN_CODE=77), so a tools-absent box
#               stays green. Each tool self-skips individually; a present tool is
#               always run (never "optional").
#
# Tools are EXPECTED FROM THE SYSTEM PACKAGE MANAGER on Linux (lv2lint,
# lilv-utils/sordi, carla) — see docs/BUILDING.md — not downloaded/pinned (unlike
# pluginval/clap-validator), because they are distro packages with no portable
# release tarball. Absent tools self-skip; they are never silently passed.

set -euo pipefail

SKIP_EXIT=77

log()  { printf 'run_lv2_checks: %s\n' "$*"; }
warn() { printf 'run_lv2_checks: %s\n' "$*" >&2; }

# --- repo / build locations -------------------------------------------------
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
build_dir="${MWS_BUILD_DIR:-${repo_root}/build/linux-default}"

# --- headless wrapper -------------------------------------------------------
# Wrap GUI-touching commands in xvfb-run when there is no DISPLAY and xvfb-run is
# available. On a box with a real display (or without xvfb) we just run directly.
xvfb=()
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
    log "no DISPLAY set; wrapping GUI steps in xvfb-run (headless)."
    xvfb=(xvfb-run -a)
fi

# --- locate the built LV2 bundle --------------------------------------------
# JUCE writes it under <build>/plugin/mwStime_artefacts/<config>/LV2/mwStime.lv2.
lv2_bundle=""
while IFS= read -r p; do
    [ -n "$p" ] && { lv2_bundle="$p"; break; }
done < <(find "${build_dir}" -type d -name '*.lv2' 2>/dev/null | sort | head -n 1)

if [ -z "${lv2_bundle}" ]; then
    warn "no built LV2 bundle (*.lv2) found under ${build_dir}."
    warn "build the plugin first: cmake --build --preset linux-default"
    warn "nothing to validate — SKIPPING (exit ${SKIP_EXIT})."
    exit "${SKIP_EXIT}"
fi
log "found LV2 bundle: ${lv2_bundle}"

# The plugin URI (must match plugin/CMakeLists.txt LV2URI).
lv2_uri="https://github.com/mattWoolly/mwStime"

# Make the bundle discoverable by URI-based tools (lv2lint, carla) on this run.
export LV2_PATH="$(dirname "${lv2_bundle}")${LV2_PATH:+:${LV2_PATH}}"
log "LV2_PATH=${LV2_PATH}"

ran=0      # how many validators actually executed (>=1 required for a PASS)
failed=0   # how many available validators failed

# --- 1) lv2lint -------------------------------------------------------------
if command -v lv2lint >/dev/null 2>&1; then
    log "running lv2lint on ${lv2_uri} ..."
    ran=$((ran + 1))
    if "${xvfb[@]}" lv2lint -s "${lv2_uri}"; then
        log "lv2lint PASS."
    else
        warn "lv2lint FAILED."
        failed=$((failed + 1))
    fi
else
    warn "lv2lint not installed — skipping this validator (install: apt-get install lv2lint)."
fi

# --- 2) lv2_validate / sord_validate ----------------------------------------
# lv2_validate (lilv-utils) validates the bundle's RDF; if it is absent, fall
# back to running sord_validate (sordi/sord package) on every .ttl directly.
if command -v lv2_validate >/dev/null 2>&1; then
    log "running lv2_validate ..."
    ran=$((ran + 1))
    # lv2_validate takes the .ttl files of the bundle.
    ttls=()
    while IFS= read -r f; do ttls+=("$f"); done \
        < <(find "${lv2_bundle}" -name '*.ttl' 2>/dev/null | sort)
    if [ "${#ttls[@]}" -eq 0 ]; then
        warn "lv2_validate: no .ttl files in the bundle — FAIL."
        failed=$((failed + 1))
    elif lv2_validate "${ttls[@]}"; then
        log "lv2_validate PASS."
    else
        warn "lv2_validate FAILED."
        failed=$((failed + 1))
    fi
elif command -v sord_validate >/dev/null 2>&1; then
    log "lv2_validate absent; running sord_validate on the bundle's .ttl ..."
    ran=$((ran + 1))
    ttls=()
    while IFS= read -r f; do ttls+=("$f"); done \
        < <(find "${lv2_bundle}" -name '*.ttl' 2>/dev/null | sort)
    if [ "${#ttls[@]}" -eq 0 ]; then
        warn "sord_validate: no .ttl files in the bundle — FAIL."
        failed=$((failed + 1))
    elif sord_validate "${ttls[@]}"; then
        log "sord_validate PASS."
    else
        warn "sord_validate FAILED."
        failed=$((failed + 1))
    fi
else
    warn "neither lv2_validate nor sord_validate installed — skipping the RDF"
    warn "  validator (install: apt-get install lilv-utils sordi)."
fi

# --- 3) Carla load smoke (REQUIRED row, self-skip only if Carla absent) -----
# Actually LOAD the LV2 in Carla headlessly. carla-single runs one plugin and
# exits; we drive it for a moment and treat a clean start/stop as a PASS. This is
# the testing-strategy.md §5 LV2-row "Carla load smoke" — a gate, never optional;
# it self-skips ONLY when Carla is not installed (the same pattern as lv2lint).
carla_bin=""
for cand in carla-single carla; do
    if command -v "${cand}" >/dev/null 2>&1; then carla_bin="${cand}"; break; fi
done

if [ -n "${carla_bin}" ]; then
    log "Carla load smoke via ${carla_bin} (headless) ..."
    ran=$((ran + 1))
    smoke_rc=0
    if [ "${carla_bin}" = "carla-single" ]; then
        # carla-single <arch> <format> <bundle> <uri>; loads, instantiates, then
        # we give it a brief window and terminate. A nonzero start is a FAIL.
        "${xvfb[@]}" timeout 25 carla-single native lv2 "${lv2_bundle}" "${lv2_uri}" \
            >/tmp/mws_carla_lv2.log 2>&1 || smoke_rc=$?
        # `timeout` returns 124 when it had to kill a still-running (= happily
        # loaded) Carla: that is the SUCCESS case for a load smoke.
        if [ "${smoke_rc}" -eq 124 ] || [ "${smoke_rc}" -eq 0 ]; then
            log "Carla loaded the LV2 cleanly (rc=${smoke_rc}) — PASS."
        else
            warn "Carla FAILED to load the LV2 (rc=${smoke_rc}); log:"
            sed 's/^/  carla: /' /tmp/mws_carla_lv2.log >&2 || true
            failed=$((failed + 1))
        fi
    else
        # Plain `carla` opening the bundle: drive headlessly for a short window.
        "${xvfb[@]}" timeout 25 carla "${lv2_bundle}" \
            >/tmp/mws_carla_lv2.log 2>&1 || smoke_rc=$?
        if [ "${smoke_rc}" -eq 124 ] || [ "${smoke_rc}" -eq 0 ]; then
            log "Carla opened the LV2 cleanly (rc=${smoke_rc}) — PASS."
        else
            warn "Carla FAILED to open the LV2 (rc=${smoke_rc}); log:"
            sed 's/^/  carla: /' /tmp/mws_carla_lv2.log >&2 || true
            failed=$((failed + 1))
        fi
    fi
    rm -f /tmp/mws_carla_lv2.log
else
    warn "Carla not installed — skipping the Carla load smoke (install: apt-get install carla)."
fi

# --- verdict ----------------------------------------------------------------
if [ "${ran}" -eq 0 ]; then
    warn "no LV2 validator available (lv2lint / lv2_validate|sord_validate / carla)"
    warn "  — SKIPPING (exit ${SKIP_EXIT}). See docs/BUILDING.md for the apt packages."
    exit "${SKIP_EXIT}"
fi

if [ "${failed}" -ne 0 ]; then
    warn "${failed} LV2 validator(s) FAILED on ${lv2_bundle}."
    exit 1
fi

log "LV2 bundle passed all ${ran} available validator(s)."
exit 0
