// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

#include <optional>

namespace mws::core {

/// Deterministic normalized-autocorrelation period estimator
/// (docs/design/architecture.md §2.1). The primitive behind autC/AUTO-D auto
/// cycle detection (dsp-engine.md §7.1 **(PI)**) and the S950 MON1 per-grain
/// cycle snap (dsp-engine.md §5).
///
/// Analysis-time code: it allocates scratch internally and is NOT
/// real-time-safe. Double accumulation throughout; same input always yields
/// the same result.
struct AutoCorr
{
    /// Normalized autocorrelation r(L) = sum(x[n]·x[n+L]) /
    /// sqrt(sum(x[n]²)·sum(x[n+L]²)) evaluated for every integer lag in
    /// [lagMin, lagMax] (lagMin clamped to >= 1, lagMax to < x.size()).
    ///
    /// Returns the lag of the highest local peak whose value exceeds
    /// `threshold`, or nullopt if no peak does (the caller falls back — 1000
    /// for autC, dsp-engine.md §7.1). Ties are deterministic: an exactly
    /// periodic signal correlates equally at every multiple of its period,
    /// so among peaks within kTieTolerance of the maximum the LOWEST lag
    /// (the fundamental) wins.
    [[nodiscard]] static std::optional<int>
    bestLag(ConstAudioView x, int lagMin, int lagMax, float threshold);

    /// The MON1 "around C" search (dsp-engine.md §5): bestLag over the lag
    /// window [round(center·(1−searchFraction)), round(center·(1+searchFraction))].
    [[nodiscard]] static std::optional<int>
    bestLagNear(ConstAudioView x, int center, double searchFraction, float threshold);

    /// Peaks within this distance of the maximum count as tied (resolved to
    /// the lowest lag). Covers the period-multiple ambiguity: for a
    /// near-periodic signal, r at 2x/3x the period can exceed r at the
    /// fundamental by O(1e-4) purely through fractional-period rounding.
    static constexpr double kTieTolerance = 1.0e-3;
};

} // namespace mws::core
