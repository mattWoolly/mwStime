// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// RepitchEngine — the honest S900 mode (ADR-003, dsp-engine.md §6): the S900
// has NO timestretch, so the engine is varispeed repitch. `rate = 1/T`, the
// transpose parameter multiplies the same rate, and the read is zero-order
// hold with NO interpolation [deep-research-report.md Finding 4,
// service-manual grade]. The variable-rate read IS the virtual DAC clock the
// §8.1 character chain (task 016) consumes — this engine only reports the
// clock ratio; the 12-bit quantize / tracking Butterworth live in the chain.

#pragma once

#include "mws/core/Buffer.h"

namespace mws::stretch {

/// Output of an offline S900 repitch render.
struct RepitchResult {
    /// Mono rendered audio, length = round(N / clockRatio).
    core::AudioBuffer out;

    /// Virtual DAC clock ratio relative to the source rate:
    /// `clockRatio = rate * 2^(transpose/12)` with `rate = 100/timeFactorPct`
    /// (dsp-engine.md §6: clock = f_s * rate * 2^(transpose/12)). Consumed by
    /// the §8.1 character chain (ZOH playback + clock-tracking Butterworth).
    double clockRatio = 1.0;
};

/// S900 varispeed repitch engine (offline, mono). Stateless and deterministic:
/// the read schedule is `pos(k) = k * clockRatio`, output sample
/// `src[floor(pos)]` — zero-order hold, no interpolation anywhere [DRR F4].
class RepitchEngine
{
public:
    /// Renders `src` at the varispeed rate implied by TIME FACTOR (percent of
    /// original length, > 0) and TRANSPOSE (semitones). Time and pitch are
    /// coupled (that is the point — ADR-003).
    [[nodiscard]] static RepitchResult render(core::ConstAudioView src,
                                              double timeFactorPct,
                                              double transposeSemitones);

    /// LCD display helper: the semitone offset implied by the time factor,
    /// `-12 * log2(T)` with `T = timeFactorPct / 100` (dsp-engine.md §6).
    /// T=200% -> -12.0, T=100% -> 0.0.
    [[nodiscard]] static double semitoneOffset(double timeFactorPct) noexcept;
};

} // namespace mws::stretch
