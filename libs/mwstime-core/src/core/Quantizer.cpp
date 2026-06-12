// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/Quantizer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace mws::core {
namespace {

/// splitmix64 — deterministic, allocation-free, in-repo rng for the dither
/// path (no std::rand; testing-strategy.md §3 item 6). Well-defined for any
/// seed including zero. Reference: Steele/Lea/Flood, "Fast splittable
/// pseudorandom number generators" (the JDK SplittableRandom mixer).
[[nodiscard]] std::uint64_t splitmix64Next(std::uint64_t& state) noexcept
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// Uniform float in [0, 1) from the top 24 bits (exact in float32).
[[nodiscard]] float uniform01(std::uint64_t& state) noexcept
{
    return static_cast<float>(splitmix64Next(state) >> 40) * 0x1.0p-24f;
}

} // namespace

Quantizer::Quantizer(int bits) noexcept : bits_(bits)
{
    assert(bits >= 2 && bits <= 24);
    codesPerUnit_ = std::ldexp(1.0f, bits - 1); // 2^(bits-1)
    step_ = 1.0f / codesPerUnit_;               // exact: power of two
    maxCode_ = (std::int32_t{ 1 } << (bits - 1)) - 1;
    minCode_ = -(std::int32_t{ 1 } << (bits - 1));
}

float Quantizer::quantizeSample(float sample) const noexcept
{
    // Mid-tread: nearest integer code, zero a code. lround (half away from
    // zero) keeps the tread symmetric about 0; exact code multiples pass
    // through unchanged (k * step * codesPerUnit == k exactly — power-of-two
    // step), which makes the quantizer idempotent.
    const long code = std::lround(sample * codesPerUnit_);
    const auto clamped =
        std::clamp(static_cast<std::int32_t>(code), minCode_, maxCode_);
    return static_cast<float>(clamped) * step_;
}

void Quantizer::process(AudioView view) const noexcept
{
    for (std::size_t i = 0; i < view.size(); ++i)
        view[i] = quantizeSample(view[i]);
}

void Quantizer::process(AudioView view, std::uint64_t seed) const noexcept
{
    // TPDF dither: difference of two independent uniforms in [0, 1) LSB
    // => triangular pdf over (-1, +1) LSB, zero mean, added before the
    // quantizer (S1100 output stage, dsp-engine.md §8.2).
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < view.size(); ++i)
    {
        // Two calls sequenced explicitly: operand order of '-' is
        // unspecified, and both calls advance the rng state.
        const float u1 = uniform01(state);
        const float u2 = uniform01(state);
        const float dither = (u1 - u2) * step_;
        view[i] = quantizeSample(view[i] + dither);
    }
}

} // namespace mws::core
