// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Tests for mws::core::Quantizer (plan/backlog/007-core-quantizer.md, written
// first per docs/design/testing-strategy.md §2 Quantizer bullet and §3 item 6).

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/Quantizer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

// Test-case names begin with the tag word so `ctest -R quantizer` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::core::Quantizer;

/// Deterministic arbitrary test signal in roughly [-amplitude, amplitude]:
/// a sum of incommensurate sines so values do not land on the quantizer grid.
void fillArbitrary(AudioView view, float amplitude)
{
    for (std::size_t i = 0; i < view.size(); ++i)
    {
        const auto n = static_cast<float>(i);
        view[i] = amplitude
                  * (0.61f * std::sin(0.0731f * n) + 0.39f * std::sin(0.4173f * n + 1.3f));
    }
}
} // namespace

TEST_CASE("quantizer: 12-bit step size is exactly 1/2048 of full scale", "[quantizer]")
{
    const Quantizer q(12);

    // Full scale ±1.0, 4096 mid-tread levels => step = 2/4096 = 1/2048, exact
    // (a power of two, hence exactly representable in float32).
    REQUIRE(q.step() == 1.0f / 2048.0f);

    // Mid-tread: zero is a code, and inputs snap to the nearest code.
    REQUIRE(q.quantizeSample(0.0f) == 0.0f);
    REQUIRE(q.quantizeSample(0.2f * q.step()) == 0.0f);
    REQUIRE(q.quantizeSample(0.8f * q.step()) == q.step());
    REQUIRE(q.quantizeSample(-0.8f * q.step()) == -q.step());

    // Adjacent codes around an arbitrary level differ by exactly one step.
    const float low = q.quantizeSample(100.2f * q.step());
    const float high = q.quantizeSample(101.2f * q.step());
    REQUIRE(low == 100.0f * q.step());
    REQUIRE(high - low == q.step());
}

TEST_CASE("quantizer: 16-bit step size is exactly 1/32768 of full scale", "[quantizer]")
{
    const Quantizer q(16);
    REQUIRE(q.step() == 1.0f / 32768.0f);
    REQUIRE(q.quantizeSample(0.7f * q.step()) == q.step());
}

TEST_CASE("quantizer: every output sample is an integer multiple of the step",
          "[quantizer]")
{
    const Quantizer q(12);
    AudioBuffer buffer(1, 4096);
    fillArbitrary(buffer.channel(0), 0.95f);
    q.process(buffer.channel(0));

    for (std::size_t i = 0; i < buffer.numFrames(); ++i)
    {
        const float code = buffer.channel(0)[i] / q.step();
        REQUIRE(code == std::floor(code)); // exact integer code
    }
}

TEST_CASE("quantizer: idempotent on already-quantized input", "[quantizer]")
{
    const Quantizer q(12);
    AudioBuffer buffer(1, 8192);
    fillArbitrary(buffer.channel(0), 1.0f);

    q.process(buffer.channel(0));
    std::vector<float> once(buffer.channel(0).begin(), buffer.channel(0).end());

    q.process(buffer.channel(0));
    for (std::size_t i = 0; i < once.size(); ++i)
        REQUIRE(buffer.channel(0)[i] == once[i]); // bitwise equal
}

TEST_CASE("quantizer: distinct-value count <= 4096 on arbitrary input (12-bit)",
          "[quantizer]")
{
    const Quantizer q(12);
    AudioBuffer buffer(1, 65536);
    // Deliberately overdriven beyond ±1.0: out-of-range input must clamp to
    // the code range, never mint extra levels.
    fillArbitrary(buffer.channel(0), 1.4f);
    q.process(buffer.channel(0));

    std::set<float> distinct(buffer.channel(0).begin(), buffer.channel(0).end());
    REQUIRE(distinct.size() <= 4096);
    REQUIRE(distinct.size() > 2048); // signal actually exercises the range

    // Clamped extremes stay inside full scale.
    for (const float v : distinct)
    {
        REQUIRE(v >= -1.0f);
        REQUIRE(v <= 1.0f);
    }
}

TEST_CASE("quantizer: dithered path is bit-identical across runs with the same seed",
          "[quantizer]")
{
    const Quantizer q(16);
    constexpr std::uint64_t seed = 0xA5A5'1234'DEAD'BEEFULL;

    AudioBuffer a(1, 16384);
    AudioBuffer b(1, 16384);
    fillArbitrary(a.channel(0), 0.9f);
    fillArbitrary(b.channel(0), 0.9f);

    q.process(a.channel(0), seed);
    q.process(b.channel(0), seed);

    for (std::size_t i = 0; i < a.numFrames(); ++i)
        REQUIRE(a.channel(0)[i] == b.channel(0)[i]); // bitwise equal
}

TEST_CASE("quantizer: dithered output still lies on the quantizer grid", "[quantizer]")
{
    const Quantizer q(16);
    AudioBuffer buffer(1, 8192);
    fillArbitrary(buffer.channel(0), 0.9f);
    q.process(buffer.channel(0), 42);

    for (std::size_t i = 0; i < buffer.numFrames(); ++i)
    {
        const float v = buffer.channel(0)[i];
        REQUIRE(v >= -1.0f);
        REQUIRE(v <= 1.0f);
        const float code = v / q.step();
        REQUIRE(code == std::floor(code));
    }
}

TEST_CASE("quantizer: different seeds produce different dither", "[quantizer]")
{
    const Quantizer q(16);
    AudioBuffer a(1, 8192);
    AudioBuffer b(1, 8192);
    fillArbitrary(a.channel(0), 0.9f);
    fillArbitrary(b.channel(0), 0.9f);

    q.process(a.channel(0), 1);
    q.process(b.channel(0), 2);

    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.numFrames(); ++i)
        if (a.channel(0)[i] != b.channel(0)[i])
            ++differing;
    REQUIRE(differing > 0);
}

TEST_CASE("quantizer: TPDF dither decorrelates quantization error from the signal",
          "[quantizer]")
{
    // A low-level ramp through a handful of 16-bit codes: undithered, the
    // error is a deterministic staircase; TPDF dither averages each input
    // level toward its true value. Check the dithered mean error per region
    // is closer to zero than the undithered one for a worst-case mid-rise
    // offset (x = code + 0.5 step would tie; use 0.3 step).
    const Quantizer q(16);
    const float offset = 0.3f * q.step();

    constexpr std::size_t frames = 32768;
    AudioBuffer plain(1, frames);
    AudioBuffer dithered(1, frames);
    for (std::size_t i = 0; i < frames; ++i)
    {
        plain.channel(0)[i] = offset;
        dithered.channel(0)[i] = offset;
    }

    q.process(plain.channel(0));
    q.process(dithered.channel(0), 7);

    double plainErr = 0.0;
    double ditherErr = 0.0;
    for (std::size_t i = 0; i < frames; ++i)
    {
        plainErr += static_cast<double>(plain.channel(0)[i]) - offset;
        ditherErr += static_cast<double>(dithered.channel(0)[i]) - offset;
    }
    plainErr /= static_cast<double>(frames);
    ditherErr /= static_cast<double>(frames);

    REQUIRE(std::abs(ditherErr) < std::abs(plainErr));
    // Dithered mean error well under 5% of a step.
    REQUIRE(std::abs(ditherErr) < 0.05 * static_cast<double>(q.step()));
}
