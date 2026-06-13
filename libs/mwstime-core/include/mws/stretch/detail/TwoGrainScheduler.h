// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// TwoGrainScheduler — THE single [AKZ §4.2] two-grain overlap scheduler
// (docs/design/dsp-engine.md §3.4 reference pseudocode), factored out of
// CyclicEngine so the offline renderer and the streaming FX front-end share
// ONE grain-loop implementation (plan/decisions/006-fx-vs-sample-mode.md
// option C "two thin front-ends over ONE grain scheduler"; plan/backlog/022
// acceptance criterion "no fork").
//
// The scheduler owns the per-sample grain mechanics — phase advance, grain-B
// launch at the overlap start, the A<-B swap at cycle end, and the 15-bit
// linear fade fraction (§3.2/§3.3). It does NOT own source access or
// termination policy: callers supply reads (flat buffer offline, history ring
// streaming) and decide when to stop (offline: input exhaustion; streaming:
// never). Two grain policies cover the §3.2 hop arithmetic:
//   - ClassicGrainPolicy: integer offsets, 32.32 fixed-point phase — the
//     float-free CLASSIC stretch path (cross-platform bit-exactness,
//     architecture.md §2);
//   - RevisedGrainPolicy: fractional (double) offsets, integer phase — the
//     REVISED fractional hop. With integer-valued offsets its reads degrade
//     to verbatim samples, which is what lets the streaming engine serve
//     CLASSIC through this policy bit-identically (RealtimeStretcher.h).

#pragma once

#include <cmath>
#include <cstdint>

namespace mws::stretch::detail {

// ---------------------------------------------------------------------------
// Float-free stretch-path arithmetic (dsp-engine.md §3.2; OpenMPT [AKZ §4.1]).
// ---------------------------------------------------------------------------

/// 32.32 fixed-point position step of one sample (OpenMPT SamplePosition
/// convention [AKZ §4.1]). In CLASSIC the fraction is always zero — grains
/// start at integer offsets and advance by exactly 1 — so stretch alone never
/// resamples.
inline constexpr std::int64_t kPosOne = std::int64_t{ 1 } << 32;

[[nodiscard]] constexpr std::int64_t intPos(std::int64_t pos3232) noexcept
{
    return pos3232 >> 32;
}

/// float32 <-> Q31 integer lattice for the crossfade mix. std::ldexp is exact
/// exponent scaling (no rounding) and std::llround has pinned IEEE semantics,
/// so the conversions are deterministic on every platform; all *arithmetic*
/// between them is integer. Lattice headroom: |sample| < 2^16 (audio is
/// nominal +/-1 full scale; index-encoded test signals stay well inside).
[[nodiscard]] inline std::int64_t toQ31(float x) noexcept
{
    return std::llround(std::ldexp(static_cast<double>(x), 31));
}

[[nodiscard]] inline float fromQ31(std::int64_t v) noexcept
{
    return static_cast<float>(std::ldexp(static_cast<double>(v), -31));
}

/// Rounded integer crossfade lerp, 15-bit fade fraction (dsp-engine.md §3.2):
/// a + (b - a) * fade15 / 32768 with the OpenMPT truncating integer division
/// ([AKZ §4.1] sample-interpolation arithmetic). fade15 in [0, 32767].
[[nodiscard]] inline float mixQ31(float sA, float sB, std::int64_t fade15) noexcept
{
    const std::int64_t a = toQ31(sA);
    const std::int64_t b = toQ31(sB);
    return fromQ31(a + (b - a) * fade15 / 32768);
}

/// The §3.4 emit rule shared by every front-end: grain A verbatim outside the
/// crossfade, the rounded integer lerp inside it (fade15 == 0 short-circuits
/// to A so the fade entry sample stays a verbatim copy — §3.2 property).
///
/// Equal taps short-circuit too: the source arithmetic `a + (b - a) * fade`
/// is exactly `a` when a == b (OpenMPT computes it on native int16 samples,
/// where the identity is free [AKZ §4.1]); the float -> Q31 adaptation must
/// not lose it to lattice rounding. This is what makes T = 100% — where both
/// grains read the SAME sample throughout the fade — a bit-exact pure delay
/// in FX mode (ADR-006 null contract).
[[nodiscard]] inline float crossfadeOutput(float sA, float sB,
                                           std::int64_t fade15) noexcept
{
    return (fade15 == 0 || sA == sB) ? sA : mixQ31(sA, sB, fade15);
}

/// REVISED/streaming 2-point linear interpolation between two bracketing
/// samples (dsp-engine.md §3.4 REVISED note), factored so the flat-buffer and
/// history-ring readers share the exact arithmetic (stream/offline
/// equivalence, testing-strategy.md §3.4b).
[[nodiscard]] inline float lerpSamples(float s0, float s1, double frac) noexcept
{
    return static_cast<float>(static_cast<double>(s0)
                              + (static_cast<double>(s1) - static_cast<double>(s0))
                                    * frac);
}

// ---------------------------------------------------------------------------
// Grain geometry (dsp-engine.md §3.1): the per-cycle integer constants. The
// calibration-to-integer derivation below (ovStart from the float overlapF)
// is the ONLY place a floating-point value enters the CLASSIC path; it runs
// once per schedule (offline) or per grain-boundary parameter change
// (streaming), never per sample.
// ---------------------------------------------------------------------------
struct GrainGeometry
{
    std::int64_t c = 0;       ///< cycle length C (model-rate samples)
    std::int64_t ovStart = 0; ///< grain phase where the crossfade begins
    std::int64_t fadeLen = 1; ///< C - ovStart (>= 1 guard for the division)
    std::int64_t hopOut = 0;  ///< output spacing of grain launches = ovStart

    /// ovStart = C * (1 - F), clamped to [1, C]; fadeLen = max(1, C - ovStart);
    /// hop_out = ovStart (§3.1). `cycleLen` must already carry the caller's
    /// clamps (ModelSpec range; offline clamp-to-input-length).
    [[nodiscard]] static GrainGeometry fromCycle(std::int64_t cycleLen,
                                                 float overlapF) noexcept
    {
        GrainGeometry g;
        g.c = cycleLen < 1 ? 1 : cycleLen;
        g.ovStart = std::llround(static_cast<double>(g.c)
                                 * (1.0 - static_cast<double>(overlapF)));
        if (g.ovStart < 1)
            g.ovStart = 1;
        if (g.ovStart > g.c)
            g.ovStart = g.c;
        g.fadeLen = g.c - g.ovStart;
        if (g.fadeLen < 1)
            g.fadeLen = 1;
        g.hopOut = g.ovStart;
        return g;
    }
};

// ---------------------------------------------------------------------------
// Grain policies — the §3.2 CLASSIC/REVISED split, as data + four operations.
// ---------------------------------------------------------------------------

/// CLASSIC (dsp-engine.md §3.2): integer input start offset, 32.32 fixed-point
/// phase. Read positions are always integer-valued — verbatim copies.
struct ClassicGrainPolicy
{
    using Offset = std::int64_t;

    struct Grain
    {
        std::int64_t off = 0;     ///< integer input start offset
        std::int64_t pos3232 = 0; ///< 32.32 fixed-point phase within the grain
        bool active = false;
    };

    [[nodiscard]] static std::int64_t phase(const Grain& g) noexcept
    {
        return intPos(g.pos3232);
    }

    static void advance(Grain& g) noexcept { g.pos3232 += kPosOne; }

    [[nodiscard]] static Grain launch(Offset off) noexcept
    {
        return { off, 0, true };
    }
};

/// REVISED (dsp-engine.md §3.2): fractional (double) start offset, integer
/// phase. The phase advances by exactly one input sample per output sample
/// (rate 1 — perfect in-grain pitch), so every read shares the constant
/// fractional offset of the grain start (§3.4 REVISED note).
struct RevisedGrainPolicy
{
    using Offset = double;

    struct Grain
    {
        double off = 0.0;     ///< fractional input start offset
        std::int64_t pos = 0; ///< integer phase within the grain
        bool active = false;
    };

    [[nodiscard]] static std::int64_t phase(const Grain& g) noexcept
    {
        return g.pos;
    }

    static void advance(Grain& g) noexcept { ++g.pos; }

    [[nodiscard]] static Grain launch(Offset off) noexcept
    {
        return { off, 0, true };
    }
};

// ---------------------------------------------------------------------------
// The scheduler.
// ---------------------------------------------------------------------------

/// One output sample = taps() (where to read, how to fade) then advance()
/// (move phases, launch grain B at the overlap start, swap at cycle end).
/// The fade is the §3.3 linear complementary ramp as a 15-bit integer
/// fraction; FadeShape has only the authentic Linear value at v1.
template <typename Policy>
class TwoGrainScheduler
{
public:
    using Grain = typename Policy::Grain;
    using Offset = typename Policy::Offset;

    /// Restart the schedule: grain A launches at `startOffset` with phase 0,
    /// grain B inactive. The caller reports this launch to any observer (the
    /// scheduler never sees launch observers — front-ends own reporting).
    void reset(const GrainGeometry& geometry, Offset startOffset) noexcept
    {
        geom_ = geometry;
        a_ = Policy::launch(startOffset);
        b_ = Grain{};
    }

    /// Streaming-only: swap in new geometry at a grain boundary (ADR-006
    /// "parameter changes apply at grain boundaries"). Callers must invoke
    /// this only right after advance() reported a swap — grain B is inactive
    /// there, so no in-flight crossfade is morphed.
    void setGeometry(const GrainGeometry& geometry) noexcept { geom_ = geometry; }

    [[nodiscard]] const GrainGeometry& geometry() const noexcept { return geom_; }
    [[nodiscard]] const Grain& grainA() const noexcept { return a_; }
    [[nodiscard]] const Grain& grainB() const noexcept { return b_; }

    /// Read positions + fade for the CURRENT output sample (§3.4 loop head).
    struct Taps
    {
        Offset posA{};            ///< grain A read position (off + phase)
        Offset posB{};            ///< grain B read position (valid iff hasB)
        std::int64_t fade15 = 0;  ///< 15-bit linear fade fraction [0, 32767]
        bool hasB = false;
    };

    [[nodiscard]] Taps taps() const noexcept
    {
        Taps t;
        t.posA = readPos(a_);
        t.hasB = b_.active;
        if (t.hasB)
        {
            t.posB = readPos(b_);
            t.fade15 = ((Policy::phase(a_) - geom_.ovStart) << 15) / geom_.fadeLen;
        }
        return t;
    }

    /// Advance one output sample (§3.4 loop tail): step both grains; launch
    /// grain B when A's phase reaches the overlap start (`nextLaunchOffset`
    /// maps A's start offset to B's — offline: off + hop_in frozen; streaming:
    /// off + the hop pending at this boundary); `onLaunch(offset)` fires once
    /// per launch for the task-012 observation hook. Returns true when grain A
    /// retired and B was swapped in — the GRAIN BOUNDARY where streaming
    /// applies parameter changes and exhaustion resyncs (ADR-006).
    template <typename NextLaunchOffset, typename OnLaunch>
    [[nodiscard]] bool advance(NextLaunchOffset&& nextLaunchOffset,
                               OnLaunch&& onLaunch)
    {
        Policy::advance(a_);
        if (b_.active)
            Policy::advance(b_);

        if (!b_.active && Policy::phase(a_) >= geom_.ovStart)
        {
            const Offset off = nextLaunchOffset(a_.off);
            b_ = Policy::launch(off);
            onLaunch(off);
        }

        if (Policy::phase(a_) >= geom_.c)
        {
            a_ = b_;
            b_ = Grain{};
            return true;
        }
        return false;
    }

private:
    [[nodiscard]] static Offset readPos(const Grain& g) noexcept
    {
        return g.off + static_cast<Offset>(Policy::phase(g));
    }

    GrainGeometry geom_{};
    Grain a_{};
    Grain b_{};
};

} // namespace mws::stretch::detail
