// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// akzcheck — self-test of the Akaizer secondary-cross-check comparison MATH on
// SYNTHETIC data (task plan/backlog/026c-akaizer-crosscheck.md). It deliberately
// needs NO Akaizer binary (closed payware, not in CI — testing-strategy.md §8):
// it constructs schedules and comb signals analytically and checks the math.
//
// Every test-case name begins with the tag word "akzcheck" so `ctest -R akzcheck`
// selects it (plan/backlog/README.md silent-pass rule); the [akzcheck] tag is
// carried for label-style selection too. This is the ONLY Akaizer-related ctest,
// and it is registered from tools/akaizer_crosscheck/CMakeLists.txt — NOT from
// tests/CMakeLists.txt and NOT from any CI config (026c acceptance criterion).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>

#include "mws/core/Buffer.h"

#include "CrossCheck.h"

namespace {

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using mws::core::AudioBuffer;

constexpr double kPi = std::numbers::pi_v<double>;

} // namespace

TEST_CASE("akzcheck: certified window filter is T 120-2000% inclusive", "[akzcheck]")
{
    // Below the window: compression has no Akaizer oracle.
    CHECK_FALSE(mws::akz::inWindow(25.0));
    CHECK_FALSE(mws::akz::inWindow(100.0));
    CHECK_FALSE(mws::akz::inWindow(119.999));
    // Inside the window.
    CHECK(mws::akz::inWindow(120.0));      // lower boundary inclusive
    CHECK(mws::akz::inWindow(300.0));
    CHECK(mws::akz::inWindow(2000.0));     // upper boundary inclusive
    // Above the window.
    CHECK_FALSE(mws::akz::inWindow(2000.001));
}

TEST_CASE("akzcheck: out-of-window cases are skipped with a no-oracle note",
          "[akzcheck]")
{
    // A compression case (50%) — outside the window, so analyzeCase must skip
    // it and never fabricate a schedule comparison.
    const auto r = mws::akz::analyzeCase("s1000_classic_50", /*T%*/ 50.0,
                                         /*C*/ 1000, /*N*/ 44100, /*rate*/ 44100.0);
    CHECK(r.skipped);
    CHECK(r.skipReason.find("no Akaizer oracle") != std::string::npos);
    CHECK(r.skipReason.find("120") != std::string::npos);
    CHECK(r.skipReason.find("2000") != std::string::npos);
    // Skipped => no schedule was computed.
    CHECK(r.schedule.hopIn == 0);
    CHECK(r.predictedFlutterHz == 0.0);
}

TEST_CASE("akzcheck: CLASSIC schedule matches the dsp-engine §3.4 formula",
          "[akzcheck]")
{
    // Hand-computed reference, F = 0.20:
    //   C = 1000, T = 2.0 (200%), N = 100000
    //   hop_out = 1000 * (1 - 0.20) = 800
    //   hop_in  = round(800 / 2.0)  = 400
    //   grains  = floor((100000 - 1000)/400) + 1 = floor(247.5)+1 = 247 + 1 = 248
    //   length  = (248 - 1)*800 + 1000 = 247*800 + 1000 = 197600 + 1000 = 198600
    //   realizedRatio = 800 / 400 = 2.0
    const auto s = mws::akz::classicSchedule(/*N*/ 100000, /*C*/ 1000, /*T*/ 2.0);
    CHECK_THAT(s.hopOut, WithinAbs(800.0, 1e-9));
    CHECK(s.hopIn == 400);
    CHECK(s.grains == 248);
    CHECK(s.outputLen == 198600);
    CHECK_THAT(s.realizedRatio, WithinAbs(2.0, 1e-9));
}

TEST_CASE("akzcheck: integer hop rounding produces 'bad timing' (not round(N·T))",
          "[akzcheck]")
{
    // T = 175% (1.75): hop_in = round(800/1.75) = round(457.14) = 457, so the
    // realized ratio (800/457 = 1.7505...) is NOT exactly 1.75 — the quantized
    // "bad timing" property (akaizer-analysis.md §2.2). The schedule-derived
    // length must come from the scheduler, never from round(N·T).
    const long N = 50000;
    const auto s = mws::akz::classicSchedule(N, /*C*/ 1000, /*T*/ 1.75);
    CHECK(s.hopIn == 457);
    CHECK_THAT(s.realizedRatio, WithinRel(800.0 / 457.0, 1e-12));
    CHECK(s.realizedRatio != 1.75);
    // The schedule length differs from the naive round(N*T) = round(50000*1.75)
    // = 87500: the integer scheduler quantizes it.
    CHECK(s.outputLen != static_cast<long>(std::lround(N * 1.75)));
}

TEST_CASE("akzcheck: cycle clamps to input length when the file is shorter",
          "[akzcheck]")
{
    // Input shorter than C: C is clamped to N (dsp-engine §3.4, AKZ §2.1 v1.6).
    const auto s = mws::akz::classicSchedule(/*N*/ 500, /*C*/ 1000, /*T*/ 3.0);
    // hop_out = 500*0.8 = 400; hop_in = round(400/3) = 133; only one grain
    // (N == C after clamp) so grains = 1, length = C = 500.
    CHECK_THAT(s.hopOut, WithinAbs(400.0, 1e-9));
    CHECK(s.grains == 1);
    CHECK(s.outputLen == 500);
}

TEST_CASE("akzcheck: hop_in floors at 1 for extreme stretch", "[akzcheck]")
{
    // Tiny cycle + huge T would round hop_in below 1; CLASSIC requires >= 1.
    const auto s = mws::akz::classicSchedule(/*N*/ 10000, /*C*/ 20, /*T*/ 20.0);
    // hop_out = 20*0.8 = 16; round(16/20) = round(0.8) = 1.
    CHECK(s.hopIn == 1);
}

TEST_CASE("akzcheck: predicted flutter is modelRate / hop_out", "[akzcheck]")
{
    // Splice-comb sideband spacing (testing-strategy §3 property 8).
    CHECK_THAT(mws::akz::predictedFlutterHz(/*hopOut*/ 800.0, /*rate*/ 44100.0),
               WithinRel(44100.0 / 800.0, 1e-12));
    // Degenerate inputs => 0.
    CHECK(mws::akz::predictedFlutterHz(0.0, 44100.0) == 0.0);
    CHECK(mws::akz::predictedFlutterHz(800.0, 0.0) == 0.0);
}

TEST_CASE("akzcheck: peak-normalization scales both sides to the same peak",
          "[akzcheck]")
{
    // A render at peak 0.25 and another at peak 0.80 must normalize to the same
    // peak (1.0) so a flutter/level comparison is amplitude-independent. This is
    // the procedural invariant: BOTH sides normalized (dsp-engine §7.3).
    AudioBuffer quiet(1, 16);
    quiet.sampleRate = 44100.0;
    auto qv = quiet.channel(0);
    for (std::size_t n = 0; n < qv.size(); ++n)
        qv[n] = (n == 3) ? 0.25f : 0.10f;

    AudioBuffer loud(1, 16);
    loud.sampleRate = 44100.0;
    auto lv = loud.channel(0);
    for (std::size_t n = 0; n < lv.size(); ++n)
        lv[n] = (n == 7) ? -0.80f : 0.30f;

    mws::akz::peakNormalize(quiet);
    mws::akz::peakNormalize(loud);

    double qpeak = 0.0, lpeak = 0.0;
    for (std::size_t n = 0; n < qv.size(); ++n)
    {
        qpeak = std::max(qpeak, std::fabs(static_cast<double>(qv[n])));
        lpeak = std::max(lpeak, std::fabs(static_cast<double>(lv[n])));
    }
    CHECK_THAT(qpeak, WithinAbs(1.0, 1e-6));
    CHECK_THAT(lpeak, WithinAbs(1.0, 1e-6));
}

TEST_CASE("akzcheck: peak-normalization preserves stereo balance and silence",
          "[akzcheck]")
{
    // One common gain across channels (stereo balance preserved): a 0.5/0.25 L/R
    // pair stays 2:1 after normalization (L -> 1.0, R -> 0.5).
    AudioBuffer st(2, 8);
    st.sampleRate = 44100.0;
    auto L = st.channel(0);
    auto R = st.channel(1);
    for (std::size_t n = 0; n < L.size(); ++n) { L[n] = 0.5f; R[n] = 0.25f; }
    mws::akz::peakNormalize(st);
    CHECK_THAT(static_cast<double>(L[0]), WithinAbs(1.0, 1e-6));
    CHECK_THAT(static_cast<double>(R[0]), WithinAbs(0.5, 1e-6));

    // A silent buffer is left untouched (no divide-by-zero).
    AudioBuffer silent(1, 8);
    silent.sampleRate = 44100.0;
    mws::akz::peakNormalize(silent);
    CHECK(silent.channel(0)[0] == 0.0f);
}

TEST_CASE("akzcheck: dominant-frequency measurement finds a synthetic comb tone",
          "[akzcheck]")
{
    // Build a pure tone at the predicted flutter frequency and confirm the FFT
    // measurement recovers it within one bin — this is how the harness reads the
    // splice-comb peak off a (normalized) render, no Akaizer binary involved.
    const double rate = 44100.0;
    const std::size_t n = 16384; // power of two -> exact FFT length
    const double flutterHz = mws::akz::predictedFlutterHz(800.0, rate); // 55.125
    AudioBuffer buf(1, n);
    buf.sampleRate = rate;
    auto v = buf.channel(0);
    const double w = 2.0 * kPi * flutterHz / rate;
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(0.5 * std::sin(w * static_cast<double>(i)));
    mws::akz::peakNormalize(buf);

    const double measured = mws::akz::measureDominantHz(buf);
    const double binHz = rate / static_cast<double>(n);
    CHECK_THAT(measured, WithinAbs(flutterHz, binHz));
}

TEST_CASE("akzcheck: an in-window case yields a full analytic report row",
          "[akzcheck]")
{
    // The §9 'Jungle Amen 300' setting (C=1000, T=300%) on a ~2 s 44.1k input.
    const long N = 88200; // 2 s @ 44.1k
    const auto r = mws::akz::analyzeCase("s1100_jungle_amen_300", /*T%*/ 300.0,
                                         /*C*/ 1000, N, /*rate*/ 44100.0);
    REQUIRE_FALSE(r.skipped);
    // hop_out = 800; hop_in = round(800/3) = 267; flutter = 44100/800 = 55.125.
    CHECK(r.schedule.hopIn == 267);
    CHECK_THAT(r.predictedFlutterHz, WithinRel(44100.0 / 800.0, 1e-12));
    CHECK(r.schedule.outputLen > N); // a 300% stretch is longer than the input
}
