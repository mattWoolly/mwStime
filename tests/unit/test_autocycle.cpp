// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/stretch/AutoCycle.h"

#include <random>
#include <vector>

// Test-case names begin with the tag word so `ctest -R autocycle` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::ConstAudioView;
using mws::stretch::AutoCycle;

constexpr double kSampleRate = 44100.0;

/// Exactly periodic sawtooth in [-1, 1): integer period, so x[n + period] ==
/// x[n] bit-for-bit — the autocorrelation peak at `period` is exact.
std::vector<float> makeSaw(int period, int numFrames)
{
    std::vector<float> x(static_cast<std::size_t>(numFrames));
    for (int n = 0; n < numFrames; ++n)
        x[static_cast<std::size_t>(n)] =
            2.0f * (static_cast<float>(n % period) / static_cast<float>(period)) - 1.0f;
    return x;
}

/// Deterministic white noise (fixed seed) in [-1, 1].
std::vector<float> makeNoise(int numFrames)
{
    std::minstd_rand rng(1234u);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> x(static_cast<std::size_t>(numFrames));
    for (auto& sample : x)
        sample = dist(rng);
    return x;
}

ConstAudioView view(const std::vector<float>& x)
{
    return { x.data(), x.size() };
}
} // namespace

TEST_CASE("autocycle: 100 Hz saw at 44.1 kHz proposes cycle length 441 +/- 1",
          "[autocycle]")
{
    // 100 Hz at 44100 Hz = period 441 samples. One second of signal — longer
    // than the 200 ms analysis window, so the window cap is exercised too.
    const auto saw = makeSaw(441, 44100);

    const int cycleLen = AutoCycle::detectCycleLen(view(saw), kSampleRate);

    REQUIRE(cycleLen >= 440);
    REQUIRE(cycleLen <= 442);
}

TEST_CASE("autocycle: white noise falls back to exactly 1000", "[autocycle]")
{
    // No periodicity: no normalized autocorrelation peak exceeds the 0.3
    // threshold, so the helper proposes the documented fallback of 1000
    // (dsp-engine.md §7.1 (PI)).
    const auto noise = makeNoise(44100);

    REQUIRE(AutoCycle::detectCycleLen(view(noise), kSampleRate) == 1000);
}

TEST_CASE("autocycle: zone shorter than 200 ms uses the whole zone",
          "[autocycle]")
{
    // 100 ms of 100 Hz saw (4410 frames < the 8820-frame 200 ms window):
    // detection still works on the full (short) zone.
    const auto saw = makeSaw(441, 4410);

    const int cycleLen = AutoCycle::detectCycleLen(view(saw), kSampleRate);

    REQUIRE(cycleLen >= 440);
    REQUIRE(cycleLen <= 442);
}

TEST_CASE("autocycle: PI constants live in one named-constants block",
          "[autocycle]")
{
    // QA retuning hook (testing-strategy.md §7: every (PI) constant needs a
    // tuning note): the §7.1 numbers are named constants in AutoCycle.h, not
    // literals buried in the implementation. Pin the spec values here so any
    // retune is a deliberate, reviewed change.
    STATIC_REQUIRE(AutoCycle::kAnalysisWindowSeconds == 0.2);
    STATIC_REQUIRE(AutoCycle::kPeakThreshold == 0.3f);
    STATIC_REQUIRE(AutoCycle::kFallbackCycleLen == 1000);
    STATIC_REQUIRE(AutoCycle::kLagMin == 20);
    STATIC_REQUIRE(AutoCycle::kLagMax == 2000);
}
