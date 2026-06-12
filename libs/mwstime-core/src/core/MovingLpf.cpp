// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/MovingLpf.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mws::core {

void MovingLpf3::setCutoff(double cutoffHz, double sampleRate) noexcept
{
    const double clamped = std::clamp(cutoffHz, 1.0, 0.49 * sampleRate);

    // Bilinear transform with cutoff prewarp: K = tan(pi * fc / fs). The
    // analog 3rd-order Butterworth prototype factors as
    //   H(s) = 1 / ((s + 1) (s^2 + s + 1))
    // i.e. one real pole plus a complex pair with Q = 1.0
    // (docs/design/dsp-engine.md §8.2).
    const double k = std::tan(std::numbers::pi * clamped / sampleRate);

    // First-order section (real pole).
    {
        const double norm = 1.0 / (k + 1.0);
        foB0_ = k * norm;
        foB1_ = k * norm;
        foA1_ = (k - 1.0) * norm;
    }

    // Biquad section (complex pair, Q = 1.0).
    {
        constexpr double q = 1.0;
        const double norm = 1.0 / (1.0 + k / q + k * k);
        bqB0_ = k * k * norm;
        bqB1_ = 2.0 * bqB0_;
        bqB2_ = bqB0_;
        bqA1_ = 2.0 * (k * k - 1.0) * norm;
        bqA2_ = (1.0 - k / q + k * k) * norm;
    }

    fullyOpen_ = false;
}

void MovingLpf3::setFullyOpen() noexcept
{
    fullyOpen_ = true;
}

void MovingLpf3::reset() noexcept
{
    foX1_ = 0.0;
    foY1_ = 0.0;
    bqS1_ = 0.0;
    bqS2_ = 0.0;
}

void MovingLpf3::process(AudioView view) noexcept
{
    if (fullyOpen_)
        return; // transparent passthrough — the hardware default state

    for (float& sample : view)
    {
        const auto x = static_cast<double>(sample);

        // First-order section.
        const double v = foB0_ * x + foB1_ * foX1_ - foA1_ * foY1_;
        foX1_ = x;
        foY1_ = v;

        // Biquad section, transposed direct form II.
        const double y = bqB0_ * v + bqS1_;
        bqS1_ = bqB1_ * v - bqA1_ * y + bqS2_;
        bqS2_ = bqB2_ * v - bqA2_ * y;

        sample = static_cast<float>(y);
    }
}

} // namespace mws::core
