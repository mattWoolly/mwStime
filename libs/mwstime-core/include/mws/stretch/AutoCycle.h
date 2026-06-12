// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// AutoCycle — the autC / AUTO-D helper (dsp-engine.md §7.1 **(PI)**). The
// hardware documents a software-logic cycle proposer "like autolooping",
// "not always infallible" [akai-manuals-specs.md §3 p.46 / §2]; the manual
// never says "autocorrelation" — that is our ADR-001-flagged inference.
// Ours: normalized autocorrelation (mws::core::AutoCorr) over the stretch
// zone, capped to its first 200 ms, lag range 20–2000 samples; the best
// peak above 0.3 wins, otherwise fall back to 1000. Deterministic.
//
// Analysis-time code (AutoCorr allocates scratch): NOT real-time-safe.

#pragma once

#include "mws/core/Buffer.h"

namespace mws::stretch {

/// Deterministic cycle-length proposer behind the autC (S1000/S1100) and
/// AUTO-D (S950) soft keys. Pure function of (zone, sampleRate).
struct AutoCycle
{
    // ----------------------------------------------------------------------
    // Named-constants block — every (PI) value the QA fleet may retune lives
    // here and nowhere else (testing-strategy.md §7: each (PI) constant
    // carries a tuning note; test_autocycle.cpp pins these values).
    // ----------------------------------------------------------------------

    /// Analysis-window cap in seconds: zones longer than this analyze only
    /// their first 200 ms. **(PI)** Tuning note: pure inference — no manual
    /// documents an analysis span. 200 ms at 44.1 kHz (8820 samples) gives
    /// >4 full periods of the longest detectable cycle (lag 2000) while
    /// keeping the helper fast on long zones. Retune against hardware
    /// captures of autC on long material (testing-strategy.md §7 wave 2).
    static constexpr double kAnalysisWindowSeconds = 0.2;

    /// Minimum acceptable normalized-autocorrelation peak. **(PI)** Tuning
    /// note: inferred confidence floor, not a hardware number. 0.3 rejects
    /// white noise (peaks ~0.05 at these window sizes) while accepting
    /// clearly pitched material (peaks >0.9). If hardware autC proposes
    /// content-derived cycles on noisier material than we do, lower it; if
    /// it falls back more readily, raise it.
    static constexpr float kPeakThreshold = 0.3f;

    /// Proposed cycle length when no confident peak exists. Matches the
    /// hardware's documented default CYCLE value of 1000 (dsp-engine.md
    /// §7.1); the *use* of it as the no-detection fallback is **(PI)**.
    /// Tuning note: verify against hardware autC behavior on noise — the
    /// manual only warns the feature is "not always infallible".
    static constexpr int kFallbackCycleLen = 1000;

    /// Lag search range in samples, 20–2000 — the hardware CYCLE parameter's
    /// own range (dsp-engine.md §2/§7.1), so every proposal is a value the
    /// CYCLE field can actually hold. Not retunable independently of that
    /// parameter range.
    static constexpr int kLagMin = 20;
    static constexpr int kLagMax = 2000;

    /// Proposes a CYCLE length (in samples) from audio content. Analysis
    /// window = `zone`, or its first `kAnalysisWindowSeconds` if longer;
    /// returns the best AutoCorr lag in [kLagMin, kLagMax] whose normalized
    /// peak exceeds kPeakThreshold, else kFallbackCycleLen. Same input
    /// always yields the same result.
    [[nodiscard]] static int detectCycleLen(core::ConstAudioView zone,
                                            double sampleRate);
};

} // namespace mws::stretch
