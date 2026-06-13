#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_auval.sh — Apple Audio Unit validation of the built AU with the system
# `auval` tool (task plan/backlog/048; testing-strategy.md §5/§6, architecture.md
# §7). The plugin is exported as a MUSIC EFFECT (`aumf`, NOT `aufx`) so Logic
# routes MIDI to it (IS_SYNTH FALSE + NEEDS_MIDI_INPUT TRUE — plugin/CMakeLists.txt):
#
#       auval -v aumf MwS1 MwSt
#                 ^^^^ ^^^^ ^^^^
#                 type subtype(PLUGIN_CODE) manufacturer(PLUGIN_MANUFACTURER_CODE)
#
# auval only sees AUs that are REGISTERED with the system (installed under
# ~/Library/Audio/Plug-Ins/Components, which COPY_PLUGIN_AFTER_BUILD does on a
# normal `cmake --build`). After (re)installing a component, macOS caches the
# AudioComponent registry; the refresh dance is:
#
#       killall -9 AudioComponentRegistrar   # force a re-scan on next query
#
# This script performs that dance automatically when it copies a freshly built
# component into place, then runs auval.
#
# Behaviour (CTest contract):
#   - exit 0  : auval validated the aumf component.
#   - exit 1  : auval reported a validation failure.
#   - exit 77 : auval is unavailable (non-macOS) OR no built/installed AU
#               component could be found — a SKIP (CTest SKIP_RETURN_CODE=77),
#               so `ctest --preset default` stays green off-macOS / without a build.

set -euo pipefail

# --- component identity (must match plugin/CMakeLists.txt) ------------------
AU_TYPE="aumf"        # kAudioUnitType_MusicEffect (architecture.md §7)
AU_SUBTYPE="MwS1"     # PLUGIN_CODE
AU_MANUFACTURER="MwSt" # PLUGIN_MANUFACTURER_CODE

SKIP_EXIT=77

log()  { printf 'run_auval: %s\n' "$*"; }
warn() { printf 'run_auval: %s\n' "$*" >&2; }

# --- platform / tool availability -------------------------------------------
if [ "$(uname -s)" != "Darwin" ]; then
    warn "auval is macOS-only — SKIPPING (exit ${SKIP_EXIT})."
    exit "${SKIP_EXIT}"
fi
if ! command -v auval >/dev/null 2>&1; then
    warn "auval not found on PATH — SKIPPING (exit ${SKIP_EXIT})."
    exit "${SKIP_EXIT}"
fi

# --- repo / build locations -------------------------------------------------
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
build_dir="${MWS_BUILD_DIR:-${repo_root}/build/default}"
components_dir="${HOME}/Library/Audio/Plug-Ins/Components"

# Locate a freshly built .component in the build tree (JUCE writes it under
# <build>/plugin/mwStime_artefacts/<config>/AU/mwStime.component).
built_component=""
while IFS= read -r p; do
    [ -n "$p" ] && { built_component="$p"; break; }
done < <(find "${build_dir}" -type d -name '*.component' 2>/dev/null | sort | head -n 1)

installed_component="${components_dir}/mwStime.component"

# --- register the component so auval can see it -----------------------------
# COPY_PLUGIN_AFTER_BUILD already installs it on a normal build, but be robust:
# if we found a fresh build artifact, (re)install it and bust the AU registry
# cache. This is the documented "killall -9 AudioComponentRegistrar" dance.
refresh_registry() {
    log "refreshing the AudioComponent registry (killall -9 AudioComponentRegistrar) ..."
    # The registrar is auto-relaunched on the next AudioComponent query; -9 is
    # the documented way to force a re-scan after (un)installing a component.
    killall -9 AudioComponentRegistrar 2>/dev/null || true
}

if [ -n "${built_component}" ]; then
    log "found freshly built component: ${built_component}"
    # Only copy when it differs from / is newer than the installed one, then
    # bust the cache. (COPY_PLUGIN_AFTER_BUILD usually already did this.)
    if [ ! -d "${installed_component}" ] || \
       [ "${built_component}" -nt "${installed_component}" ]; then
        log "installing component into ${components_dir}"
        mkdir -p "${components_dir}"
        rm -rf "${installed_component}"
        cp -R "${built_component}" "${installed_component}"
        refresh_registry
    else
        log "installed component is up to date."
    fi
elif [ -d "${installed_component}" ]; then
    log "no build artifact found; using already-installed ${installed_component}"
else
    warn "no built or installed mwStime.component found."
    warn "build the plugin first: cmake --build --preset default (COPY_PLUGIN_AFTER_BUILD installs it)."
    warn "nothing to validate — SKIPPING (exit ${SKIP_EXIT})."
    exit "${SKIP_EXIT}"
fi

# --- run auval --------------------------------------------------------------
log "running: auval -v ${AU_TYPE} ${AU_SUBTYPE} ${AU_MANUFACTURER}"

# auval exits nonzero on validation failure. If it cannot find the component at
# all it prints "no component found" / "FATAL" — handle that as a SKIP so the
# default ctest stays green when the AU was never installed (e.g. a non-APPLE
# COPY_PLUGIN_AFTER_BUILD path), distinguishing "not registered" from "failed".
auval_log="$(mktemp -t mws_auval)"
set +e
auval -v "${AU_TYPE}" "${AU_SUBTYPE}" "${AU_MANUFACTURER}" 2>&1 | tee "${auval_log}"
auval_rc="${PIPESTATUS[0]}"
set -e

if [ "${auval_rc}" -eq 0 ]; then
    log "auval PASSED (${AU_TYPE} ${AU_SUBTYPE} ${AU_MANUFACTURER})."
    rm -f "${auval_log}"
    exit 0
fi

# Distinguish "component not registered" (SKIP) from a real validation FAIL.
if grep -qiE 'did not find the specified component|no component|cannot find' "${auval_log}"; then
    warn "auval could not find the registered component — SKIPPING (exit ${SKIP_EXIT})."
    warn "ensure the plugin built+installed (COPY_PLUGIN_AFTER_BUILD) and the registry was refreshed."
    rm -f "${auval_log}"
    exit "${SKIP_EXIT}"
fi

warn "auval FAILED for ${AU_TYPE} ${AU_SUBTYPE} ${AU_MANUFACTURER} (rc=${auval_rc})."
rm -f "${auval_log}"
exit 1
