// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

#include <array>
#include <cstddef>

namespace mws::core {

/// 6th-order Butterworth low-pass: three cascaded biquads with the standard
/// Butterworth factorization Q = 0.5176 / 0.7071 / 1.9319
/// (docs/design/architecture.md §2.1, docs/design/dsp-engine.md §8.1).
///
/// This is the S900/S950 reconstruction filter model: the hardware's
/// switched-capacitor filter tracks the variable voice clock
/// (cutoff = clock / 2.5 — deep-research-report.md Finding 4), so the cutoff
/// must be retunable per note-on **on the audio thread**: `setCutoff` is
/// closed-form (bilinear transform), noexcept and allocation-free
/// (architecture.md §4.2). Retuning preserves filter state — only the
/// coefficients change, so audio keeps flowing without a reset click.
///
/// Each section runs in transposed direct form II with double-precision
/// state, which stays well-behaved at the low normalized cutoffs the
/// oversampled variable-clock chain produces (dsp-engine.md §8.1).
class Butterworth6LP
{
public:
    Butterworth6LP() noexcept = default;

    /// Recomputes the three biquads for the given cutoff via the bilinear
    /// transform (RBJ low-pass form, which maps the analog -3.01 dB point
    /// exactly onto `cutoffHz`). Closed-form, allocation-free, callable per
    /// note-on on the audio thread. Filter state is preserved.
    ///
    /// `cutoffHz` is clamped into (0, 0.49 * sampleRate] so the transform
    /// stays defined for any parameter combination; `sampleRate` must be > 0.
    void setCutoff(double cutoffHz, double sampleRate) noexcept;

    /// Clears all section state (the coefficients are kept).
    void reset() noexcept
    {
        for (Biquad& section : sections_)
            section.reset();
    }

    /// Per-sample path for the variable-clock chain (dsp-engine.md §8.1).
    [[nodiscard]] float processSample(float input) noexcept
    {
        double value = static_cast<double>(input);
        for (Biquad& section : sections_)
            value = section.process(value);
        return static_cast<float>(value);
    }

    /// In-place block processing over one channel. No allocation.
    void process(AudioView view) noexcept
    {
        for (float& sample : view)
            sample = processSample(sample);
    }

private:
    /// One second-order section, transposed direct form II.
    struct Biquad
    {
        // Identity (pass-through) until setCutoff supplies real coefficients.
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        double process(double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void reset() noexcept { z1 = z2 = 0.0; }
    };

    std::array<Biquad, 3> sections_{};
};

} // namespace mws::core
