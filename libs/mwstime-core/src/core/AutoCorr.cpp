// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/AutoCorr.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mws::core {

std::optional<int> AutoCorr::bestLag(ConstAudioView x,
                                     int lagMin,
                                     int lagMax,
                                     float threshold)
{
    const auto numFrames = static_cast<int>(x.size());

    lagMin = std::max(lagMin, 1);
    lagMax = std::min(lagMax, numFrames - 1);
    if (lagMax < lagMin)
        return std::nullopt;

    // Prefix sums of x[n]^2 (double) so each lag's two energy terms are O(1):
    // energy0(L) = sum_{n<N-L} x[n]^2, energyL(L) = sum_{n>=L} x[n]^2.
    std::vector<double> energyPrefix(static_cast<std::size_t>(numFrames) + 1, 0.0);
    for (int n = 0; n < numFrames; ++n)
    {
        const double sample = x[static_cast<std::size_t>(n)];
        energyPrefix[static_cast<std::size_t>(n) + 1] =
            energyPrefix[static_cast<std::size_t>(n)] + sample * sample;
    }

    const int numLags = lagMax - lagMin + 1;
    std::vector<double> corr(static_cast<std::size_t>(numLags), 0.0);

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const int overlap = numFrames - lag;

        double numerator = 0.0;
        for (int n = 0; n < overlap; ++n)
            numerator += static_cast<double>(x[static_cast<std::size_t>(n)])
                         * static_cast<double>(x[static_cast<std::size_t>(n + lag)]);

        const double energy0 = energyPrefix[static_cast<std::size_t>(overlap)];
        const double energyL = energyPrefix[static_cast<std::size_t>(numFrames)]
                               - energyPrefix[static_cast<std::size_t>(lag)];
        const double denominator = std::sqrt(energy0 * energyL);

        corr[static_cast<std::size_t>(lag - lagMin)] =
            denominator > 0.0 ? numerator / denominator : 0.0;
    }

    // Candidate peaks: local maxima (>= both neighbours; range edges compare
    // against their single in-range neighbour, so the global max always
    // qualifies).
    const auto isPeak = [&](int i)
    {
        const double value = corr[static_cast<std::size_t>(i)];
        const bool leftOk = i == 0 || value >= corr[static_cast<std::size_t>(i - 1)];
        const bool rightOk =
            i == numLags - 1 || value >= corr[static_cast<std::size_t>(i + 1)];
        return leftOk && rightOk;
    };

    double maxPeak = -1.0;
    for (int i = 0; i < numLags; ++i)
        if (isPeak(i))
            maxPeak = std::max(maxPeak, corr[static_cast<std::size_t>(i)]);

    // "Highest peak above threshold" (dsp-engine.md §7.1): no peak exceeding
    // the threshold means no detection.
    if (!(maxPeak > static_cast<double>(threshold)))
        return std::nullopt;

    // Deterministic tie-break: lowest lag among peaks tied with the maximum
    // (period-multiple ambiguity — see header).
    for (int i = 0; i < numLags; ++i)
        if (isPeak(i) && corr[static_cast<std::size_t>(i)] >= maxPeak - kTieTolerance)
            return lagMin + i;

    return std::nullopt; // unreachable: maxPeak itself satisfies the scan
}

std::optional<int> AutoCorr::bestLagNear(ConstAudioView x,
                                         int center,
                                         double searchFraction,
                                         float threshold)
{
    const auto lo = static_cast<int>(
        std::llround(static_cast<double>(center) * (1.0 - searchFraction)));
    const auto hi = static_cast<int>(
        std::llround(static_cast<double>(center) * (1.0 + searchFraction)));
    return bestLag(x, lo, hi, threshold);
}

} // namespace mws::core
