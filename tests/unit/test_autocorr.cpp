// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/AutoCorr.h"
#include "mws/core/Buffer.h"

#include <cmath>
#include <random>
#include <vector>

// Test-case names begin with the tag word so `ctest -R autocorr` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::AutoCorr;
using mws::core::ConstAudioView;

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

std::vector<float> makeSine(double freqHz, int numFrames)
{
    std::vector<float> x(static_cast<std::size_t>(numFrames));
    for (int n = 0; n < numFrames; ++n)
        x[static_cast<std::size_t>(n)] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * freqHz * static_cast<double>(n)
                     / kSampleRate));
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

TEST_CASE("autocorr: 100 Hz saw at 44.1 kHz detects lag 441 +/- 1", "[autocorr]")
{
    // 100 Hz at 44100 Hz = period 441 samples; 20 full periods of signal.
    const auto saw = makeSaw(441, 441 * 20);

    // dsp-engine.md §7.1 parameters: lag range 20–2000, peak threshold 0.3.
    const auto lag = AutoCorr::bestLag(view(saw), 20, 2000, 0.3f);

    REQUIRE(lag.has_value());
    REQUIRE(*lag >= 440);
    REQUIRE(*lag <= 442);
}

TEST_CASE("autocorr: sub-threshold white noise returns nullopt", "[autocorr]")
{
    const auto noise = makeNoise(8192);

    // No periodicity: no normalized peak exceeds 0.3, so the estimator
    // reports nothing and the caller falls back to 1000 (dsp-engine.md §7.1).
    const auto lag = AutoCorr::bestLag(view(noise), 20, 2000, 0.3f);

    REQUIRE_FALSE(lag.has_value());
}

TEST_CASE("autocorr: bestLagNear finds 220 Hz sine period from 1.5x-off center",
          "[autocorr]")
{
    // 220 Hz at 44100 Hz: true period 200.4545… samples -> best integer lag 200.
    const auto sine = makeSine(220.0, 8192);

    // MON1 "around C" search (dsp-engine.md §5): centered at 1.5x the true
    // lag, the search window still recovers the fundamental period.
    const int center = static_cast<int>(std::lround(1.5 * kSampleRate / 220.0));
    const auto lag = AutoCorr::bestLagNear(view(sine), center, 0.5, 0.3f);

    REQUIRE(lag.has_value());
    REQUIRE(*lag >= 199);
    REQUIRE(*lag <= 201);
}

TEST_CASE("autocorr: same input gives same result (determinism)", "[autocorr]")
{
    const auto saw = makeSaw(441, 441 * 20);
    const auto sine = makeSine(220.0, 8192);
    const int center = static_cast<int>(std::lround(1.5 * kSampleRate / 220.0));

    const auto first = AutoCorr::bestLag(view(saw), 20, 2000, 0.3f);
    const auto second = AutoCorr::bestLag(view(saw), 20, 2000, 0.3f);
    REQUIRE(first.has_value());
    REQUIRE(first == second);

    const auto nearFirst = AutoCorr::bestLagNear(view(sine), center, 0.5, 0.3f);
    const auto nearSecond = AutoCorr::bestLagNear(view(sine), center, 0.5, 0.3f);
    REQUIRE(nearFirst.has_value());
    REQUIRE(nearFirst == nearSecond);
}
