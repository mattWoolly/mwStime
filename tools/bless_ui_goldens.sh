#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# bless_ui_goldens.sh — the GATED UI golden-screenshot blessing procedure (task
# 047b; mirrors the task-026 tools/bless_goldens.sh pattern; docs/design/
# testing-strategy.md §4 "Blessing procedure" / §7 Wave 3, ADR-005). Wrapped by
# the `bless_ui_goldens` CMake target. Re-captures every defined state into
# tests/ui/blessed/ and writes tests/ui/blessed/MANIFEST.json (date, blesser,
# reason, platform, state count).
#
# HARD GATE: refuses to run unless BLESS_REASON is set to a non-empty string —
# exactly like the golden-render bless target (testing-strategy.md §4: golden
# churn without an audible/intentional change is a review-rejection criterion).
# The blessed PNGs are byte-stable for a given build, so a same-commit re-bless
# changes only the MANIFEST (its date/blesser/reason fields), never the PNGs —
# the task verification command checks exactly this.
#
# The state list comes from `ui_screenshot --list` (the driver is the single
# source of truth for the state set), so this script needs no state table.
#
# Usage (normally via the CMake target, which fills in the paths):
#   BLESS_REASON="why" bless_ui_goldens.sh <ui-screenshot-bin> <blessed-dir>

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: bless_ui_goldens.sh <ui-screenshot-bin> <blessed-dir>" >&2
    exit 2
fi

bin="$1"
blessed_dir="$2"

# --- the gate ---------------------------------------------------------------
if [ -z "${BLESS_REASON:-}" ]; then
    echo "bless_ui_goldens: REFUSED — BLESS_REASON is not set." >&2
    echo "  Blessing rewrites the committed golden screenshots. Set a justification:" >&2
    echo "    BLESS_REASON=\"<why; link an ADR/bug>\" cmake --build --preset default --target bless_ui_goldens" >&2
    echo "  Policy: testing-strategy.md §4/§7 — UI golden churn without an" >&2
    echo "  intentional visual change is a review-rejection criterion." >&2
    exit 1
fi

mkdir -p "$blessed_dir"

echo "bless_ui_goldens: re-capturing the UI golden set into $blessed_dir"
echo "  reason : $BLESS_REASON"

count=0
while read -r state_id; do
    [ -z "$state_id" ] && continue
    case "$state_id" in \#*) continue ;; esac
    "$bin" --capture "$state_id" --out "$blessed_dir/${state_id}.png" > /dev/null
    count=$((count + 1))
done < <("$bin" --list)

echo "bless_ui_goldens: re-captured $count states"

# --- MANIFEST.json ----------------------------------------------------------
# Date is UTC ISO-8601; blesser comes from the environment so a CI/agent bless is
# attributable; platform records where the reference set was produced (the gate
# is reference-platform-only — ADR-005).
date_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
blesser="${BLESS_BLESSER:-${USER:-unknown}}"
platform="$(uname -sm)"

json_escape() {
    printf '%s' "$1" | tr '\n' ' ' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}
reason_escaped="$(json_escape "$BLESS_REASON")"
blesser_escaped="$(json_escape "$blesser")"
platform_escaped="$(json_escape "$platform")"

manifest="$blessed_dir/MANIFEST.json"
cat > "$manifest" <<EOF
{
  "kind": "ui-golden-screenshots",
  "referencePlatform": "macOS arm64",
  "date": "$date_utc",
  "blesser": "$blesser_escaped",
  "platform": "$platform_escaped",
  "reason": "$reason_escaped",
  "stateCount": $count
}
EOF

echo "bless_ui_goldens: wrote $manifest"
echo "bless_ui_goldens: done"
