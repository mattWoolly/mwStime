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

# --- pinned clap-validator release (free-audio), selected PER OS ------------
CLAP_VALIDATOR_VERSION="0.3.2"
case "$(uname -s)" in
    Darwin)
        CLAP_VALIDATOR_URL="https://github.com/free-audio/clap-validator/releases/download/${CLAP_VALIDATOR_VERSION}/clap-validator-${CLAP_VALIDATOR_VERSION}-macos-universal.tar.gz"
        # SHA-256 of the macOS-universal tarball for the pinned tag (verified at authoring time).
        CLAP_VALIDATOR_SHA256="3750f3729adfd8489f2b29019f7f2ed65ba71bf9d5049735f6a2ca0fccb18ffd"
        CLAP_VALIDATOR_EXE_REL="binaries/clap-validator"
        SHASUM=(shasum -a 256)
        ;;
    Linux)
        CLAP_VALIDATOR_URL="https://github.com/free-audio/clap-validator/releases/download/${CLAP_VALIDATOR_VERSION}/clap-validator-${CLAP_VALIDATOR_VERSION}-ubuntu-18.04.tar.gz"
        # SHA-256 of the Linux tarball for the pinned tag. NOTE (task 049): this
        # placeholder is verified-on-first-fetch on a Linux box — replace with the
        # real digest when blessing the Linux toolchain (see docs/BUILDING.md). A
        # mismatch is a HARD FAIL (never a silent skip), so a stale pin surfaces
        # loudly rather than running an unverified binary.
        CLAP_VALIDATOR_SHA256="${MWS_CLAP_VALIDATOR_LINUX_SHA256:-UNVERIFIED_SET_ON_LINUX}"
        CLAP_VALIDATOR_EXE_REL="binaries/clap-validator"
        SHASUM=(sha256sum)
        ;;
    *)
        printf 'run_clap_validator: unsupported OS %s — SKIPPING (exit 77).\n' "$(uname -s)" >&2
        exit 77
        ;;
esac

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
    got="$("${SHASUM[@]}" "${tarball}" | awk '{print $1}')"
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
    # (no-op / absent on Linux — guarded by command -v.)
    if command -v xattr >/dev/null 2>&1; then
        xattr -dr com.apple.quarantine "${dest_dir}" 2>/dev/null || true
    fi

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

# --- headless wrapper (Linux, testing-strategy.md §5) -----------------------
# clap-validator instantiates the plugin; on a headless Linux box the editor
# needs a virtual X server. Wrap it in xvfb-run when no DISPLAY is set and
# xvfb-run is available. macOS / a real display: run directly.
xvfb=()
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
    log "no DISPLAY set; wrapping clap-validator in xvfb-run (headless)."
    xvfb=(xvfb-run -a)
fi

# --- validate ---------------------------------------------------------------
log "validating: ${clap_bundle}"
# `validate` runs the full default test suite; nonzero exit on any failed test.
# (${arr[@]:+...} keeps this safe under `set -u` when the xvfb array is empty.)
if ${xvfb[@]:+"${xvfb[@]}"} "${validator_exe}" validate "${clap_bundle}"; then
    log "PASS: ${clap_bundle}"
    exit 0
fi

warn "FAIL: ${clap_bundle} did not pass clap-validator ${CLAP_VALIDATOR_VERSION}."
exit 1
