#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_golden_case.sh — one golden-render regression case (testing-strategy.md §4
# runner; task plan/backlog/026-golden-harness.md). Renders a single case with
# mwstime-render into a private scratch file, then gates it against the blessed
# render with golden_compare. Registered once per case by tests/golden/CMakeLists.txt
# under CTest LABEL `golden`, with the test name prefixed `golden_<model>_<case>`.
#
# Usage:
#   run_golden_case.sh <render-bin> <compare-bin> <cases.json> <inputs-dir> \
#                      <blessed-dir> <scratch-dir> <case-id> <policy> [tol]
#
# policy : exact | tolerance   (from cases.json; selects the golden_compare gate)
# tol    : optional per-sample tolerance for the tolerance policy (default 1e-6;
#          the reference platform blesses bit-exact so 0 is also valid here).
#
# Exit 0 = case matches the blessed render within policy; nonzero otherwise (the
# render refusal / mismatch diagnostics are printed by the sub-tools so they land
# in the CTest --output-on-failure log without DAW access).

set -euo pipefail

if [ "$#" -lt 8 ]; then
    echo "usage: run_golden_case.sh <render-bin> <compare-bin> <cases.json> \
<inputs-dir> <blessed-dir> <scratch-dir> <case-id> <policy> [tol]" >&2
    exit 2
fi

render_bin="$1"
compare_bin="$2"
cases_json="$3"
inputs_dir="$4"
blessed_dir="$5"
scratch_dir="$6"
case_id="$7"
policy="$8"
tol="${9:-1e-6}"

mkdir -p "$scratch_dir"
candidate="$scratch_dir/${case_id}.wav"
blessed="$blessed_dir/${case_id}.wav"

if [ ! -f "$blessed" ]; then
    echo "run_golden_case: NO BLESSED RENDER for '$case_id' ($blessed)." >&2
    echo "  Bless the goldens first: BLESS_REASON=\"...\" cmake --build --preset default --target bless_goldens" >&2
    exit 1
fi

# 1) Render the candidate (16-bit per the case's declared bitDepth).
"$render_bin" --case "$case_id" --cases "$cases_json" \
    --inputs-dir "$inputs_dir" --out "$candidate"

# 2) Gate against the blessed render with the case's comparison policy.
if [ "$policy" = "exact" ]; then
    "$compare_bin" --candidate "$candidate" --blessed "$blessed" --policy exact
else
    "$compare_bin" --candidate "$candidate" --blessed "$blessed" \
        --policy tolerance --tol "$tol"
fi
