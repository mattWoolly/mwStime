// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// S950Engine — dsp-engine.md §5. POL2 (fixed C) delegates to CyclicEngine
// verbatim; MON1 runs the same CLASSIC two-grain scheduler with C re-snapped
// at every grain launch (per-grain pitch-period snap, §5 (PI)). The MON1 loop
// mirrors CyclicEngine.cpp's CLASSIC arithmetic exactly — the steady-tone
// degenerate case (constant snap) is pinned bit-exact against CyclicEngine in
// test_s950.cpp, so any drift between the two loops fails the suite.

#include "mws/stretch/S950Engine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "mws/core/AutoCorr.h"
#include "mws/stretch/AutoCycle.h"

namespace mws::stretch {
namespace {

// ---------------------------------------------------------------------------
// CLASSIC arithmetic mirrored from CyclicEngine.cpp (dsp-engine.md §3.2;
// OpenMPT [AKZ §4.1]). Kept textually identical so the constant-snap MON1
// render stays bit-exact with CyclicEngine (anti-drift pin in test_s950.cpp).
// ---------------------------------------------------------------------------

/// Edge rule (§3.4): reads past N-1 return 0 (no wrap).
inline float readSample(core::ConstAudioView src, std::int64_t index) noexcept
{
    if (index < 0 || index >= static_cast<std::int64_t>(src.size()))
        return 0.0f;
    return src[static_cast<std::size_t>(index)];
}

/// float32 <-> Q31 integer lattice for the crossfade mix (see CyclicEngine.cpp).
inline std::int64_t toQ31(float x) noexcept
{
    return std::llround(std::ldexp(static_cast<double>(x), 31));
}

inline float fromQ31(std::int64_t v) noexcept
{
    return static_cast<float>(std::ldexp(static_cast<double>(v), -31));
}

/// Rounded integer crossfade lerp, 15-bit fade fraction (dsp-engine.md §3.2).
inline float mixQ31(float sA, float sB, std::int64_t fade15) noexcept
{
    const std::int64_t a = toQ31(sA);
    const std::int64_t b = toQ31(sB);
    return fromQ31(a + (b - a) * fade15 / 32768);
}

// ---------------------------------------------------------------------------
// Per-grain MON1 schedule (dsp-engine.md §5). Each grain carries its OWN
// schedule constants, derived from its snapped C exactly the way
// CyclicEngine::makeClassicSchedule derives them from the fixed C.
// ---------------------------------------------------------------------------
struct GrainSpec
{
    std::int64_t off = 0;     ///< integer input start offset
    std::int64_t c = 0;       ///< snapped cycle length, clamped to N [AKZ §2.1]
    std::int64_t ovStart = 0; ///< grain phase where the crossfade begins
    std::int64_t fadeLen = 0; ///< c - ovStart (>= 1 guard for the division)
    std::int64_t hopIn = 0;   ///< integer input advance launching the NEXT grain
};

GrainSpec makeGrainSpec(std::int64_t off, int snappedC, std::int64_t n,
                        std::int64_t tPct, const CyclicEngine::SpliceCal& cal)
{
    GrainSpec g;
    g.off = off;
    g.c = std::max<std::int64_t>(1, snappedC);
    g.c = std::min(g.c, std::max<std::int64_t>(1, n)); // clamp C to N (§3.4)
    g.ovStart = std::llround(static_cast<double>(g.c)
                             * (1.0 - static_cast<double>(cal.overlapF)));
    g.ovStart = std::clamp<std::int64_t>(g.ovStart, 1, g.c);
    g.fadeLen = std::max<std::int64_t>(1, g.c - g.ovStart);

    // hop_in = round(hop_out / T), hop_out = ovStart — integer arithmetic
    // identical to CyclicEngine's CLASSIC schedule (§3.2).
    switch (cal.rounding)
    {
        case HopRounding::RoundNearest:
            g.hopIn = (2 * g.ovStart * 100 + tPct) / (2 * tPct);
            break;
    }
    g.hopIn = std::max<std::int64_t>(1, g.hopIn);
    return g;
}

/// The MON1 per-grain snap (dsp-engine.md §5 (PI, deviation-until-calibrated)):
/// AutoCorr::bestLagNear around the SET C, over a short window of source
/// audio starting at the grain's input offset. No confident detection (short
/// window near the input end, silence, noise) keeps the previous grain's C —
/// steady fallback, no snap thrash.
int snapMon1(core::ConstAudioView src, std::int64_t off, int setC, int prevC)
{
    const auto n = static_cast<std::int64_t>(src.size());
    if (off < 0 || off >= n)
        return prevC;

    const std::int64_t window = std::min<std::int64_t>(
        n - off, static_cast<std::int64_t>(S950Engine::kMon1WindowCycles) * setC);
    const core::ConstAudioView zone{ src.data() + off,
                                     static_cast<std::size_t>(window) };

    const auto lag = core::AutoCorr::bestLagNear(zone, setC,
                                                 S950Engine::kMon1SearchFraction,
                                                 S950Engine::kMon1PeakThreshold);
    if (!lag)
        return prevC;

    // A snapped period is still a cycle length: keep it inside the D-TIME/C
    // parameter range (dsp-engine.md §2/§5).
    return std::clamp(*lag, S950Engine::kDTimeMinSamples,
                      S950Engine::kDTimeMaxSamples);
}

/// MON1 CLASSIC render: the §3.4 reference loop with per-grain schedule
/// constants. With a constant snap this emits exactly CyclicEngine's CLASSIC
/// sample sequence (bit-exact, tested).
S950Result renderMon1Classic(core::ConstAudioView src, int setC, std::int64_t tPct,
                             const CyclicEngine::SpliceCal& cal)
{
    const auto n = static_cast<std::int64_t>(src.size());

    S950Result result;
    result.effectiveTimeFactorPct = static_cast<double>(tPct);
    if (n <= 0)
    {
        result.effectiveCycleLen = 0;
        return result;
    }

    int prevSnap = snapMon1(src, 0, setC, /*prevC=*/setC);

    struct ActiveGrain
    {
        GrainSpec spec;
        std::int64_t pos = 0;
        bool active = false;
    };

    ActiveGrain a{ makeGrainSpec(0, prevSnap, n, tPct, cal), 0, true };
    ActiveGrain b;
    result.effectiveCycleLen = static_cast<int>(a.spec.c);

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(
        std::max<std::int64_t>(0, (n / a.spec.hopIn + 1) * a.spec.ovStart + a.spec.c)));

    // --- dsp-engine.md §3.4 reference loop, per-grain constants ----------
    while (true)
    {
        const float sA = readSample(src, a.spec.off + a.pos); // verbatim read

        float outSample = sA;
        if (b.active)
        {
            // Linear complementary fade over the last F of the PLAYING grain
            // (§3.3), 15-bit integer fraction (§3.2) — grain-A geometry.
            switch (cal.shape)
            {
                case FadeShape::Linear:
                    break;
            }
            const std::int64_t fade15 = ((a.pos - a.spec.ovStart) << 15)
                                        / a.spec.fadeLen;
            const float sB = readSample(src, b.spec.off + b.pos);
            outSample = (fade15 == 0) ? sA : mixQ31(sA, sB, fade15);
        }
        out.push_back(outSample);

        ++a.pos;
        if (b.active)
            ++b.pos;

        if (!b.active && a.pos >= a.spec.ovStart)
        {
            // Per-grain snap at launch (dsp-engine.md §5 MON1 row): the new
            // grain's C comes from the source content at ITS input offset.
            const std::int64_t bOff = a.spec.off + a.spec.hopIn;
            prevSnap = snapMon1(src, bOff, setC, prevSnap);
            b = ActiveGrain{ makeGrainSpec(bOff, prevSnap, n, tPct, cal), 0, true };
        }

        if (a.pos >= a.spec.c)
        {
            a = b;
            b = ActiveGrain{};
            // Loop exit (§3.4): the incoming grain cannot play a complete
            // cycle — the output length is exactly the schedule-derived value
            // (for a constant snap, the §3.4 formula at the snapped C).
            if (!a.active || a.spec.off + a.spec.c > n)
                break;
        }

        // §3.4 literal guards: ran off the end of input.
        if (a.spec.off >= n || a.spec.off + a.pos >= n)
            break;
    }

    result.out = core::AudioBuffer(1, out.size());
    auto view = result.out.channel(0);
    std::copy(out.begin(), out.end(), view.begin());
    return result;
}

} // namespace

S950Result S950Engine::render(core::ConstAudioView monoSrc,
                              const S950Params& params) const
{
    const auto n = static_cast<std::int64_t>(monoSrc.size());

    // STRETCH %: engine-clamped to 999 ("up to 999%" [MAN §2]; dsp-engine.md
    // §2 timeFactor row — the S950 clamps the superset range at the engine).
    const double clampedPct =
        std::min(params.timeFactorPct, static_cast<double>(kTimeFactorMaxPct));

    // Integer % in CLASSIC [AKZ §2.1] (CyclicEngine re-derives the same value;
    // REVISED keeps the 0.01 step, dsp-engine.md §2).
    const bool classic = params.hopMode == engine::HopMode::Classic;
    const double effectivePct =
        classic ? static_cast<double>(std::max<std::int64_t>(1, std::llround(clampedPct)))
                : std::max(1.0, clampedPct);

    // AUTO-D: run the task-014 detector and use the result as C (dsp-engine.md
    // §5); otherwise C = mapDTimeToCycleLen(D-TIME) — the ONE (PI) mapping.
    const int setC = params.autoD
                         ? AutoCycle::detectCycleLen(monoSrc, params.sampleRate)
                         : mapDTimeToCycleLen(params.dTime);

    if (params.material == engine::Material::Mon1 && classic)
        return renderMon1Classic(monoSrc, setC,
                                 static_cast<std::int64_t>(effectivePct),
                                 cyclic_.spliceCal());

    // MON1 + REVISED **(PI simplification)**: per-grain snapping is defined on
    // the CLASSIC integer schedule (the hardware-faithful mode, dsp-engine.md
    // §5); under REVISED timing the snap is taken once, at the start of the
    // material, and the render delegates with that fixed C.
    const int renderC = params.material == engine::Material::Mon1
                            ? snapMon1(monoSrc, 0, setC, setC)
                            : setC; // POL2: fixed C exactly as set (§5)

    S950Result result;
    result.out = cyclic_.render(monoSrc, renderC, effectivePct, params.hopMode);
    result.effectiveTimeFactorPct = effectivePct;
    // Report the schedule's C: clamped to the input length like the engine
    // does internally [AKZ §2.1] (§3.4 "clamp C to N").
    result.effectiveCycleLen = static_cast<int>(std::min<std::int64_t>(
        std::max<std::int64_t>(1, renderC), std::max<std::int64_t>(1, n)));
    if (n <= 0)
        result.effectiveCycleLen = 0;
    return result;
}

S950Result S950Engine::render(const core::AudioBuffer& monoSrc,
                              const S950Params& params) const
{
    // The S950 is a mono machine [MAN §2]; stereo summing happens upstream
    // (OfflineRenderer/CharacterChain, dsp-engine.md §5) — never here.
    assert(monoSrc.numChannels() == 1 && "S950Engine is mono-only [MAN §2]");
    return render(monoSrc.channel(0), params);
}

} // namespace mws::stretch
