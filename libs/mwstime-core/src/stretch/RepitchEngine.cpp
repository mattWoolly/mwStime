// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/stretch/RepitchEngine.h"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace mws::stretch {

RepitchResult RepitchEngine::render(core::ConstAudioView src,
                                    double timeFactorPct,
                                    double transposeSemitones)
{
    assert(timeFactorPct > 0.0);

    // rate = 1/T = 100/timeFactorPct; transpose multiplies the same rate
    // (dsp-engine.md §6). This ratio is the virtual DAC clock the §8.1
    // character chain tracks.
    const double rate = 100.0 / timeFactorPct;
    const double clockRatio = rate * std::exp2(transposeSemitones / 12.0);

    RepitchResult result;
    result.clockRatio = clockRatio;

    const std::size_t numIn = src.size();
    if (numIn == 0)
        return result;

    // Output length derived from the read schedule pos(k) = k * clockRatio:
    // round(N / clockRatio) outputs; the last read position
    // (outLen - 1) * clockRatio is then always < N, so no clamp is needed
    // (kept as an assert).
    const auto outLen = static_cast<std::size_t>(
        std::llround(static_cast<double>(numIn) / clockRatio));

    result.out.resize(1, outLen);
    core::AudioView out = result.out.channel(0);

    for (std::size_t k = 0; k < outLen; ++k)
    {
        // Zero-order hold at the virtual clock: output sample is the source
        // sample under the read head, verbatim. NO interpolation [DRR F4].
        const double pos = static_cast<double>(k) * clockRatio;
        const auto idx = static_cast<std::size_t>(pos); // floor (pos >= 0)
        assert(idx < numIn);
        out[k] = src[idx];
    }

    return result;
}

double RepitchEngine::semitoneOffset(double timeFactorPct) noexcept
{
    assert(timeFactorPct > 0.0);
    // LCD formula (dsp-engine.md §6): -12 * log2(T), T = timeFactorPct / 100.
    return -12.0 * std::log2(timeFactorPct / 100.0);
}

} // namespace mws::stretch
