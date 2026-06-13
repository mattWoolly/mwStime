// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// The SpliceCal / D-TIME calibration fitter (task plan/backlog/026b;
// docs/qa/hardware-capture-plan.md §4). JUCE-free; links only mwstime_core
// (the engines + WavIo) and the shared mwstime_render_lib case schema.
//
// The fitter is the analysis half of the hardware-capture calibration loop
// (docs/design/testing-strategy.md §7 Wave 2; architecture.md §10 risk 1). It
// never reads the hardware analytically — the splice fine structure is not
// recoverable that way (akaizer-analysis.md §2.4) — so it FITS by re-rendering
// the engine across the SpliceCal grid and minimizing a normalization-robust
// distance to a capture. For a synthetic capture made at a known SpliceCal the
// search recovers it exactly (distance 0, the ctest `calibrate` self-test); for
// a real capture it finds the closest overlapF and reports the residual.
//
// Captures are arbitrary local WAVs (NEVER committed — no third-party audio in
// the repo). The tool reads a capture directory by path plus a calibration
// manifest (the tests/golden/cases.json schema extended with a per-case `set`
// tag: "calibration" | "validation"); the inputs are the committed original
// synthetic corpus.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mws/engine/Params.h"
#include "mws/stretch/CyclicEngine.h"

namespace mwscal {

/// Which disjoint set a case belongs to (docs/qa/hardware-capture-plan.md §3):
/// the fitter searches over `Calibration` cases and only PREDICTS (no search)
/// `Validation` cases — the no-overfitting rule. Default Calibration when a
/// manifest omits the tag.
enum class CaseSet : std::uint8_t { Calibration, Validation };

/// One case to fit/predict: the render parameters (already overlaid on the §2
/// defaults by the shared loader) plus the input filename and the disjoint-set
/// tag. The S950 D-TIME value is the case's `cycleLen` field (D-TIME ≡ cycle
/// length in samples is the (PI) mapping under test — dsp-engine.md §5).
struct CalCase {
    std::string id;
    std::string inputFile;                 ///< file inside the inputs dir
    mws::engine::ParamSnapshot params{};    ///< §2 defaults overlaid with the case
    CaseSet set = CaseSet::Calibration;
};

/// The geometry the fitter measures for one case (docs/qa/...-plan.md §4). All
/// derived from the SCHEDULE at the fitted/frozen overlapF, then cross-checked
/// against the capture. These are the human-readable observables the plan doc
/// names: flutter rate, stutter schedule, schedule-derived output length.
struct Observables {
    int cycleLen = 0;             ///< C after ModelSpec clamp + D-TIME mapping
    double modelRateHz = 0.0;     ///< the model's processing rate (dsp-engine §3.5)
    std::int64_t hopOut = 0;      ///< output grain spacing = round(C·(1−F))
    std::int64_t hopIn = 0;       ///< input advance = round(hop_out/T) (CLASSIC)
    double flutterRateHz = 0.0;   ///< splice-comb spacing = modelRate / hop_out
    std::int64_t scheduleLen = 0; ///< schedule-derived output length (model rate)
    std::int64_t captureLen = 0;  ///< the capture's actual length (frames)
};

/// The result of fitting (or predicting) one case against its capture.
struct CaseFit {
    std::string id;
    CaseSet set = CaseSet::Calibration;
    bool ok = false;             ///< false ⇒ `error` explains (missing capture, etc.)
    std::string error;

    float overlapF = 0.0f;       ///< the fitted (Calibration) or frozen (Validation) F
    double residual = 0.0;       ///< distance metric at overlapF (0 = perfect match)
    Observables obs{};

    /// D-TIME mapping check (S950 only; dsp-engine.md §5). `dTimeChecked` is
    /// true for S950 cases; `dTimeConsistent` is true when interpreting D-TIME
    /// as cycle length in samples predicts the capture within tolerance.
    bool dTimeChecked = false;
    bool dTimeConsistent = false;
};

/// Configuration for one fitter run.
struct FitConfig {
    std::string inputsDir;      ///< committed original synthetic corpus
    std::string capturesDir;    ///< arbitrary local capture dir (NOT committed)

    /// overlapF grid (inclusive) the search sweeps for Calibration cases. The
    /// authentic engines span 0.05..0.95 (dsp-engine.md §3.1); the default grid
    /// is dense enough to recover a planted value to gridStep.
    float gridLo = 0.05f;
    float gridHi = 0.60f;
    float gridStep = 0.01f;

    /// Frozen overlapF used for Validation cases (no search): the value fitted
    /// on the calibration set, passed back in to PREDICT the held-out captures.
    /// Negative ⇒ "use the same grid as calibration" (used by the self-test on
    /// a single combined run; production splits the two runs).
    float frozenOverlapF = -1.0f;

    /// Per-sample tolerance for treating the D-TIME mapping / validation
    /// prediction as consistent (RMS residual after normalization). (PI)
    /// 1e-3: well above float-render noise, below an audible splice mismatch.
    double consistencyTol = 1e-3;
};

/// Fits (Calibration) or predicts (Validation) a single case against its
/// capture WAV in `cfg.capturesDir`. Re-renders the engine across the grid (or
/// at the frozen F) and returns the best overlapF, the residual, the measured
/// observables, and the S950 D-TIME consistency. `error` is set (ok=false) when
/// the input/capture cannot be read.
[[nodiscard]] CaseFit fitCase(const CalCase& c, const FitConfig& cfg);

/// The splice-comb characteristic (flutter) frequency for a schedule:
/// modelRate / hop_out (testing-strategy.md §3 property 8). Pure helper.
[[nodiscard]] double flutterRateHz(double modelRateHz, std::int64_t hopOut) noexcept;

/// hop_out = round(C·(1−F)) clamped to [1, C] — the schedule's output grain
/// spacing (dsp-engine.md §3.1, GrainGeometry::fromCycle). Pure helper so the
/// observables and the proposed-F inverse share one definition.
[[nodiscard]] std::int64_t hopOutForCycle(int cycleLen, float overlapF) noexcept;

/// The overlapF that a measured hop_out implies for a given cycle length:
/// F = 1 − hop_out / C, clamped to (0, 1). The analytic inverse of
/// hopOutForCycle, used to report a proposed F directly from a comb measurement
/// (cross-check on the grid search). Pure helper.
[[nodiscard]] float overlapFForHopOut(int cycleLen, std::int64_t hopOut) noexcept;

} // namespace mwscal
