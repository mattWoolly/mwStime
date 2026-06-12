// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CyclicEngine REVISED property tests — the "modern timing" hop mode of
// docs/design/testing-strategy.md §3 item 3, written FIRST per
// plan/backlog/011-cyclic-engine-revised.md (TDD mandatory).
//
// REVISED keeps the adopted [AKZ §4.2] two-grain scheduler but advances the
// input read with a FRACTIONAL hop (`hop_in = hop_out / T`), giving sample-exact
// output timing (output length = round(N·T)) at the cost of a slight sub-cycle
// pitch drift — the fractional grain start makes every source read a 2-point
// linear interpolation (dsp-engine.md §3.2 REVISED line, §3.4 REVISED note;
// akaizer-analysis.md §2.2 "perfect timing … pitch drifting ever so slightly").
//
// Expectations are computed INDEPENDENTLY from the §3.1–§3.4 scheduler — never
// from the §10 splice formula and never by calling the engine's own scheduling
// helpers (testing-strategy.md §3). Test-case names begin with the tag word so
// `ctest -R cyclic` matches (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include "mws/stretch/CyclicEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::engine::HopMode;
using mws::stretch::CyclicEngine;

/// Index-encoded source: src[n] = n. A linear ramp is interpolated EXACTLY by
/// 2-point linear interpolation, so it doubles as the REVISED interp probe:
/// the value read at a fractional source position p equals p itself.
AudioBuffer makeIndexEncodedSource(std::int64_t numFrames)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] = static_cast<float>(n);
    return buffer;
}

/// Independent re-derivation of the schedule constants shared by both hop modes
/// (dsp-engine.md §3.1–§3.2). REVISED differs from CLASSIC only in that hop_in
/// is fractional (no integer-% coercion, no rounding).
struct IndependentSchedule
{
    std::int64_t C = 0;
    std::int64_t ovStart = 0;
    std::int64_t hopOut = 0;
    double hopInClassic = 0.0; // round(hop_out / T)
    double hopInRevised = 0.0; // hop_out / T (fractional)
    double T = 0.0;

    IndependentSchedule(std::int64_t n, std::int64_t cycleLen, double timeFactorPct)
    {
        C = std::min(cycleLen, n);
        const double overlapF = static_cast<double>(CyclicEngine::SpliceCal{}.overlapF);
        ovStart = std::llround(static_cast<double>(C) * (1.0 - overlapF));
        ovStart = std::clamp<std::int64_t>(ovStart, 1, C);
        hopOut = ovStart;
        T = timeFactorPct / 100.0;
        hopInRevised = static_cast<double>(hopOut) / T;
        const auto tPct = std::max<std::int64_t>(1, std::llround(timeFactorPct));
        hopInClassic = std::max<std::int64_t>(
            1, std::llround(static_cast<double>(hopOut) * 100.0 / static_cast<double>(tPct)));
    }

    /// CLASSIC length (schedule-quantized): (G-1)*hop_out + C with the integer
    /// hop. Used by the regression-guard case.
    [[nodiscard]] std::int64_t classicLength(std::int64_t n) const
    {
        if (n <= 0)
            return 0;
        const std::int64_t hopIn = static_cast<std::int64_t>(hopInClassic);
        const std::int64_t grains = (n - C) / hopIn + 1;
        return (grains - 1) * hopOut + C;
    }
};

/// 2-point linear interpolation with the engine's edge rule (reads past the end
/// return 0; dsp-engine.md §3.4). For an index-encoded ramp this returns the
/// fractional position itself while it stays inside the buffer.
double interpRead(ConstAudioView src, double pos)
{
    const std::int64_t n = static_cast<std::int64_t>(src.size());
    const std::int64_t i0 = static_cast<std::int64_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i0);
    const auto at = [&](std::int64_t i) -> double {
        return (i >= 0 && i < n) ? static_cast<double>(src[static_cast<std::size_t>(i)]) : 0.0;
    };
    return at(i0) + (at(i0 + 1) - at(i0)) * frac;
}
} // namespace

TEST_CASE("cyclic: revised timing is sample-exact (output length within 1 of N*T)",
          "[cyclic][revised]")
{
    // Property test 3 (testing-strategy.md §3): REVISED output length is within
    // +/-1 sample of N*T for fractional time factors — the defining contrast
    // with CLASSIC's quantized "bad timing" (akaizer-analysis.md §2.2).
    struct Combo
    {
        std::int64_t n;
        int cycleLen;
        double timeFactorPct;
    };
    const Combo combos[] = {
        { 6000, 1000, 137.50 }, // fractional stretch
        { 6000, 1000, 66.67 },  // fractional compression
        { 44100, 441, 150.25 }, // non-round cycle and factor
        { 8000, 500, 312.50 },
        { 5000, 777, 199.99 },
        { 3000, 250, 1750.33 },
    };

    const CyclicEngine engine;
    for (const auto& combo : combos)
    {
        INFO("N=" << combo.n << " C=" << combo.cycleLen << " T=" << combo.timeFactorPct);
        const AudioBuffer src = makeIndexEncodedSource(combo.n);
        const AudioBuffer out =
            engine.render(src.channel(0), combo.cycleLen, combo.timeFactorPct,
                          HopMode::Revised);
        REQUIRE(out.numChannels() == 1);

        const double exact = static_cast<double>(combo.n) * combo.timeFactorPct / 100.0;
        const std::int64_t len = static_cast<std::int64_t>(out.numFrames());
        REQUIRE(std::abs(static_cast<double>(len) - exact) <= 1.0);

        // The engine's own length helper agrees with what it rendered.
        REQUIRE(engine.expectedOutputLength(combo.n, combo.cycleLen,
                                            combo.timeFactorPct, HopMode::Revised)
                == len);
    }
}

TEST_CASE("cyclic: revised does not coerce the time factor to integer percent",
          "[cyclic][revised]")
{
    // Unlike CLASSIC, REVISED honours the 0.01% step (dsp-engine.md §2 table;
    // akaizer-analysis.md §2.1 "two decimal places allowed only in REVISED").
    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(6000);

    const AudioBuffer a = engine.render(src.channel(0), 1000, 150.00, HopMode::Revised);
    const AudioBuffer b = engine.render(src.channel(0), 1000, 150.50, HopMode::Revised);
    const AudioBuffer c = engine.render(src.channel(0), 1000, 150.51, HopMode::Revised);

    // Distinct fractional factors yield distinct exact lengths (no integer snap).
    REQUIRE(a.numFrames() != b.numFrames());
    REQUIRE(b.numFrames() != c.numFrames());
}

TEST_CASE("cyclic: revised reads are 2-point linear interpolations of the source",
          "[cyclic][revised]")
{
    // dsp-engine.md §3.4 REVISED note: fractional B.off => source reads are
    // 2-point linear interpolation. On an index-encoded ramp the interpolated
    // read equals the fractional read position, so the engine output can be
    // checked sample-by-sample against the independently scheduled positions.
    const std::int64_t n = 6000;
    const int cycleLen = 1000;
    const double timeFactorPct = 137.50;

    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(n);
    const AudioBuffer out =
        engine.render(src.channel(0), cycleLen, timeFactorPct, HopMode::Revised);

    const IndependentSchedule s(n, cycleLen, timeFactorPct);
    const auto outView = out.channel(0);
    const auto srcView = src.channel(0);

    for (std::int64_t t = 0; t < static_cast<std::int64_t>(out.numFrames()); ++t)
    {
        const std::int64_t a = (t < s.C) ? 0 : (t - s.C) / s.hopOut + 1;
        const std::int64_t pos = t - a * s.hopOut;
        const double aReadPos = static_cast<double>(a) * s.hopInRevised + static_cast<double>(pos);

        // Stay clear of the input-exhaustion tail, where partial grains read 0
        // and the closed-form ramp identity no longer holds.
        if (aReadPos + 1.0 >= static_cast<double>(n))
            continue;

        const double sA = interpRead(srcView, aReadPos);
        double expected = sA;
        if (pos >= s.ovStart && s.ovStart < s.C)
        {
            const double bReadPos = static_cast<double>(a + 1) * s.hopInRevised
                                    + static_cast<double>(pos - s.ovStart);
            if (bReadPos + 1.0 >= static_cast<double>(n))
                continue;
            const double sB = interpRead(srcView, bReadPos);
            const double fade = static_cast<double>(pos - s.ovStart)
                                / static_cast<double>(s.C - s.ovStart);
            expected = sA + (sB - sA) * fade;
        }

        const double got = static_cast<double>(outView[static_cast<std::size_t>(t)]);
        INFO("t=" << t << " grain=" << a << " pos=" << pos << " expected=" << expected
                  << " got=" << got);
        // Tolerance covers the engine's float32/Q31 crossfade quantisation; the
        // interpolation itself is exact on a ramp.
        REQUIRE(std::abs(got - expected) <= 0.5);
    }
}

TEST_CASE("cyclic: revised with an integral hop matches CLASSIC (degenerate equivalence)",
          "[cyclic][revised]")
{
    // When hop_in = hop_out / T is integral, the fractional grain start has zero
    // fraction, every interpolation degenerates to a verbatim read, and the
    // schedule is identical to CLASSIC. REVISED therefore reproduces the CLASSIC
    // samples exactly; it only runs longer (to the sample-exact N*T length),
    // so the CLASSIC render is a bit-identical prefix of the REVISED render.
    struct Combo
    {
        std::int64_t n;
        int cycleLen;
        double timeFactorPct; // chosen so hop_out / T is an integer
    };
    const Combo combos[] = {
        { 6000, 1000, 200.0 }, // ovStart 800 -> hop_in 400
        { 6000, 1000, 400.0 }, // hop_in 200
        { 6000, 1000, 160.0 }, // hop_in 500
    };

    const CyclicEngine engine;
    for (const auto& combo : combos)
    {
        INFO("N=" << combo.n << " C=" << combo.cycleLen << " T=" << combo.timeFactorPct);
        const IndependentSchedule s(combo.n, combo.cycleLen, combo.timeFactorPct);
        // Guard: this case really is the integral-hop degenerate one.
        REQUIRE(s.hopInRevised == std::floor(s.hopInRevised));
        REQUIRE(s.hopInClassic == s.hopInRevised);

        const AudioBuffer src = makeIndexEncodedSource(combo.n);
        const AudioBuffer classic =
            engine.render(src.channel(0), combo.cycleLen, combo.timeFactorPct,
                          HopMode::Classic);
        const AudioBuffer revised =
            engine.render(src.channel(0), combo.cycleLen, combo.timeFactorPct,
                          HopMode::Revised);

        const std::int64_t classicLen = static_cast<std::int64_t>(classic.numFrames());
        REQUIRE(classicLen == s.classicLength(combo.n));
        REQUIRE(static_cast<std::int64_t>(revised.numFrames()) >= classicLen);

        const auto cv = classic.channel(0);
        const auto rv = revised.channel(0);
        for (std::int64_t i = 0; i < classicLen; ++i)
        {
            INFO("i=" << i);
            REQUIRE(rv[static_cast<std::size_t>(i)] == cv[static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("cyclic: revised leaves the CLASSIC path unchanged (regression guard)",
          "[cyclic][revised]")
{
    // Acceptance criterion: the task-010 CLASSIC expectation must still hold
    // exactly. Re-derive one canonical CLASSIC case independently and confirm
    // the engine output is unchanged by the addition of REVISED.
    const std::int64_t n = 6000;
    const int cycleLen = 1000;
    const double timeFactorPct = 300.0;

    const CyclicEngine engine;
    const AudioBuffer src = makeIndexEncodedSource(n);
    const AudioBuffer out =
        engine.render(src.channel(0), cycleLen, timeFactorPct, HopMode::Classic);

    const IndependentSchedule s(n, cycleLen, timeFactorPct);
    REQUIRE(static_cast<std::int64_t>(out.numFrames()) == s.classicLength(n));
    REQUIRE(engine.expectedOutputLength(n, cycleLen, timeFactorPct, HopMode::Classic)
            == s.classicLength(n));
}

TEST_CASE("cyclic: revised handles empty input", "[cyclic][revised]")
{
    const CyclicEngine engine;
    const AudioBuffer src(1, 0);
    const AudioBuffer out = engine.render(src.channel(0), 1000, 137.5, HopMode::Revised);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.numFrames() == 0);
    REQUIRE(engine.expectedOutputLength(0, 1000, 137.5, HopMode::Revised) == 0);
}
