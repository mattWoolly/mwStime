// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// S950Engine — the S950 STRETCH page (EDIT SAMPLE page 14 [MAN §2 pp.29–30])
// as a configuration/extension of CyclicEngine (dsp-engine.md §5; ADR-001:
// engines = CyclicEngine + config). The §5 mapping table is the spec; every
// row marked (PI) below is deviation-until-calibrated — real S950 captures are
// a v1-freeze gate for the D-TIME mapping (testing-strategy.md §7 Wave 2):
//
//   STRETCH %  -> T = stretch/100, integer %, CLASSIC hop by default;
//                 engine-clamped to <= 999 ("up to 999%" [MAN §2])
//   D-TIME     -> cycle length C in samples, 20–2000
//                 (PI, deviation-until-calibrated — mapDTimeToCycleLen)
//   AUTO-D     -> run the §7.1 auto-cycle detector (AutoCycle, task 014) and
//                 use the result as C (feature documented [MAN §2]; algorithm
//                 ours (PI))
//   MON1       -> per-grain: snap C to the nearest detected pitch period
//                 (AutoCorr::bestLagNear around the set C)
//                 (PI, deviation-until-calibrated)
//   POL2       -> fixed C exactly as set (PI) — degenerates to CyclicEngine
//                 CLASSIC bit-exactly (tested)
//
// Mono: the S950 is a mono machine [MAN §2]. This engine takes mono input
// only; stereo summing happens in OfflineRenderer/CharacterChain (dsp-engine
// §5), never here. The §8.1 S950 character chain (12-bit/variable clock) is
// tasks 016/019 — out of scope for the engine.

#pragma once

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/stretch/CyclicEngine.h"

namespace mws::stretch {

/// The S950 STRETCH page controls (LCD: ">14 STRETCH ... #200% / D-time 1000
/// #Auto D_ [Mon1/Pol2]" [MAN §2 p.30]). Defaults are the manual's printed
/// display defaults.
struct S950Params
{
    /// STRETCH percentage, display default 200% [MAN §2 p.30]. Integer-%
    /// CLASSIC by default; the engine clamps to kTimeFactorMaxPct (999).
    double timeFactorPct = 200.0;

    /// D-TIME, display default 1000 [MAN §2 p.30]. Units undocumented on the
    /// hardware; mapped to cycle length C in samples, 20–2000, by
    /// mapDTimeToCycleLen (dsp-engine.md §5 (PI, deviation-until-calibrated)).
    int dTime = 1000;

    /// MON1/POL2 material switch [MAN §2 p.30]. POL2 default (breaks/loops
    /// are the primary use, dsp-engine.md §2 (PI)).
    engine::Material material = engine::Material::Pol2;

    /// AUTO-D: "like autoloop, gets the S950 to select a suitable D-TIME
    /// value" [MAN §2]. When true, dTime is ignored and C comes from the
    /// task-014 detector (dsp-engine.md §5).
    bool autoD = false;

    /// TIMING (dsp-engine.md §2 — applies to "CYCLIC & S950"). CLASSIC is the
    /// hardware-faithful default (integer hop, integer-% [AKZ §2.2]).
    engine::HopMode hopMode = engine::HopMode::Classic;

    /// Sample rate of monoSrc, Hz — used only by the AUTO-D analysis-window
    /// cap (AutoCycle::kAnalysisWindowSeconds).
    double sampleRate = 44100.0;
};

/// Offline S950 render result.
struct S950Result
{
    /// Mono rendered audio (stretch only — the §8.1 character chain runs
    /// downstream, tasks 016/019).
    core::AudioBuffer out;

    /// The cycle length C the schedule actually used for its FIRST grain,
    /// after AUTO-D resolution, the MON1 snap, and the clamp of C to the
    /// input length [AKZ §2.1]. In MON1, later grains may re-snap (per-grain
    /// semantics); on steady material the snap is constant.
    int effectiveCycleLen = 0;

    /// The time factor actually rendered: clamped to 999 [MAN §2], and
    /// integer-coerced in CLASSIC [AKZ §2.1].
    double effectiveTimeFactorPct = 0.0;
};

/// The S950 stretch engine (offline, mono). Stateless across renders: the
/// same (input, parameters) always yields bit-identical output. Analysis-time
/// code (AutoCorr/AutoCycle allocate scratch): NOT real-time-safe.
class S950Engine
{
public:
    S950Engine() noexcept = default;

    /// Shares CyclicEngine's splice calibration (dsp-engine.md §3.1) — the
    /// S950 is CyclicEngine + config (ADR-001), so retuning splice constants
    /// retunes this engine too.
    explicit S950Engine(CyclicEngine::SpliceCal cal) noexcept : cyclic_(cal) {}

    // ----------------------------------------------------------------------
    // Named-constants block — every value the QA fleet may retune lives here
    // (testing-strategy.md §7: each (PI) constant carries a tuning note;
    // test_s950.cpp pins these values).
    // ----------------------------------------------------------------------

    /// STRETCH ceiling, percent: "up to 999%" [MAN §2 intro, p.29, spec
    /// p.74]. Documented hardware value — NOT (PI), not retunable.
    static constexpr int kTimeFactorMaxPct = 999;

    /// D-TIME -> C range, samples: 20–2000 — the CYCLE LENGTH / D-TIME
    /// parameter range (dsp-engine.md §2, Akaizer's convention adopted as an
    /// ADR-001 deviation; the manual states no range). Tuning note: shared
    /// with the cycleLen parameter — not retunable independently of it.
    static constexpr int kDTimeMinSamples = 20;
    static constexpr int kDTimeMaxSamples = 2000;

    /// MON1 snap search half-width as a fraction of the set C (the
    /// AutoCorr::bestLagNear window, dsp-engine.md §5). **(PI)** Tuning note:
    /// pure inference — no manual documents MON1 internals. 0.5 spans a
    /// tritone-ish band around the set C: wide enough to find the true period
    /// from a roughly-set D-TIME, narrow enough to exclude the 2x/0.5x
    /// period-multiple ambiguity when C is set near the true period. Retune
    /// against hardware MON1 captures (testing-strategy.md §7 Wave 2).
    static constexpr double kMon1SearchFraction = 0.5;

    /// Minimum normalized-autocorrelation peak for a MON1 snap. **(PI)**
    /// Tuning note: same confidence floor as AutoCycle::kPeakThreshold (0.3
    /// rejects noise, accepts pitched material); below it the grain keeps the
    /// previous grain's C (steady fallback — no snap thrash on silence).
    static constexpr float kMon1PeakThreshold = 0.3f;

    /// MON1 per-grain analysis window, in multiples of the set C. **(PI)**
    /// Tuning note: 4 cycles gives the autocorrelation >2 full periods of
    /// overlap at the largest searched lag (1.5 x C) while keeping the
    /// per-grain analysis local in time (the point of per-grain snapping).
    static constexpr int kMon1WindowCycles = 4;

    /// THE D-TIME mapping (dsp-engine.md §5, one function by task-015
    /// mandate): D-TIME == cycle length C in samples, clamped to 20–2000.
    /// **(PI, deviation-until-calibrated)** — the manual documents neither
    /// units nor range [MAN §2 pp.29–30]; the mapping rests on the behavior
    /// match ("longer D-TIME => slight tremolo, shorter => metallic" [MAN §2]
    /// == the documented cycle-length behavior [AKZ §2.1]) plus the shared
    /// default of 1000. Real S950 captures are the v1-freeze calibration gate
    /// for this function; change it HERE only.
    [[nodiscard]] static constexpr int mapDTimeToCycleLen(int dTime) noexcept
    {
        return dTime < kDTimeMinSamples   ? kDTimeMinSamples
               : dTime > kDTimeMaxSamples ? kDTimeMaxSamples
                                          : dTime;
    }

    /// Offline mono render of the S950 STRETCH page. POL2 delegates to the
    /// wrapped CyclicEngine with C fixed exactly as set (bit-exact degenerate
    /// equivalence, tested); MON1 runs the same CLASSIC schedule but re-snaps
    /// C at every grain launch via AutoCorr::bestLagNear around the set C
    /// (dsp-engine.md §5 (PI)). monoSrc IS mono by construction (single-
    /// channel view); use the AudioBuffer overload for an asserted check.
    [[nodiscard]] S950Result render(core::ConstAudioView monoSrc,
                                    const S950Params& params) const;

    /// Convenience overload asserting the mono contract: the S950 is a mono
    /// machine [MAN §2]; stereo summing is OfflineRenderer/CharacterChain's
    /// job (dsp-engine.md §5), never the engine's.
    [[nodiscard]] S950Result render(const core::AudioBuffer& monoSrc,
                                    const S950Params& params) const;

    [[nodiscard]] const CyclicEngine& cyclicEngine() const noexcept
    {
        return cyclic_;
    }

private:
    CyclicEngine cyclic_;
};

} // namespace mws::stretch
