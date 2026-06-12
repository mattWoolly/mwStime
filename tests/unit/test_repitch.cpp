// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// RepitchEngine — S900 varispeed (ADR-003, dsp-engine.md §6). Honest repitch:
// rate = 1/T, transpose multiplies the same rate, ZOH read with NO
// interpolation [DRR F4].

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/stretch/RepitchEngine.h"

#include <cmath>
#include <set>
#include <utility>

// Test-case names begin with the tag word so `ctest -R repitch` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::stretch::RepitchEngine;
using mws::stretch::RepitchResult;

/// Mono source with N distinct, deterministic sample values, so verbatim
/// (bit-exact) ZOH copies can be told apart from any interpolated value.
AudioBuffer makeDistinctSource(std::size_t numFrames)
{
    AudioBuffer src(1, numFrames);
    auto view = src.channel(0);
    for (std::size_t i = 0; i < numFrames; ++i)
    {
        // Irrational-ish spread in (-1, 1); every value distinct.
        view[i] = static_cast<float>(
            std::sin(0.7361 * static_cast<double>(i) + 0.1234));
    }
    return src;
}
} // namespace

TEST_CASE("repitch: T=200% doubles length, ZOH emits only verbatim source samples",
          "[repitch]")
{
    constexpr std::size_t n = 256;
    const AudioBuffer src = makeDistinctSource(n);

    const RepitchResult result =
        RepitchEngine::render(src.channel(0), 200.0, 0.0);

    // rate = 100/200 = 0.5, transpose 0 => clockRatio 0.5.
    REQUIRE(result.clockRatio == 0.5);

    // Output length ~ 2N (+-1).
    const auto outLen = result.out.numFrames();
    REQUIRE(result.out.numChannels() == 1);
    REQUIRE(outLen >= 2 * n - 1);
    REQUIRE(outLen <= 2 * n + 1);

    // ZOH, no interpolation: out[k] == src[floor(k * clockRatio)] bit-exact.
    const ConstAudioView in = src.channel(0);
    const ConstAudioView out = std::as_const(result.out).channel(0);
    for (std::size_t k = 0; k < outLen; ++k)
    {
        const auto idx = static_cast<std::size_t>(
            std::floor(static_cast<double>(k) * result.clockRatio));
        REQUIRE(idx < n);
        REQUIRE(out[k] == in[idx]); // verbatim — any interpolation fails this
    }

    // Belt and braces: ZOH can create no new values at all.
    std::set<float> sourceValues(in.begin(), in.end());
    for (const float sample : out)
        REQUIRE(sourceValues.count(sample) == 1);
}

TEST_CASE("repitch: semitone offset LCD formula -12*log2(T)", "[repitch]")
{
    REQUIRE(RepitchEngine::semitoneOffset(200.0) == -12.0); // exact
    REQUIRE(RepitchEngine::semitoneOffset(100.0) == 0.0);   // exact
    REQUIRE(RepitchEngine::semitoneOffset(50.0) == 12.0);   // exact (up an octave)
}

TEST_CASE("repitch: transpose +12 at T=100% halves length, clockRatio 2.0",
          "[repitch]")
{
    constexpr std::size_t n = 256;
    const AudioBuffer src = makeDistinctSource(n);

    const RepitchResult result =
        RepitchEngine::render(src.channel(0), 100.0, 12.0);

    // rate = 1.0, transpose +12 => x2 => clockRatio 2.0 exactly.
    REQUIRE(result.clockRatio == 2.0);

    // round(N / 2) frames.
    REQUIRE(result.out.numFrames() == n / 2);

    // Still ZOH verbatim: out[k] == src[2k].
    const ConstAudioView in = src.channel(0);
    const ConstAudioView out = std::as_const(result.out).channel(0);
    for (std::size_t k = 0; k < result.out.numFrames(); ++k)
        REQUIRE(out[k] == in[2 * k]);
}

TEST_CASE("repitch: deterministic — two runs are bit-identical", "[repitch]")
{
    constexpr std::size_t n = 512;
    const AudioBuffer src = makeDistinctSource(n);

    // Non-trivial T and transpose so the read schedule has fractional steps.
    const RepitchResult a = RepitchEngine::render(src.channel(0), 137.5, -3.17);
    const RepitchResult b = RepitchEngine::render(src.channel(0), 137.5, -3.17);

    REQUIRE(a.clockRatio == b.clockRatio);
    REQUIRE(a.out.numFrames() == b.out.numFrames());
    REQUIRE(a.out.numChannels() == 1);
    REQUIRE(b.out.numChannels() == 1);

    const ConstAudioView outA = std::as_const(a.out).channel(0);
    const ConstAudioView outB = std::as_const(b.out).channel(0);
    for (std::size_t k = 0; k < outA.size(); ++k)
        REQUIRE(outA[k] == outB[k]); // bit-identical
}

TEST_CASE("repitch: empty source renders empty output", "[repitch]")
{
    const RepitchResult result =
        RepitchEngine::render(ConstAudioView{}, 200.0, 0.0);

    REQUIRE(result.out.numFrames() == 0);
    REQUIRE(result.clockRatio == 0.5);
}
