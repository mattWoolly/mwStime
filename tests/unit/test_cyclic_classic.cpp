// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CyclicEngine CLASSIC property tests — the authenticity contracts of
// docs/design/testing-strategy.md §3 items 1–2, written FIRST per
// plan/backlog/010-cyclic-engine-classic.md (TDD mandatory).
//
// The expectations below are computed INDEPENDENTLY from the adopted
// [AKZ §4.2] scheduler as specified in docs/design/dsp-engine.md §3.1–§3.4 —
// never from the §10 splice formula, and never by calling into the engine's
// own scheduling code (testing-strategy.md §3.2).
//
// Test-case names begin with the tag word so `ctest -R cyclic` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include "mws/stretch/CyclicEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::engine::HopMode;
using mws::stretch::CyclicEngine;

/// Index-encoded source: src[n] = n (testing-strategy.md §3.1). Every value is
/// exactly representable in float32 for n < 2^24.
AudioBuffer makeIndexEncodedSource(std::int64_t numFrames)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] = static_cast<float>(n);
    return buffer;
}

/// Independent re-derivation of the CLASSIC schedule constants from
/// dsp-engine.md §3.1–§3.2 (NOT from the engine):
///   C       = cycle length, clamped to N for short input [AKZ §2.1]
///   ovStart = C * (1 - F), F = SpliceCal default overlap
///   hop_out = ovStart
///   hop_in  = round(hop_out / T) with T = round(timeFactorPct) / 100
///             (integer-% coercion, [AKZ §2.1]), clamped >= 1
struct IndependentSchedule
{
    std::int64_t C = 0;
    std::int64_t ovStart = 0;
    std::int64_t hopOut = 0;
    std::int64_t hopIn = 0;

    IndependentSchedule(std::int64_t n, std::int64_t cycleLen, double timeFactorPct)
    {
        C = std::min(cycleLen, n);
        // Default overlap fraction comes from SpliceCal — the single home of
        // the splice constants (acceptance criterion of task 010).
        const double overlapF = static_cast<double>(CyclicEngine::SpliceCal{}.overlapF);
        ovStart = std::llround(static_cast<double>(C) * (1.0 - overlapF));
        ovStart = std::clamp<std::int64_t>(ovStart, 1, C);
        hopOut = ovStart;
        const auto tPct = std::max<std::int64_t>(1, std::llround(timeFactorPct));
        hopIn = std::max<std::int64_t>(
            1, std::llround(static_cast<double>(hopOut) * 100.0 / static_cast<double>(tPct)));
    }

    /// Length derived from THIS scheduler (testing-strategy.md §3.2):
    /// G = floor((N - C) / hop_in) + 1 complete grains; (G-1)*hop_out + C.
    [[nodiscard]] std::int64_t expectedLength(std::int64_t n) const
    {
        if (n <= 0)
            return 0;
        const std::int64_t grains = (n - C) / hopIn + 1;
        return (grains - 1) * hopOut + C;
    }
};

/// Reads like the engine's edge rule: past N-1 returns 0 (dsp-engine.md §3.4).
double srcRead(std::int64_t index, std::int64_t n)
{
    return (index < n) ? static_cast<double>(index) : 0.0;
}

/// Verbatim-copy property (testing-strategy.md §3.1) checked sample by sample
/// against a closed-form reading of the §3.4 scheduler:
///  - grain a is the playing grain A for t in [(a-1)*hopOut + C, a*hopOut + C)
///    (grain 0 from t = 0); its read position is a*hopIn + (t - a*hopOut),
///  - the crossfade is active iff pos >= ovStart, with grain a+1 as B at read
///    (a+1)*hopIn + (pos - ovStart) and linear fade (pos-ovStart)/(C-ovStart).
/// Outside fades: output must equal src at an integer offset EXACTLY.
/// Inside fades: output must be the rounded convex combination of exactly the
/// two scheduled samples (tolerance 0.5 = the rounding allowance of the
/// property statement; dsp-engine.md §3.2).
void checkVerbatimProperty(ConstAudioView out, std::int64_t n,
                           const IndependentSchedule& s)
{
    for (std::int64_t t = 0; t < static_cast<std::int64_t>(out.size()); ++t)
    {
        const std::int64_t a = (t < s.C) ? 0 : (t - s.C) / s.hopOut + 1;
        const std::int64_t pos = t - a * s.hopOut;
        const std::int64_t aRead = a * s.hopIn + pos;
        const float outSample = out[static_cast<std::size_t>(t)];

        if (pos < s.ovStart || s.ovStart >= s.C)
        {
            // Verbatim region: an exact copy of src at an integer offset.
            const double expected = srcRead(aRead, n);
            INFO("t=" << t << " grain=" << a << " pos=" << pos << " read=" << aRead);
            REQUIRE(outSample == static_cast<float>(expected));
        }
        else
        {
            // Crossfade region: rounded convex combination of exactly two
            // input samples (linear complementary fade, dsp-engine.md §3.3).
            const std::int64_t bRead = (a + 1) * s.hopIn + (pos - s.ovStart);
            const double sA = srcRead(aRead, n);
            const double sB = srcRead(bRead, n);
            const double fade = static_cast<double>(pos - s.ovStart)
                                / static_cast<double>(s.C - s.ovStart);
            const double combo = sA + (sB - sA) * fade;
            INFO("t=" << t << " grain=" << a << " pos=" << pos << " sA=" << sA
                      << " sB=" << sB << " fade=" << fade << " out=" << outSample);
            REQUIRE(static_cast<double>(outSample)
                    >= std::min(sA, sB) - 0.5);
            REQUIRE(static_cast<double>(outSample)
                    <= std::max(sA, sB) + 0.5);
            REQUIRE(std::abs(static_cast<double>(outSample) - combo) <= 0.5);
        }
    }
}
} // namespace

TEST_CASE("cyclic: classic verbatim-copy property (stretch alone never resamples)",
          "[cyclic][classic]")
{
    // Property test 1 (testing-strategy.md §3.1): with src[n] = n, every output
    // sample is src[i] at an integer offset or, inside a crossfade region only,
    // the rounded convex combination of exactly two such samples.
    struct Combo
    {
        std::int64_t n;
        int cycleLen;
        double timeFactorPct;
    };
    const Combo combos[] = {
        { 6000, 1000, 300.0 },  // canonical stretch
        { 6000, 1000, 50.0 },   // compression: hop_in > hop_out skips material
        { 5000, 441, 177.0 },   // non-round cycle and factor
        { 3000, 250, 2000.0 },  // superset maximum factor
    };

    const CyclicEngine engine;
    for (const auto& combo : combos)
    {
        INFO("N=" << combo.n << " C=" << combo.cycleLen << " T=" << combo.timeFactorPct);
        const AudioBuffer src = makeIndexEncodedSource(combo.n);
        const AudioBuffer out =
            engine.render(src.channel(0), combo.cycleLen, combo.timeFactorPct,
                          HopMode::Classic);
        REQUIRE(out.numChannels() == 1);

        const IndependentSchedule schedule(combo.n, combo.cycleLen, combo.timeFactorPct);
        REQUIRE(out.numFrames() > 0);
        checkVerbatimProperty(out.channel(0), combo.n, schedule);
    }
}

TEST_CASE("cyclic: classic length quantization follows the grain schedule, not round(N*T)",
          "[cyclic][classic]")
{
    // Property test 2 (testing-strategy.md §3.2): output length equals the
    // independently computed (G-1)*hop_out + C with hop_in = round(hop_out/T),
    // and differs from round(N*T) for at least one tested (N, C, T).
    struct Combo
    {
        std::int64_t n;
        int cycleLen;
        double timeFactorPct;
    };
    const Combo combos[] = {
        { 6000, 1000, 300.0 },
        { 44100, 441, 150.0 },
        { 6000, 1000, 50.0 },   // T < 100%: same schedule formula (compression)
        { 3000, 250, 2000.0 },
        { 10000, 2000, 125.0 },
    };

    const CyclicEngine engine;
    bool divergesFromNaiveLength = false;
    for (const auto& combo : combos)
    {
        INFO("N=" << combo.n << " C=" << combo.cycleLen << " T=" << combo.timeFactorPct);
        const AudioBuffer src = makeIndexEncodedSource(combo.n);
        const AudioBuffer out =
            engine.render(src.channel(0), combo.cycleLen, combo.timeFactorPct,
                          HopMode::Classic);

        const IndependentSchedule schedule(combo.n, combo.cycleLen, combo.timeFactorPct);
        const std::int64_t expected = schedule.expectedLength(combo.n);
        REQUIRE(static_cast<std::int64_t>(out.numFrames()) == expected);

        // The engine's own helper must agree with the schedule it implements.
        REQUIRE(engine.expectedOutputLength(combo.n, combo.cycleLen,
                                            combo.timeFactorPct, HopMode::Classic)
                == expected);

        const auto naive = std::llround(static_cast<double>(combo.n)
                                        * combo.timeFactorPct / 100.0);
        if (expected != naive)
            divergesFromNaiveLength = true;
    }
    // "Bad timing" [AKZ §2.2]: the schedule-quantized length is NOT round(N*T).
    REQUIRE(divergesFromNaiveLength);
}

TEST_CASE("cyclic: classic coerces the time factor to integer percent", "[cyclic][classic]")
{
    // CLASSIC: "Akai samplers don't use decimal values for Time Factor"
    // [AKZ §2.1]; dsp-engine.md §3.2.
    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(4000);

    const AudioBuffer exact = engine.render(src.channel(0), 500, 300.0, HopMode::Classic);
    const AudioBuffer fractional =
        engine.render(src.channel(0), 500, 300.4, HopMode::Classic);

    REQUIRE(exact.numFrames() == fractional.numFrames());
    const auto a = exact.channel(0);
    const auto b = fractional.channel(0);
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(a[i] == b[i]);
}

TEST_CASE("cyclic: classic hop_in clamps to >= 1 at extreme time factors",
          "[cyclic][classic]")
{
    // Raw hop would round to 0 (ovStart=16, T=5000% -> 0.32); the engine must
    // clamp it to 1 (dsp-engine.md §3.4 "require hop_in >= 1").
    const std::int64_t n = 40;
    const int cycleLen = 20;
    const double timeFactorPct = 5000.0; // deliberately beyond the superset:
                                         // range clamping is ModelSpec's job,
                                         // hop safety is the engine's.

    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(n);
    const AudioBuffer out =
        engine.render(src.channel(0), cycleLen, timeFactorPct, HopMode::Classic);

    const IndependentSchedule schedule(n, cycleLen, timeFactorPct);
    REQUIRE(schedule.hopIn == 1); // the clamp is active for this combo
    const std::int64_t expected = schedule.expectedLength(n); // 20*16 + 20 = 340
    REQUIRE(expected == 340);
    REQUIRE(static_cast<std::int64_t>(out.numFrames()) == expected);
    checkVerbatimProperty(out.channel(0), n, schedule);
}

TEST_CASE("cyclic: classic clamps the cycle length to short input", "[cyclic][classic]")
{
    // C is clamped to N when the file is shorter than one cycle
    // [AKZ §2.1, v1.6 behavior]; dsp-engine.md §3.4.
    const std::int64_t n = 500;
    const int cycleLen = 1000; // longer than the input
    const double timeFactorPct = 200.0;

    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(n);
    const AudioBuffer out =
        engine.render(src.channel(0), cycleLen, timeFactorPct, HopMode::Classic);

    const IndependentSchedule schedule(n, cycleLen, timeFactorPct);
    REQUIRE(schedule.C == n); // the clamp is active
    REQUIRE(static_cast<std::int64_t>(out.numFrames()) == schedule.expectedLength(n));
    checkVerbatimProperty(out.channel(0), n, schedule);
}

TEST_CASE("cyclic: classic render is deterministic (bit-identical reruns)",
          "[cyclic][classic]")
{
    // Acceptance criterion of task 010 (full determinism suite is task 012).
    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(6000);

    const AudioBuffer first = engine.render(src.channel(0), 1000, 300.0, HopMode::Classic);
    const AudioBuffer second = engine.render(src.channel(0), 1000, 300.0, HopMode::Classic);

    REQUIRE(first.numFrames() == second.numFrames());
    const auto a = first.channel(0);
    const auto b = second.channel(0);
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(a[i] == b[i]); // bit-identical: float == on identical bits
}

TEST_CASE("cyclic: classic handles empty input", "[cyclic][classic]")
{
    const CyclicEngine engine;
    const AudioBuffer src(1, 0);
    const AudioBuffer out = engine.render(src.channel(0), 1000, 300.0, HopMode::Classic);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.numFrames() == 0);
    REQUIRE(engine.expectedOutputLength(0, 1000, 300.0, HopMode::Classic) == 0);
}

TEST_CASE("cyclic: revised mode is now implemented (task 011)", "[cyclic][classic]")
{
    // Task 011 replaced the REVISED stub: it must render without throwing and
    // produce the sample-exact length round(N*T). Full REVISED property
    // coverage lives in test_cyclic_revised.cpp.
    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(100);
    AudioBuffer out;
    REQUIRE_NOTHROW(out = engine.render(src.channel(0), 50, 150.0, HopMode::Revised));
    REQUIRE(out.numFrames() == 150); // round(100 * 1.5)
    REQUIRE(engine.expectedOutputLength(100, 50, 150.0, HopMode::Revised) == 150);
}
