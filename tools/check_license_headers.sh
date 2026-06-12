#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# Fails (exit 1) when any committed C/C++ source under libs/, plugin/, tools/,
# or tests/ lacks the AGPLv3 SPDX header (docs/CONTRIBUTING.md). Registered as
# the `license_headers` ctest.
#
# Usage: check_license_headers.sh [repo-root]

set -euo pipefail

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$repo_root"

spdx_line="// SPDX-License-Identifier: AGPL-3.0-or-later"

missing=0
while IFS= read -r file; do
    # Header must appear within the first 3 lines of the file.
    if ! head -n 3 "$file" | grep -qF "$spdx_line"; then
        echo "MISSING LICENSE HEADER: $file" >&2
        missing=1
    fi
done < <(git ls-files -- \
            'libs/**/*.h' 'libs/**/*.hpp' 'libs/**/*.cpp' 'libs/**/*.cc' 'libs/**/*.mm' \
            'plugin/**/*.h' 'plugin/**/*.hpp' 'plugin/**/*.cpp' 'plugin/**/*.cc' 'plugin/**/*.mm' \
            'tools/**/*.h' 'tools/**/*.hpp' 'tools/**/*.cpp' 'tools/**/*.cc' 'tools/**/*.mm' \
            'tests/**/*.h' 'tests/**/*.hpp' 'tests/**/*.cpp' 'tests/**/*.cc' 'tests/**/*.mm')

if [ "$missing" -ne 0 ]; then
    echo "License-header check FAILED. Required first-lines header:" >&2
    echo "  $spdx_line" >&2
    echo "  // mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly." >&2
    exit 1
fi

echo "License-header check passed."
