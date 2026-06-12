// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CyclicEngine — the [AKZ §4.2] two-grain overlap scheduler, implementing the
// dsp-engine.md §3.4 reference pseudocode. CLASSIC uses the float-free integer
// stretch path of §3.2; REVISED (task 011) uses a fractional input hop with
// 2-point linear-interpolation reads for sample-exact timing (§3.2 REVISED
// line, §3.4 REVISED note).

#include "mws/stretch/CyclicEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace mws::stretch {
namespace {

// ---------------------------------------------------------------------------
// CLASSIC schedule constants (dsp-engine.md §3.1–§3.2).
//
// The calibration-to-integer derivation below (ovStart from the float
// overlapF) is the ONLY place a floating-point value enters; it happens once
// per render, producing integer constants. The per-sample stretch path is
// integer/fixed-point only (§3.2 bit-exactness property).
// ---------------------------------------------------------------------------
struct ClassicSchedule
{
    std::int64_t n = 0;       ///< input length N
    std::int64_t c = 0;       ///< cycle length C, clamped to N [AKZ §2.1]
    std::int64_t ovStart = 0; ///< grain phase where the crossfade begins
    std::int64_t fadeLen = 0; ///< C - ovStart (>= 1 guard for the division)
    std::int64_t hopOut = 0;  ///< output spacing of grain launches = ovStart
    std::int64_t hopIn = 0;   ///< integer input advance per grain, >= 1
};

ClassicSchedule makeClassicSchedule(std::int64_t numInputFrames, int cycleLenSamples,
                                    double timeFactorPct,
                                    const CyclicEngine::SpliceCal& cal)
{
    ClassicSchedule s;
    s.n = std::max<std::int64_t>(0, numInputFrames);

    // Engine arithmetic safety only — range clamping is ModelSpec's job.
    s.c = std::max<std::int64_t>(1, cycleLenSamples);
    s.c = std::min(s.c, std::max<std::int64_t>(1, s.n)); // clamp C to N (§3.4)

    // ovStart = C * (1 - F): the one setup-time float -> integer conversion
    // (exact, deterministic IEEE double ops; never on the per-sample path).
    s.ovStart = std::llround(static_cast<double>(s.c)
                             * (1.0 - static_cast<double>(cal.overlapF)));
    s.ovStart = std::clamp<std::int64_t>(s.ovStart, 1, s.c);
    s.fadeLen = std::max<std::int64_t>(1, s.c - s.ovStart);
    s.hopOut = s.ovStart;

    // CLASSIC: timeFactor coerced to integer percent [AKZ §2.1].
    const auto tPct = std::max<std::int64_t>(1, std::llround(timeFactorPct));

    // hop_in = round(hop_out / T) = round(hop_out * 100 / tPct), computed in
    // pure integer arithmetic. RoundNearest = round half up for the positive
    // operands here: floor((2a + b) / 2b) == round(a / b).
    switch (cal.rounding)
    {
        case HopRounding::RoundNearest:
            s.hopIn = (2 * s.hopOut * 100 + tPct) / (2 * tPct);
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
    const std::int64_t grains = (s.n - s.c) / s.hopIn + 1;
    return (grains - 1) * s.hopOut + s.c;
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
    std::int64_t n = 0;       ///< input length N
    std::int64_t c = 0;       ///< cycle length C, clamped to N [AKZ §2.1]
    std::int64_t ovStart = 0; ///< grain phase where the crossfade begins
    std::int64_t fadeLen = 0; ///< C - ovStart (>= 1 guard for the division)
    std::int64_t hopOut = 0;  ///< output spacing of grain launches = ovStart
    double hopIn = 0.0;       ///< fractional input advance per grain = hop_out/T
    std::int64_t outLen = 0;  ///< sample-exact length round(N·T)
};

RevisedSchedule makeRevisedSchedule(std::int64_t numInputFrames, int cycleLenSamples,
                                    double timeFactorPct,
                                    const CyclicEngine::SpliceCal& cal)
{
    RevisedSchedule s;
    s.n = std::max<std::int64_t>(0, numInputFrames);

    // Engine arithmetic safety only — range clamping is ModelSpec's job.
    s.c = std::max<std::int64_t>(1, cycleLenSamples);
    s.c = std::min(s.c, std::max<std::int64_t>(1, s.n)); // clamp C to N (§3.4)

    s.ovStart = std::llround(static_cast<double>(s.c)
                             * (1.0 - static_cast<double>(cal.overlapF)));
    s.ovStart = std::clamp<std::int64_t>(s.ovStart, 1, s.c);
    s.fadeLen = std::max<std::int64_t>(1, s.c - s.ovStart);
    s.hopOut = s.ovStart;

    // REVISED keeps the 0.01% step — no integer coercion (dsp-engine.md §2).
    // Engine safety floor on T mirrors CLASSIC's tPct >= 1 (avoids /0).
    const double t = std::max(0.01, timeFactorPct / 100.0);
    s.hopIn = static_cast<double>(s.hopOut) / t;

    // Sample-exact length: round(N·T) (akaizer-analysis.md §2.2). Uses the
    // un-floored factor so the rendered length matches the user's exact ratio.
    s.outLen = (s.n <= 0) ? 0
                          : std::llround(static_cast<double>(s.n) * timeFactorPct / 100.0);
    s.outLen = std::max<std::int64_t>(0, s.outLen);
    return s;
}

// ---------------------------------------------------------------------------
// Float-free stretch-path arithmetic (dsp-engine.md §3.2; OpenMPT [AKZ §4.1]).
// ---------------------------------------------------------------------------

/// 32.32 fixed-point position step of one sample (OpenMPT SamplePosition
/// convention [AKZ §4.1]). In CLASSIC the fraction is always zero — grains
/// start at integer offsets and advance by exactly 1 — so stretch alone never
/// resamples; the representation is shared with REVISED (task 011).
constexpr std::int64_t kPosOne = std::int64_t{ 1 } << 32;

constexpr std::int64_t intPos(std::int64_t pos3232) noexcept
{
    return pos3232 >> 32;
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
    const float s1 = readSample(src, i0 + 1);
    return static_cast<float>(static_cast<double>(s0)
                              + (static_cast<double>(s1) - static_cast<double>(s0)) * frac);
}

/// float32 <-> Q31 integer lattice for the crossfade mix. std::ldexp is exact
/// exponent scaling (no rounding) and std::llround has pinned IEEE semantics,
/// so the conversions are deterministic on every platform; all *arithmetic*
/// between them is integer. Lattice headroom: |sample| < 2^16 (audio is
/// nominal +/-1 full scale; index-encoded test signals stay well inside).
inline std::int64_t toQ31(float x) noexcept
{
    return std::llround(std::ldexp(static_cast<double>(x), 31));
}

inline float fromQ31(std::int64_t v) noexcept
{
    return static_cast<float>(std::ldexp(static_cast<double>(v), -31));
}

/// Rounded integer crossfade lerp, 15-bit fade fraction (dsp-engine.md §3.2):
/// a + (b - a) * fade15 / 32768 with the OpenMPT truncating integer division
/// ([AKZ §4.1] sample-interpolation arithmetic). fade15 in [0, 32767].
inline float mixQ31(float sA, float sB, std::int64_t fade15) noexcept
{
    const std::int64_t a = toQ31(sA);
    const std::int64_t b = toQ31(sB);
    return fromQ31(a + (b - a) * fade15 / 32768);
}

struct Grain
{
    std::int64_t off = 0;     ///< integer input start offset
    std::int64_t pos3232 = 0; ///< 32.32 fixed-point phase within the grain
    bool active = false;
};

/// REVISED grain: a fractional start offset, an integer phase. The phase
/// advances by exactly one input sample per output sample (rate 1 — perfect
/// in-grain pitch), so every read shares the constant fractional offset of the
/// grain start (dsp-engine.md §3.4 REVISED note).
struct GrainR
{
    double off = 0.0;     ///< fractional input start offset
    std::int64_t pos = 0; ///< integer phase within the grain
    bool active = false;
};

/// REVISED render loop — the §3.4 reference scheduler with the fractional hop
/// and interpolated reads. The crossfade reuses the CLASSIC `mixQ31`/`fade15`
/// arithmetic so that an integral hop (zero-fraction reads) is bit-identical to
/// the CLASSIC render (degenerate-equivalence property). Unlike CLASSIC the
/// loop never breaks on input exhaustion: it runs to the sample-exact
/// `outLen = round(N·T)` length, with reads past the end returning 0.
core::AudioBuffer renderRevised(core::ConstAudioView src, const RevisedSchedule& s,
                                FadeShape shape)
{
    core::AudioBuffer outBuffer(1, static_cast<std::size_t>(s.outLen));
    if (s.outLen == 0)
        return outBuffer;
    core::AudioView out = outBuffer.channel(0);

    GrainR a{ /*off*/ 0.0, /*pos*/ 0, /*active*/ true };
    GrainR b;
    std::int64_t outIdx = 0;

    while (outIdx < s.outLen)
    {
        const float sA = readSampleInterp(src, a.off + static_cast<double>(a.pos));

        float outSample = sA;
        if (b.active)
        {
            switch (shape)
            {
                case FadeShape::Linear:
                    break;
            }
            const std::int64_t fade15 = ((a.pos - s.ovStart) << 15) / s.fadeLen;
            const float sB = readSampleInterp(src, b.off + static_cast<double>(b.pos));
            outSample = (fade15 == 0) ? sA : mixQ31(sA, sB, fade15);
        }
        out[static_cast<std::size_t>(outIdx)] = outSample;
        ++outIdx;

        ++a.pos;
        if (b.active)
            ++b.pos;

        if (!b.active && a.pos >= s.ovStart)
        {
            b.off = a.off + s.hopIn; // REVISED: fractional grain start
            b.pos = 0;
            b.active = true;
        }

        if (a.pos >= s.c)
        {
            a = b;
            b = GrainR{};
            // No input-exhaustion break (contrast with CLASSIC): REVISED runs
            // to the exact round(N·T) length. The relaunch above keeps `a`
            // active every cycle, so the loop always fills outLen.
        }
    }

    return outBuffer;
}

} // namespace

core::AudioBuffer CyclicEngine::render(core::ConstAudioView src, int cycleLenSamples,
                                       double timeFactorPct,
                                       engine::HopMode mode) const
{
    if (mode == engine::HopMode::Revised)
        return renderRevised(
            src,
            makeRevisedSchedule(static_cast<std::int64_t>(src.size()), cycleLenSamples,
                                timeFactorPct, cal_),
            cal_.shape);

    const ClassicSchedule s =
        makeClassicSchedule(static_cast<std::int64_t>(src.size()), cycleLenSamples,
                            timeFactorPct, cal_);
    const std::int64_t outLen = classicOutputLength(s);

    core::AudioBuffer outBuffer(1, static_cast<std::size_t>(outLen));
    if (outLen == 0)
        return outBuffer;
    core::AudioView out = outBuffer.channel(0);

    // --- dsp-engine.md §3.4 reference loop -------------------------------
    Grain a{ /*off*/ 0, /*pos*/ 0, /*active*/ true };
    Grain b;
    std::int64_t outIdx = 0;

    while (outIdx < outLen)
    {
        const std::int64_t aPos = intPos(a.pos3232);
        const float sA = readSample(src, a.off + aPos); // verbatim read

        float outSample = sA;
        if (b.active)
        {
            // Linear complementary fade over the last F of the grain (§3.3),
            // as a 15-bit integer fraction (§3.2). SpliceCal::shape has only
            // the authentic Linear value at v1.
            switch (cal_.shape)
            {
                case FadeShape::Linear:
                    break;
            }
            const std::int64_t fade15 = ((aPos - s.ovStart) << 15) / s.fadeLen;
            const float sB = readSample(src, b.off + intPos(b.pos3232));
            outSample = (fade15 == 0) ? sA : mixQ31(sA, sB, fade15);
        }
        out[static_cast<std::size_t>(outIdx)] = outSample;
        ++outIdx;

        a.pos3232 += kPosOne;
        if (b.active)
            b.pos3232 += kPosOne;

        if (!b.active && intPos(a.pos3232) >= s.ovStart)
        {
            b.off = a.off + s.hopIn; // CLASSIC: integer grain start
            b.pos3232 = 0;
            b.active = true;
        }

        if (intPos(a.pos3232) >= s.c)
        {
            a = b;
            b = Grain{};
            // Loop exit (§3.4): the incoming grain cannot play a complete
            // cycle — the render ends after the last complete grain, so the
            // output length is exactly the §3.4 schedule formula
            // (G - 1) * hop_out + C (testing-strategy.md §3.2 exact equality;
            // a partial-tail overhang would contradict that contract).
            if (!a.active || a.off + s.c > s.n)
                break;
        }

        // §3.4 literal guards: ran off the end of input.
        if (a.off >= s.n || a.off + intPos(a.pos3232) >= s.n)
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
