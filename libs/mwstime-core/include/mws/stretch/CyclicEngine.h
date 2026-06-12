// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CyclicEngine — the S1000/S1100 CYCLIC two-grain overlap timestretch
// (docs/design/dsp-engine.md §3; the base for the S950 engine). Implements
// the adopted [AKZ §4.2] OpenMPT-style scheduler (ADR-001 resolution 3) —
// NEVER the [AKZ §10] splice formula. All splice constants live in SpliceCal
// so hardware calibration is data-only.
//
// CLASSIC hop mode (plan/backlog/010): integer input hop, integer-% time
// factor, and a float-free stretch path (32.32 fixed-point positions, rounded
// integer crossfade lerp with a 15-bit fraction — dsp-engine.md §3.2;
// cross-platform bit-exactness, architecture.md §2).
//
// REVISED hop mode (plan/backlog/011): fractional input hop (hop_in = hop_out/T,
// no integer-% coercion), giving sample-exact timing (output length round(N·T));
// fractional grain starts make source reads 2-point linear interpolations, the
// sole source of REVISED's slight pitch drift (dsp-engine.md §3.2, §3.4 note).

#pragma once

#include <cstdint>

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"

namespace mws::stretch {

/// Crossfade shape between overlapping grains. Linear (complementary) is the
/// authentic choice of every faithful open implementation; the -6 dB midpoint
/// dip on uncorrelated material is part of the characteristic flutter
/// (dsp-engine.md §3.3, [AKZ §3 property 3, §4.1, §8]). No equal-power option
/// in the authentic engines.
enum class FadeShape : std::uint8_t { Linear };

/// Rounding rule for the CLASSIC integer input hop (dsp-engine.md §3.1).
enum class HopRounding : std::uint8_t { RoundNearest };

/// The cyclic timestretch engine (offline, mono). Stateless across renders:
/// the same (input, parameters) always yields bit-identical output.
class CyclicEngine
{
public:
    /// Splice calibration (dsp-engine.md §3.1) — THE single home of the
    /// overlap/fade/rounding constants. Defaults match the OpenMPT reference
    /// (crossfade over the last 20% of each grain, [AKZ §4.1]) and are (PI)
    /// pending hardware-capture calibration; retuning them is a data change
    /// here, never an engine change (ADR-001 resolution 3).
    struct SpliceCal
    {
        float overlapF = 0.20f;                       ///< F, fraction of C
        FadeShape shape = FadeShape::Linear;          ///< §3.3
        HopRounding rounding = HopRounding::RoundNearest; ///< §3.2
    };

    CyclicEngine() noexcept = default;
    explicit CyclicEngine(SpliceCal cal) noexcept : cal_(cal) {}

    [[nodiscard]] const SpliceCal& spliceCal() const noexcept { return cal_; }

    /// Offline mono render of the [AKZ §4.2] two-grain scheduler
    /// (dsp-engine.md §3.4 reference pseudocode).
    ///
    /// CLASSIC (this task): timeFactorPct is coerced to an integer percent
    /// [AKZ §2.1]; hop_in = round(hop_out / T) >= 1; grain positions are
    /// 32.32 fixed point and integer-valued, so stretch alone never resamples
    /// — every output sample is a verbatim input copy or, inside a crossfade
    /// only, the rounded integer lerp (15-bit fraction) of exactly two input
    /// samples (dsp-engine.md §3.2 properties).
    ///
    /// Edge rules (§3.4): reads past the input end return 0; cycleLenSamples
    /// is clamped to the input length when the file is shorter; T < 100%
    /// compresses (hop_in > hop_out skips material); the render ends when the
    /// next grain cannot play a complete cycle, making the output length
    /// exactly the §3.4 schedule-derived value (see expectedOutputLength).
    ///
    /// REVISED (task 011): hop_in = hop_out / T is fractional (the time factor
    /// keeps its 0.01% step — no integer coercion); grain starts are fractional
    /// so source reads are 2-point linear interpolations; the render runs to the
    /// sample-exact length round(N·T) (dsp-engine.md §3.4 REVISED note). Reads
    /// past the input end still return 0.
    ///
    /// Parameter-range clamping (superset/model) is NOT done here — ModelSpec
    /// is the single clamping authority. The engine only enforces its own
    /// arithmetic safety (hop_in >= 1, C >= 1, T >= 1%).
    [[nodiscard]] core::AudioBuffer render(core::ConstAudioView src,
                                           int cycleLenSamples,
                                           double timeFactorPct,
                                           engine::HopMode mode) const;

    /// Output length the scheduler will produce, derived from the §3.4
    /// schedule (used by the LCD later):
    ///   G = floor((N - C) / hop_in) + 1 complete grains,
    ///   length = (G - 1) * hop_out + C.
    /// Quantized by the integer hop — deliberately NOT round(N * T)
    /// ("bad timing", [AKZ §2.2]). REVISED returns the sample-exact round(N * T)
    /// (task 011; dsp-engine.md §3.4 REVISED note).
    [[nodiscard]] std::int64_t expectedOutputLength(std::int64_t numInputFrames,
                                                    int cycleLenSamples,
                                                    double timeFactorPct,
                                                    engine::HopMode mode) const;

private:
    SpliceCal cal_{};
};

} // namespace mws::stretch
