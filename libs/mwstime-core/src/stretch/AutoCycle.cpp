// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/stretch/AutoCycle.h"

#include "mws/core/AutoCorr.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mws::stretch {

int AutoCycle::detectCycleLen(core::ConstAudioView zone, double sampleRate)
{
    // Analysis window: the zone, capped to its first 200 ms (PI —
    // dsp-engine.md §7.1). Shorter zones are analyzed whole.
    std::size_t windowFrames = zone.size();
    if (sampleRate > 0.0)
    {
        const auto capFrames = static_cast<std::size_t>(
            std::llround(kAnalysisWindowSeconds * sampleRate));
        windowFrames = std::min(windowFrames, capFrames);
    }

    const core::ConstAudioView window { zone.data(), windowFrames };

    const auto lag =
        core::AutoCorr::bestLag(window, kLagMin, kLagMax, kPeakThreshold);

    return lag.value_or(kFallbackCycleLen);
}

} // namespace mws::stretch
