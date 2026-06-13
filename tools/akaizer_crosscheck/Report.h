// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Report — the per-case deviation-report table the QA fleet attaches to issues
// (task plan/backlog/026c-akaizer-crosscheck.md; docs/qa/akaizer-crosscheck.md).
//
// The report is a plain Markdown table so it pastes directly into a GitHub
// issue. It records, per case: whether the case was inside the certified
// Akaizer window (skipped cases show the explicit "no Akaizer oracle" reason),
// the analytic CLASSIC schedule predictions, and — when both normalized renders
// are supplied — the measured flutter / length deviations.
//
// Deviations are DESCRIPTIVE: this is a secondary cross-check, never a gate.

#pragma once

#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

#include "CrossCheck.h"

namespace mws::akz {

/// Writes the deviation report (Markdown) for `reports` to `os`. `cornerNote`
/// is a one-line provenance string (e.g. the Akaizer dir + version) recorded in
/// the header so a pasted report is self-describing.
inline void writeMarkdownReport(std::ostream& os,
                                const std::vector<CaseReport>& reports,
                                const std::string& cornerNote = {})
{
    os << "# Akaizer secondary cross-check — deviation report\n\n";
    os << "**SECONDARY, LOCAL-ONLY cross-check** (Akaizer fidelity refuted 0-3; the\n"
          "primary oracle is hardware captures — docs/design/testing-strategy.md §7).\n"
          "Both sides are peak-normalized; only T 120-2000% cases have an Akaizer\n"
          "oracle. Deviations are documented, NOT gated.\n\n";
    if (!cornerNote.empty())
        os << "Provenance: " << cornerNote << "\n\n";

    os << "| case | T% | C | inWindow | hop_out | hop_in | grains | schedLen | "
          "predFlutterHz | mwsFlutterHz | akzFlutterHz | flutterΔcents | "
          "mwsLen | akzLen | lenΔframes | note |\n";
    os << "|---|---:|---:|:--:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n";

    long compared = 0;
    long skipped = 0;
    for (const auto& r : reports)
    {
        os << "| " << r.id
           << " | " << std::fixed << std::setprecision(0) << r.timeFactorPct
           << " | " << r.cycleLen;
        if (r.skipped)
        {
            ++skipped;
            os << " | no | — | — | — | — | — | — | — | — | — | — | — | "
               << r.skipReason << " |\n";
            continue;
        }
        ++compared;
        os << " | yes"
           << " | " << std::fixed << std::setprecision(1) << r.schedule.hopOut
           << " | " << r.schedule.hopIn
           << " | " << r.schedule.grains
           << " | " << r.schedule.outputLen
           << " | " << std::setprecision(2) << r.predictedFlutterHz;
        if (r.haveMeasurements)
        {
            os << " | " << r.mwsFlutterHz
               << " | " << r.akaizerFlutterHz
               << " | " << std::showpos << r.flutterDeviationCents() << std::noshowpos
               << " | " << r.mwsOutputLen
               << " | " << r.akaizerOutputLen
               << " | " << std::showpos << r.outputLenDeviationFrames() << std::noshowpos
               << " | measured |\n";
        }
        else
        {
            os << " | — | — | — | — | — | — | analytic only (no renders) |\n";
        }
    }

    os << "\nCases compared: " << compared << "; skipped (no oracle): " << skipped
       << "; total: " << reports.size() << "\n";
}

} // namespace mws::akz
