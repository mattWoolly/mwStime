// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CyclicEngine — the [AKZ §4.2] two-grain overlap scheduler, implementing the
// dsp-engine.md §3.4 reference pseudocode. CLASSIC uses the float-free integer
// stretch path of §3.2; REVISED (task 011) uses a fractional input hop with
// 2-point linear-interpolation reads for sample-exact timing (§3.2 REVISED
// line, §3.4 REVISED note).
//
// The grain loop itself lives in mws/stretch/detail/TwoGrainScheduler.h — ONE
// scheduler implementation shared with the streaming FX front-end
// (RealtimeStretcher, task 022; ADR-006 option C "two thin front-ends over
// ONE grain scheduler"). This file owns only the offline concerns: schedule
// constants, flat-buffer reads, and the input-exhaustion termination rules.

#include "mws/stretch/CyclicEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "mws/stretch/detail/TwoGrainScheduler.h"

namespace mws::stretch {
namespace {

using detail::GrainGeometry;

// ---------------------------------------------------------------------------
// CLASSIC schedule constants (dsp-engine.md §3.1–§3.2).
//
// The calibration-to-integer derivation (GrainGeometry::fromCycle) is the
// ONLY place a floating-point value enters; it happens once per render,
// producing integer constants. The per-sample stretch path is
// integer/fixed-point only (§3.2 bit-exactness property).
// ---------------------------------------------------------------------------
struct ClassicSchedule
{
    std::int64_t n = 0;   ///< input length N
    GrainGeometry geom{}; ///< C (clamped to N [AKZ §2.1]), ovStart, fade, hop_out
    std::int64_t hopIn = 0; ///< integer input advance per grain, >= 1
};

ClassicSchedule makeClassicSchedule(std::int64_t numInputFrames, int cycleLenSamples,
                                    double timeFactorPct,
                                    const CyclicEngine::SpliceCal& cal)
{
    ClassicSchedule s;
    s.n = std::max<std::int64_t>(0, numInputFrames);

    // Engine arithmetic safety only — range clamping is ModelSpec's job.
    std::int64_t c = std::max<std::int64_t>(1, cycleLenSamples);
    c = std::min(c, std::max<std::int64_t>(1, s.n)); // clamp C to N (§3.4)
    s.geom = GrainGeometry::fromCycle(c, cal.overlapF);

    // CLASSIC: timeFactor coerced to integer percent [AKZ §2.1].
    const auto tPct = std::max<std::int64_t>(1, std::llround(timeFactorPct));

    // hop_in = round(hop_out / T) = round(hop_out * 100 / tPct), computed in
    // pure integer arithmetic. RoundNearest = round half up for the positive
    // operands here: floor((2a + b) / 2b) == round(a / b).
    switch (cal.rounding)
    {
        case HopRounding::RoundNearest:
            s.hopIn = (2 * s.geom.hopOut * 100 + tPct) / (2 * tPct);
            break;
    }
    s.hopIn = std::max<std::int64_t>(1, s.hopIn); // require hop_in >= 1 (§3.4)
    return s;
}

/// Schedule-derived output length (dsp-engine.md §3.4):
/// G = floor((N - C) / hop_in) + 1 complete grains; (G - 1) * hop_out + C.
std::int64_t classicOutputLength(const ClassicSchedule& s)
{
    if (s.n <= 0)
        return 0;
    const std::int64_t grains = (s.n - s.geom.c) / s.hopIn + 1;
    return (grains - 1) * s.geom.hopOut + s.geom.c;
}

// ---------------------------------------------------------------------------
// REVISED schedule constants (dsp-engine.md §3.2 REVISED line, §3.4 note).
//
// REVISED shares the §3.1 cycle/overlap geometry with CLASSIC but advances the
// input read by a FRACTIONAL hop (`hop_in = hop_out / T`, no integer-% coercion
// and no rounding). Because the read advances exactly in step with the output
// inside each grain, timing is sample-exact: the schedule consumes the whole
// input over an output of length round(N·T) (akaizer-analysis.md §2.2 "perfect
// timing"). The price is that the grain start is fractional, so reads are
// 2-point linear interpolations (§3.4) — the sole source of the slight drift.
// ---------------------------------------------------------------------------
struct RevisedSchedule
{
    std::int64_t n = 0;      ///< input length N
    GrainGeometry geom{};    ///< C (clamped to N [AKZ §2.1]), ovStart, fade, hop_out
    double hopIn = 0.0;      ///< fractional input advance per grain = hop_out/T
    std::int64_t outLen = 0; ///< sample-exact length round(N·T)
};

RevisedSchedule makeRevisedSchedule(std::int64_t numInputFrames, int cycleLenSamples,
                                    double timeFactorPct,
                                    const CyclicEngine::SpliceCal& cal)
{
    RevisedSchedule s;
    s.n = std::max<std::int64_t>(0, numInputFrames);

    // Engine arithmetic safety only — range clamping is ModelSpec's job.
    std::int64_t c = std::max<std::int64_t>(1, cycleLenSamples);
    c = std::min(c, std::max<std::int64_t>(1, s.n)); // clamp C to N (§3.4)
    s.geom = GrainGeometry::fromCycle(c, cal.overlapF);

    // REVISED keeps the 0.01% step — no integer coercion (dsp-engine.md §2).
    // Engine safety floor on T mirrors CLASSIC's tPct >= 1 (avoids /0).
    const double t = std::max(0.01, timeFactorPct / 100.0);
    s.hopIn = static_cast<double>(s.geom.hopOut) / t;

    // Sample-exact length: round(N·T) (akaizer-analysis.md §2.2). Uses the
    // un-floored factor so the rendered length matches the user's exact ratio.
    s.outLen = (s.n <= 0) ? 0
                          : std::llround(static_cast<double>(s.n) * timeFactorPct / 100.0);
    s.outLen = std::max<std::int64_t>(0, s.outLen);
    return s;
}

/// Edge rule (§3.4): reads past N-1 return 0 (no wrap).
inline float readSample(core::ConstAudioView src, std::int64_t index) noexcept
{
    if (index < 0 || index >= static_cast<std::int64_t>(src.size()))
        return 0.0f;
    return src[static_cast<std::size_t>(index)];
}

/// REVISED-only fractional read (dsp-engine.md §3.4 REVISED note): 2-point
/// linear interpolation between the two integer source samples bracketing
/// `pos`. The fractional part comes solely from the grain's fractional start
/// offset (§3.2) — this is the sole source of REVISED's slight pitch drift. A
/// zero fraction short-circuits to the verbatim sample so an integral hop is
/// bit-identical to CLASSIC (the degenerate-equivalence property). Edge rule:
/// reads past the end return 0 (shared with readSample).
inline float readSampleInterp(core::ConstAudioView src, double pos) noexcept
{
    const double floorPos = std::floor(pos);
    const auto i0 = static_cast<std::int64_t>(floorPos);
    const double frac = pos - floorPos;
    const float s0 = readSample(src, i0);
    if (frac == 0.0)
        return s0;
    return detail::lerpSamples(s0, readSample(src, i0 + 1), frac);
}

/// REVISED render loop — the §3.4 reference scheduler with the fractional hop
/// and interpolated reads. The crossfade reuses the CLASSIC `mixQ31`/`fade15`
/// arithmetic so that an integral hop (zero-fraction reads) is bit-identical to
/// the CLASSIC render (degenerate-equivalence property). Unlike CLASSIC the
/// loop never breaks on input exhaustion: it runs to the sample-exact
/// `outLen = round(N·T)` length, with reads past the end returning 0.
core::AudioBuffer renderRevised(core::ConstAudioView src, const RevisedSchedule& s,
                                const GrainLaunchObserver* onGrainLaunch)
{
    core::AudioBuffer outBuffer(1, static_cast<std::size_t>(s.outLen));
    if (s.outLen == 0)
        return outBuffer;
    core::AudioView out = outBuffer.channel(0);

    detail::TwoGrainScheduler<detail::RevisedGrainPolicy> sched;
    sched.reset(s.geom, 0.0);
    std::int64_t outIdx = 0;

    // Test-observation hook (plan/backlog/012): the initial grain launches at
    // output index 0, input offset 0. Observation never alters the render.
    if (onGrainLaunch != nullptr && *onGrainLaunch)
        (*onGrainLaunch)(GrainLaunch{ 0, 0.0 });

    while (outIdx < s.outLen)
    {
        const auto taps = sched.taps();
        const float sA = readSampleInterp(src, taps.posA);
        float outSample = sA;
        if (taps.hasB)
            outSample = detail::crossfadeOutput(
                sA, readSampleInterp(src, taps.posB), taps.fade15);
        out[static_cast<std::size_t>(outIdx)] = outSample;
        ++outIdx;

        // B's first sample plays at the CURRENT outIdx (next iteration). No
        // input-exhaustion break (contrast with CLASSIC): REVISED runs to the
        // exact round(N·T) length; the relaunch keeps grain A active every
        // cycle, so the loop always fills outLen.
        (void) sched.advance([&](double aOff) { return aOff + s.hopIn; },
                             [&](double off)
                             {
                                 if (onGrainLaunch != nullptr && *onGrainLaunch)
                                     (*onGrainLaunch)(GrainLaunch{ outIdx, off });
                             });
    }

    return outBuffer;
}

} // namespace

core::AudioBuffer CyclicEngine::render(core::ConstAudioView src, int cycleLenSamples,
                                       double timeFactorPct, engine::HopMode mode,
                                       const GrainLaunchObserver* onGrainLaunch) const
{
    if (mode == engine::HopMode::Revised)
        return renderRevised(
            src,
            makeRevisedSchedule(static_cast<std::int64_t>(src.size()), cycleLenSamples,
                                timeFactorPct, cal_),
            onGrainLaunch);

    const ClassicSchedule s =
        makeClassicSchedule(static_cast<std::int64_t>(src.size()), cycleLenSamples,
                            timeFactorPct, cal_);
    const std::int64_t outLen = classicOutputLength(s);

    core::AudioBuffer outBuffer(1, static_cast<std::size_t>(outLen));
    if (outLen == 0)
        return outBuffer;
    core::AudioView out = outBuffer.channel(0);

    // --- dsp-engine.md §3.4 reference loop (shared scheduler) -------------
    detail::TwoGrainScheduler<detail::ClassicGrainPolicy> sched;
    sched.reset(s.geom, 0);
    std::int64_t outIdx = 0;

    // Test-observation hook (plan/backlog/012): the initial grain launches at
    // output index 0, input offset 0. Observation never alters the render.
    if (onGrainLaunch != nullptr && *onGrainLaunch)
        (*onGrainLaunch)(GrainLaunch{ 0, 0.0 });

    while (outIdx < outLen)
    {
        const auto taps = sched.taps();
        const float sA = readSample(src, taps.posA); // verbatim read
        float outSample = sA;
        if (taps.hasB)
            outSample = detail::crossfadeOutput(sA, readSample(src, taps.posB),
                                                taps.fade15);
        out[static_cast<std::size_t>(outIdx)] = outSample;
        ++outIdx;

        // B's first sample plays at the CURRENT outIdx (next iteration).
        const bool swapped = sched.advance(
            [&](std::int64_t aOff) { return aOff + s.hopIn; },
            [&](std::int64_t off)
            {
                if (onGrainLaunch != nullptr && *onGrainLaunch)
                    (*onGrainLaunch)(
                        GrainLaunch{ outIdx, static_cast<double>(off) });
            });

        const auto& a = sched.grainA();

        // Loop exit (§3.4): the incoming grain cannot play a complete cycle —
        // the render ends after the last complete grain, so the output length
        // is exactly the §3.4 schedule formula (G - 1) * hop_out + C
        // (testing-strategy.md §3.2 exact equality; a partial-tail overhang
        // would contradict that contract).
        if (swapped && (!a.active || a.off + s.geom.c > s.n))
            break;

        // §3.4 literal guards: ran off the end of input.
        if (a.off >= s.n
            || a.off + detail::ClassicGrainPolicy::phase(a) >= s.n)
            break;
    }

    assert(outIdx == outLen && "render loop must realize the schedule length");
    return outBuffer;
}

std::int64_t CyclicEngine::expectedOutputLength(std::int64_t numInputFrames,
                                                int cycleLenSamples,
                                                double timeFactorPct,
                                                engine::HopMode mode) const
{
    if (mode == engine::HopMode::Revised)
        return makeRevisedSchedule(numInputFrames, cycleLenSamples, timeFactorPct, cal_)
            .outLen;

    return classicOutputLength(
        makeClassicSchedule(numInputFrames, cycleLenSamples, timeFactorPct, cal_));
}

} // namespace mws::stretch
