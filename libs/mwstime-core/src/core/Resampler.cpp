// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/Resampler.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numbers>

namespace mws::core {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;

/// Half the kernel support in input samples at ratio >= 1.
constexpr std::size_t kHalfTaps = SincResampler::kTaps / 2; // 8

/// Kaiser shape parameter (PI; dsp-engine.md §7.2).
constexpr double kBeta = 8.0;

/// Table resolution: entries per unit of kernel argument. Integer kernel
/// arguments land exactly on table entries, so the identity path reads the
/// exact sinc zeros/peak rather than interpolated neighbours.
constexpr std::size_t kPhases = 512;
constexpr std::size_t kTableSize = kHalfTaps * kPhases + 2; // +1 end, +1 guard

/// Zeroth-order modified Bessel function of the first kind (power series;
/// converges fast for |x| <= kBeta — deterministic, no libm extensions).
double besselI0(double x) noexcept
{
    const double halfX = 0.5 * x;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k <= 32; ++k)
    {
        const double factor = halfX / static_cast<double>(k);
        term *= factor * factor;
        sum += term;
        if (term < sum * 1e-18)
            break;
    }
    return sum;
}

/// One-sided table of the kernel prototype h(x) = sinc(x) * kaiser(x / 8),
/// sampled at x = i / kPhases for x in [0, 8]; h is even. Built once,
/// thread-safe (C++11 static init), read-only afterwards.
const std::array<double, kTableSize>& kernelTable()
{
    static const std::array<double, kTableSize> table = [] {
        std::array<double, kTableSize> t {};
        const double i0Beta = besselI0(kBeta);
        for (std::size_t i = 0; i <= kHalfTaps * kPhases; ++i)
        {
            const double x = static_cast<double>(i) / static_cast<double>(kPhases);
            const double sinc = (i == 0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double u = x / static_cast<double>(kHalfTaps); // [0, 1]
            const double kaiser = besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0Beta;
            t[i] = sinc * kaiser;
        }
        t[kTableSize - 1] = 0.0; // guard for interpolation at the table edge
        return t;
    }();
    return table;
}

/// Kernel value at |x| via linear interpolation in the table; 0 outside the
/// [-8, 8] support.
double kernelAt(double x) noexcept
{
    const auto& table = kernelTable();
    const double a = std::abs(x) * static_cast<double>(kPhases);
    const auto i = static_cast<std::size_t>(a);
    if (i >= kHalfTaps * kPhases)
        return 0.0;
    const double frac = a - static_cast<double>(i);
    return table[i] + (table[i + 1] - table[i]) * frac;
}

std::size_t outputLength(std::size_t inputFrames, double ratio) noexcept
{
    return static_cast<std::size_t>(
        std::ceil(static_cast<double>(inputFrames) * ratio));
}

} // namespace

AudioBuffer SincResampler::resample(ConstAudioView input, double ratio)
{
    assert(ratio > 0.0);

    AudioBuffer output(1, outputLength(input.size(), ratio));
    if (input.empty())
        return output;

    AudioView out = output.channel(0);
    const auto inSize = static_cast<long long>(input.size());

    // Downsampling stretches the kernel by 1/ratio so its cutoff lands on the
    // new Nyquist (anti-aliasing); upsampling keeps the base kernel (cutoff at
    // the input Nyquist).
    const double cutoffScale = (ratio < 1.0) ? ratio : 1.0;
    const double halfSupport = static_cast<double>(kHalfTaps) / cutoffScale;

    // Output sample n represents the input signal at input-domain position
    // n/ratio - delayIn. delayIn is an integer count of input samples for
    // ratio >= 1, so the kernel center sits exactly on input samples whenever
    // the phase is integral — identity at ratio 1.0 is then exact.
    const double delayIn = static_cast<double>(kHalfTaps) / cutoffScale;

    for (std::size_t n = 0; n < out.size(); ++n)
    {
        // Double-precision phase: derived from the index, no accumulator drift.
        const double center = static_cast<double>(n) / ratio - delayIn;

        const auto first =
            static_cast<long long>(std::ceil(center - halfSupport));
        const auto last =
            static_cast<long long>(std::floor(center + halfSupport));

        double acc = 0.0;
        double weightSum = 0.0;
        for (long long j = first; j <= last; ++j)
        {
            const double w =
                kernelAt((static_cast<double>(j) - center) * cutoffScale);
            weightSum += w;
            if (j >= 0 && j < inSize)
                acc += w * static_cast<double>(input[static_cast<std::size_t>(j)]);
        }

        // Per-phase normalization pins DC gain to exactly 1 (removes the
        // windowed-sinc passband ripple between polyphase branches). The
        // weight sum is phase-dependent but signal-independent — still a pure
        // precomputable-kernel scheme, just folded in here.
        out[n] = (weightSum > 0.0) ? static_cast<float>(acc / weightSum) : 0.0f;
    }

    return output;
}

double SincResampler::groupDelaySamples(double ratio) noexcept
{
    assert(ratio > 0.0);
    // delayIn input samples (see resample()) converted to output samples:
    //   ratio >= 1: 8 * ratio;  ratio < 1: (8 / ratio) * ratio = 8.
    return static_cast<double>(kHalfTaps) * std::max(ratio, 1.0);
}

AudioBuffer LinearResampler::resample(ConstAudioView input, double ratio)
{
    assert(ratio > 0.0);

    AudioBuffer output(1, outputLength(input.size(), ratio));
    if (input.empty())
        return output;

    AudioView out = output.channel(0);
    const std::size_t lastIndex = input.size() - 1;

    for (std::size_t n = 0; n < out.size(); ++n)
    {
        const double position = static_cast<double>(n) / ratio;
        const auto index = static_cast<std::size_t>(position);
        if (index >= lastIndex)
        {
            // Past the final input sample: interpolate toward silence
            // (everything outside the input is zero).
            if (index == lastIndex)
            {
                const double frac = position - static_cast<double>(index);
                out[n] = static_cast<float>(
                    static_cast<double>(input[lastIndex]) * (1.0 - frac));
            }
            else
            {
                out[n] = 0.0f;
            }
            continue;
        }
        const double frac = position - static_cast<double>(index);
        const double a = static_cast<double>(input[index]);
        const double b = static_cast<double>(input[index + 1]);
        out[n] = static_cast<float>(a + (b - a) * frac);
    }

    return output;
}

double LinearResampler::groupDelaySamples([[maybe_unused]] double ratio) noexcept
{
    assert(ratio > 0.0);
    return 0.0;
}

} // namespace mws::core
