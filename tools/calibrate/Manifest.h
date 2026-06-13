// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// The calibration-manifest loader (task 026b; docs/qa/hardware-capture-plan.md
// §3–§4). The manifest is the tests/golden/cases.json schema (loaded by the
// shared mwstime_render_lib `loadCase`, so the render parameters parse
// identically to the golden harness) extended with ONE harness-only key per
// case: `set` = "calibration" | "validation" (the disjoint-set tag). The
// manifest lists which captures to fit/predict; the captures themselves live
// in an arbitrary local directory and are NEVER committed.

#pragma once

#include <string>
#include <vector>

#include "Calibrate.h"

namespace mwscal {

/// Result of loading a manifest: `error` empty on success; `cases` holds every
/// case in file order (calibration + validation interleaved as written).
struct ManifestLoadResult {
    std::vector<CalCase> cases;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Parses a calibration manifest from JSON text. Each case's render parameters
/// are loaded via the shared mwstime_render_lib loader (same validation as the
/// golden harness — unknown `params` keys are rejected), and the optional
/// per-case `set` string maps to CaseSet (default Calibration, unknown value =
/// error). A missing/empty "cases" array is an error.
[[nodiscard]] ManifestLoadResult loadManifest(const std::string& manifestJsonText);

} // namespace mwscal
