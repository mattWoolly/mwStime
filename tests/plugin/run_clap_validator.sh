#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_clap_validator.sh — validation of the built CLAP with a PINNED
# clap-validator release (task plan/backlog/048; testing-strategy.md §5,
# architecture.md §3). pluginval CANNOT load CLAP — CLAP needs its own validator
# (the corrected v1 validator matrix).
#
# Behaviour (CTest contract):
#   - exit 0  : the built .clap passed clap-validator.
#   - exit 1  : the built .clap FAILED validation.
#   - exit 77 : the pinned clap-validator could not be fetched (offline) OR no
#               built .clap was found — a SKIP (CTest SKIP_RETURN_CODE=77), so
#               `ctest --preset default` stays green offline / without a build.
#
# The pinned binary is cached under build/ (never committed; build/ is in
# .gitignore). The pin and its SHA-256 are below — bumping them is a deliberate
# change (acceptance criterion: versions pinned here).

set -euo pipefail

# --- pinned clap-validator release (free-audio) -----------------------------
CLAP_VALIDATOR_VERSION="0.3.2"
CLAP_VALIDATOR_URL="https://github.com/free-audio/clap-validator/releases/download/${CLAP_VALIDATOR_VERSION}/clap-validator-${CLAP_VALIDATOR_VERSION}-macos-universal.tar.gz"
# SHA-256 of the macOS-universal tarball for the pinned tag (verified at authoring time).
CLAP_VALIDATOR_SHA256="3750f3729adfd8489f2b29019f7f2ed65ba71bf9d5049735f6a2ca0fccb18ffd"
# Path to the executable inside the unpacked tarball.
CLAP_VALIDATOR_EXE_REL="binaries/clap-validator"

SKIP_EXIT=77

log()  { printf 'run_clap_validator: %s\n' "$*"; }
warn() { printf 'run_clap_validator: %s\n' "$*" >&2; }

# --- repo / build locations -------------------------------------------------
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
build_dir="${MWS_BUILD_DIR:-${repo_root}/build/default}"
tools_cache="${repo_root}/build/validator-tools"
mkdir -p "${tools_cache}"

# --- fetch the pinned clap-validator (skip cleanly if offline) --------------
validator_exe="${tools_cache}/clap-validator-${CLAP_VALIDATOR_VERSION}/${CLAP_VALIDATOR_EXE_REL}"

fetch_validator() {
    if [ -x "${validator_exe}" ]; then
        log "using cached clap-validator ${CLAP_VALIDATOR_VERSION} (${validator_exe})"
        return 0
    fi

    local dest_dir="${tools_cache}/clap-validator-${CLAP_VALIDATOR_VERSION}"
    local tarball="${tools_cache}/clap-validator-${CLAP_VALIDATOR_VERSION}.tar.gz"
    log "fetching pinned clap-validator ${CLAP_VALIDATOR_VERSION} ..."
    if ! curl -fsSL --retry 2 -o "${tarball}" "${CLAP_VALIDATOR_URL}"; then
        warn "could not download clap-validator (${CLAP_VALIDATOR_URL}) — treating as OFFLINE SKIP."
        rm -f "${tarball}"
        return 1
    fi

    local got
    got="$(shasum -a 256 "${tarball}" | awk '{print $1}')"
    if [ "${got}" != "${CLAP_VALIDATOR_SHA256}" ]; then
        warn "clap-validator checksum mismatch! expected ${CLAP_VALIDATOR_SHA256}, got ${got}."
        warn "refusing to run an unverified binary."
        rm -f "${tarball}"
        return 2
    fi

    rm -rf "${dest_dir}"
    mkdir -p "${dest_dir}"
    if ! tar -xzf "${tarball}" -C "${dest_dir}"; then
        warn "failed to extract clap-validator — treating as SKIP."
        rm -f "${tarball}"
        return 1
    fi
    rm -f "${tarball}"
    chmod +x "${validator_exe}" 2>/dev/null || true
    # macOS Gatekeeper: strip the quarantine bit so the downloaded binary runs.
    xattr -dr com.apple.quarantine "${dest_dir}" 2>/dev/null || true

    if [ ! -x "${validator_exe}" ]; then
        warn "clap-validator executable not found after extract (${validator_exe}) — SKIP."
        return 1
    fi
    log "fetched clap-validator ${CLAP_VALIDATOR_VERSION}."
    return 0
}

if ! fetch_validator; then
    rc=$?
    if [ "${rc}" = "2" ]; then
        exit 1  # checksum mismatch is a hard failure, not a skip
    fi
    warn "clap-validator unavailable — SKIPPING (exit ${SKIP_EXIT})."
    exit "${SKIP_EXIT}"
fi

# --- locate the built .clap -------------------------------------------------
# JUCE/clap-juce-extensions writes it under
# <build>/plugin/mwStime_artefacts/<config>/CLAP/mwStime.clap.
clap_bundle=""
while IFS= read -r p; do
    [ -n "$p" ] && { clap_bundle="$p"; break; }
done < <(find "${build_dir}" -name '*.clap' 2>/dev/null | sort | head -n 1)

if [ -z "${clap_bundle}" ]; then
    warn "no built .clap found under ${build_dir}."
    warn "build the plugin first: cmake --build --preset default"
    warn "nothing to validate — SKIPPING (exit ${SKIP_EXIT})."
    exit "${SKIP_EXIT}"
fi

# --- validate ---------------------------------------------------------------
log "validating: ${clap_bundle}"
# `validate` runs the full default test suite; nonzero exit on any failed test.
if "${validator_exe}" validate "${clap_bundle}"; then
    log "PASS: ${clap_bundle}"
    exit 0
fi

warn "FAIL: ${clap_bundle} did not pass clap-validator ${CLAP_VALIDATOR_VERSION}."
exit 1
