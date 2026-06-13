// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// S950Engine tests — written FIRST per plan/backlog/015-s950-engine.md (TDD).
// The contracts are the dsp-engine.md §5 mapping table rows: STRETCH % clamped
// at 999 [MAN §2], D-TIME == cycle length C in samples (PI), POL2 == fixed-C
// CyclicEngine CLASSIC (PI degenerate equivalence), MON1 == per-grain pitch-
// period snap via AutoCorr::bestLagNear (PI), AUTO-D == task-014 detectCycleLen.
//
// Test-case names begin with the tag word so `ctest -R s950` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include "mws/stretch/CyclicEngine.h"
#include "mws/stretch/S950Engine.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::engine::HopMode;
using mws::engine::Material;
using mws::stretch::CyclicEngine;
using mws::stretch::S950Engine;
using mws::stretch::S950Params;

constexpr double kSampleRate = 44100.0;

/// Steady sine at `freqHz`: the MON1/POL2 reference material. 220 Hz at
/// 44.1 kHz has pitch period 44100 / 220 = 200.4545... samples, so the
/// nearest-integer detected period is 200 (or 201 within the test tolerance).
AudioBuffer makeSine(double freqHz, std::int64_t numFrames)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    const double w = 2.0 * 3.14159265358979323846 * freqHz / kSampleRate;
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] =
            static_cast<float>(std::sin(w * static_cast<double>(n)));
    return buffer;
}

/// Exactly periodic sawtooth (period in samples) — the AUTO-D reference
/// material, same construction as test_autocycle.cpp: x[n + period] == x[n]
/// bit-for-bit, so the autocorrelation peak at `period` is exact.
AudioBuffer makeSaw(int period, std::int64_t numFrames)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] =
            2.0f
                * (static_cast<float>(n % period) / static_cast<float>(period))
            - 1.0f;
    return buffer;
}

void requireBitIdentical(ConstAudioView a, ConstAudioView b)
{
    REQUIRE(a.size() == b.size());
    for (std::size_t k = 0; k < a.size(); ++k)
    {
        INFO("k=" << k);
        REQUIRE(a[k] == b[k]); // bit-identical (float equality is exact here)
    }
}
} // namespace

TEST_CASE("s950: timeFactor 1500 behaves identically to 999 (engine clamp)",
          "[s950]")
{
    // The S950 stretches "up to 999%" [MAN §2]; the engine clamps the superset
    // timeFactor range there (dsp-engine.md §2 timeFactor row, §5).
    const auto sine = makeSine(220.0, 44100);
    const S950Engine engine;

    S950Params params;
    params.material = Material::Pol2;
    params.dTime = 1000;

    params.timeFactorPct = 1500.0;
    const auto over = engine.render(sine.channel(0), params);

    params.timeFactorPct = 999.0;
    const auto atMax = engine.render(sine.channel(0), params);

    REQUIRE(over.effectiveTimeFactorPct == 999.0);
    REQUIRE(atMax.effectiveTimeFactorPct == 999.0);
    requireBitIdentical(over.out.channel(0), atMax.out.channel(0));
}

TEST_CASE("s950: POL2 on a steady sine reproduces CyclicEngine CLASSIC "
          "bit-exactly for the same C/T",
          "[s950]")
{
    // POL2 = fixed C exactly as set (dsp-engine.md §5 (PI)) — the degenerate
    // equivalence: S950 POL2 IS CyclicEngine CLASSIC with C = D-TIME.
    const auto sine = makeSine(220.0, 44100);

    const S950Engine engine;
    S950Params params;
    params.material = Material::Pol2;
    params.dTime = 300;
    params.timeFactorPct = 200.0;
    const auto result = engine.render(sine.channel(0), params);

    const CyclicEngine cyclic;
    const auto reference =
        cyclic.render(sine.channel(0), 300, 200.0, HopMode::Classic);

    REQUIRE(result.effectiveCycleLen == 300);
    requireBitIdentical(result.out.channel(0), reference.channel(0));
}

TEST_CASE("s950: MON1 on a 220 Hz sine snaps C to the detected pitch period",
          "[s950]")
{
    // MON1 = per-grain pitch-period snap around the set C (dsp-engine.md §5
    // (PI, deviation-until-calibrated)). 220 Hz at 44.1 kHz has period
    // 200.4545... samples — the effective C must land within +/-1 of 200.5.
    // On a steady tone every grain snaps to the same period, so the output
    // must follow the snapped schedule, not the set D-TIME of 220.
    const std::int64_t n = 44100;
    const auto sine = makeSine(220.0, n);

    const S950Engine engine;
    S950Params params;
    params.material = Material::Mon1;
    params.dTime = 220; // deliberately off the true period: the snap must move it
    params.timeFactorPct = 200.0;
    const auto result = engine.render(sine.channel(0), params);

    REQUIRE(result.effectiveCycleLen >= 200);
    REQUIRE(result.effectiveCycleLen <= 201);

    // Output length follows the SNAPPED schedule (dsp-engine.md §3.4 formula
    // evaluated at the snapped C), not the set-C schedule.
    const CyclicEngine cyclic;
    REQUIRE(static_cast<std::int64_t>(result.out.numFrames())
            == cyclic.expectedOutputLength(n, result.effectiveCycleLen, 200.0,
                                           HopMode::Classic));

    // Steady tone => the per-grain snap is constant, so MON1 degenerates to
    // the fixed-C CLASSIC render at the snapped C. This pins the per-grain
    // scheduler bit-exactly against CyclicEngine (anti-drift guard for the
    // mirrored CLASSIC arithmetic).
    const auto reference = cyclic.render(sine.channel(0), result.effectiveCycleLen,
                                         200.0, HopMode::Classic);
    requireBitIdentical(result.out.channel(0), reference.channel(0));
}

TEST_CASE("s950: AUTO-D on a 100 Hz saw selects C = 441 +/- 1", "[s950]")
{
    // AUTO-D = run the task-014 auto-cycle detector and use the result as C
    // (dsp-engine.md §5; feature documented [MAN §2], algorithm ours (PI)).
    // 100 Hz at 44100 Hz = period 441 samples.
    const std::int64_t n = 44100;
    const auto saw = makeSaw(441, n);

    const S950Engine engine;
    S950Params params;
    params.material = Material::Pol2;
    params.dTime = 1000; // ignored: AUTO-D overrides the set D-TIME
    params.autoD = true;
    params.timeFactorPct = 200.0;
    params.sampleRate = kSampleRate;
    const auto result = engine.render(saw.channel(0), params);

    REQUIRE(result.effectiveCycleLen >= 440);
    REQUIRE(result.effectiveCycleLen <= 442);

    const CyclicEngine cyclic;
    REQUIRE(static_cast<std::int64_t>(result.out.numFrames())
            == cyclic.expectedOutputLength(n, result.effectiveCycleLen, 200.0,
                                           HopMode::Classic));
}

TEST_CASE("s950: deterministic — re-render is bit-identical", "[s950]")
{
    // Covers the analysis-heavy path: AUTO-D detection plus MON1 per-grain
    // snapping must be a pure function of (input, params).
    const auto sine = makeSine(220.0, 44100);

    const S950Engine engine;
    S950Params params;
    params.material = Material::Mon1;
    params.autoD = true;
    params.timeFactorPct = 300.0;
    params.sampleRate = kSampleRate;

    const auto first = engine.render(sine.channel(0), params);
    const auto second = engine.render(sine.channel(0), params);

    REQUIRE(first.effectiveCycleLen == second.effectiveCycleLen);
    REQUIRE(first.effectiveTimeFactorPct == second.effectiveTimeFactorPct);
    requireBitIdentical(first.out.channel(0), second.out.channel(0));
}

TEST_CASE("s950: D-TIME maps to cycle length in ONE clamped function", "[s950]")
{
    // The (PI, deviation-until-calibrated) D-TIME mapping (dsp-engine.md §5)
    // lives in a single function so the hardware-capture calibration is a
    // one-place change. 20–2000 is the CYCLE-LENGTH/D-TIME range (§2).
    REQUIRE(S950Engine::mapDTimeToCycleLen(1000) == 1000);
    REQUIRE(S950Engine::mapDTimeToCycleLen(5) == 20);
    REQUIRE(S950Engine::mapDTimeToCycleLen(99999) == 2000);
}

TEST_CASE("s950: PI constants live in one named-constants block", "[s950]")
{
    // QA retuning hook (testing-strategy.md §7): every (PI) value the QA
    // fleet may revise is a named constant in S950Engine.h, pinned here so a
    // retune is a deliberate, reviewed change.
    STATIC_REQUIRE(S950Engine::kTimeFactorMaxPct == 999); // [MAN §2] — not PI
    STATIC_REQUIRE(S950Engine::kDTimeMinSamples == 20);
    STATIC_REQUIRE(S950Engine::kDTimeMaxSamples == 2000);
    STATIC_REQUIRE(S950Engine::kMon1SearchFraction == 0.5);
    STATIC_REQUIRE(S950Engine::kMon1PeakThreshold == 0.3f);
    STATIC_REQUIRE(S950Engine::kMon1WindowCycles == 4);
}
