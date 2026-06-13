#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_ui_golden_case.sh — one UI golden-screenshot regression case (task 047b;
# docs/design/ui-design.md §4, docs/design/testing-strategy.md §7 Wave 3,
# ADR-005). Captures a single state with the ui_screenshot driver into a private
# scratch PNG, then gates it against the blessed PNG.
#
# The comparison policy is chosen AUTOMATICALLY by platform, never silently
# skipped (scope): on the reference platform (macOS arm64) the gate is
# byte/pixel-EXACT; on any other platform it downgrades to the TOLERANCE compare
# (per-pixel delta + percentage-different threshold) because cross-platform AA /
# font rasterization is not deterministic (ADR-005). `ui_screenshot
# --is-reference-platform` (compiled-in detection) decides which.
#
# Usage:
#   run_ui_golden_case.sh <ui-screenshot-bin> <blessed-dir> <scratch-dir> <state-id>
#
# Exit 0 = the capture matches the blessed PNG within the platform's policy.

set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: run_ui_golden_case.sh <ui-screenshot-bin> <blessed-dir> \
<scratch-dir> <state-id>" >&2
    exit 2
fi

bin="$1"
blessed_dir="$2"
scratch_dir="$3"
state_id="$4"

mkdir -p "$scratch_dir"
candidate="$scratch_dir/${state_id}.png"
blessed="$blessed_dir/${state_id}.png"

if [ ! -f "$blessed" ]; then
    echo "run_ui_golden_case: NO BLESSED SCREENSHOT for '$state_id' ($blessed)." >&2
    echo "  Bless the UI goldens first:" >&2
    echo "    BLESS_REASON=\"...\" cmake --build --preset default --target bless_ui_goldens" >&2
    exit 1
fi

# 1) Capture the candidate offscreen (software renderer, deterministic).
"$bin" --capture "$state_id" --out "$candidate"

# 2) Pick the gate by platform — EXACT on the reference platform, TOLERANCE off it.
if "$bin" --is-reference-platform; then
    echo "run_ui_golden_case: reference platform — EXACT gate for '$state_id'"
    "$bin" --compare --candidate "$candidate" --blessed "$blessed" --policy exact
else
    echo "run_ui_golden_case: non-reference platform — TOLERANCE gate for '$state_id'"
    "$bin" --compare --candidate "$candidate" --blessed "$blessed" \
        --policy tolerance --max-delta 8 --pct-threshold 1.0
fi
