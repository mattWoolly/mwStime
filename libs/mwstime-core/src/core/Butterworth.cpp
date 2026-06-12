// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/Butterworth.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace mws::core {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Standard 6th-order Butterworth factorization: pole pairs at 75°/45°/15°
// from the imaginary axis, Q_k = 1 / (2 cos θ_k)
// (dsp-engine.md §8.1: Q = 0.5176 / 0.7071 / 1.9319).
constexpr std::array<double, 3> kSectionQ = {
    0.51763809020504153, // 1 / (2 cos 15°)
    0.70710678118654752, // 1 / (2 cos 45°)
    1.93185165257813657, // 1 / (2 cos 75°)
};
} // namespace

void Butterworth6LP::setCutoff(double cutoffHz, double sampleRate) noexcept
{
    assert(sampleRate > 0.0);

    // Keep the transform defined for any input: cutoff strictly inside
    // (0, Nyquist).
    const double fc =
        std::clamp(cutoffHz, sampleRate * 1.0e-5, sampleRate * 0.49);

    // RBJ cookbook low-pass per section (bilinear transform with the analog
    // prototype prewarped so the cascade's -3.01 dB point lands exactly on fc).
    const double w0 = 2.0 * kPi * fc / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);

    for (std::size_t i = 0; i < sections_.size(); ++i)
    {
        const double alpha = sinw0 / (2.0 * kSectionQ[i]);
        const double a0 = 1.0 + alpha;

        Biquad& s = sections_[i];
        s.b1 = (1.0 - cosw0) / a0;
        s.b0 = 0.5 * s.b1;
        s.b2 = s.b0;
        s.a1 = (-2.0 * cosw0) / a0;
        s.a2 = (1.0 - alpha) / a0;
    }
}

} // namespace mws::core
