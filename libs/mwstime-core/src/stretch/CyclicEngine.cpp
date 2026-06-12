// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CyclicEngine CLASSIC — the [AKZ §4.2] two-grain overlap scheduler,
// implementing the dsp-engine.md §3.4 reference pseudocode with the
// float-free integer stretch path of §3.2.

#include "mws/stretch/CyclicEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace mws::stretch {
namespace {

[[noreturn]] void throwRevisedUnimplemented()
{
    throw std::logic_error(
        "mws::stretch::CyclicEngine: REVISED hop mode is not implemented yet "
        "(plan/backlog/011-cyclic-engine-revised.md)");
}

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

} // namespace

core::AudioBuffer CyclicEngine::render(core::ConstAudioView src, int cycleLenSamples,
                                       double timeFactorPct,
                                       engine::HopMode mode) const
{
    if (mode == engine::HopMode::Revised)
        throwRevisedUnimplemented();

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
        throwRevisedUnimplemented();

    return classicOutputLength(
        makeClassicSchedule(numInputFrames, cycleLenSamples, timeFactorPct, cal_));
}

} // namespace mws::stretch
