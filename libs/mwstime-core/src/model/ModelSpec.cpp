// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Per-model ModelSpec tables + the single clamping authority.
// Every number traces to docs/design/dsp-engine.md §1/§2 (the spec table),
// ADR-003 (S900 semantics), ADR-004 (S3000 reserved), ADR-006 (FX causality).

#include "mws/model/ModelSpec.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace mws::model {
namespace {

using engine::ParamId;
using engine::ParamSnapshot;

constexpr std::array<ModelSpec, kModelCount> kSpecs{{
    // S900 (1986) — RepitchEngine, no timestretch (ADR-003). Rate ceiling
    // 40 kHz [MAN §1] with rate = 2.5 x bandwidth [DRR F4] => BW <= 16.0.
    {
        .id = ModelId::S900,
        .shipping = true,
        .timeFactorMinPct = superset::kTimeFactorMin,
        .timeFactorMaxPct = superset::kTimeFactorMax,
        .bandwidthMinKHz = superset::kBandwidthMinKHz,
        .bandwidthMaxKHz = 16.0,
        .variableClock = true,
        .monoSum = true,
        .chain = CharacterChainKind::VarClock12Bit,
    },
    // S950 (1988) — stretch up to 999% [MAN §2]; bandwidth 3.0–19.2 kHz,
    // rate 7.5–48 kHz continuously variable [MAN §2 p.74].
    {
        .id = ModelId::S950,
        .shipping = true,
        .timeFactorMinPct = superset::kTimeFactorMin,
        .timeFactorMaxPct = 999.0,
        .bandwidthMinKHz = superset::kBandwidthMinKHz,
        .bandwidthMaxKHz = superset::kBandwidthMaxKHz,
        .variableClock = true,
        .monoSum = true,
        .chain = CharacterChainKind::VarClock12Bit,
    },
    // S1000 (1988) — CYCLIC 25–2000% [MAN §3]; fixed FS 44.1/22.05.
    {
        .id = ModelId::S1000,
        .shipping = true,
        .timeFactorMinPct = superset::kTimeFactorMin,
        .timeFactorMaxPct = superset::kTimeFactorMax,
        .bandwidthMinKHz = superset::kBandwidthMinKHz,
        .bandwidthMaxKHz = superset::kBandwidthMaxKHz,
        .variableClock = false,
        .monoSum = false,
        .chain = CharacterChainKind::FixedRate16Bit,
    },
    // S1100 (1990) — same engine as the S1000 (manual text verbatim
    // identical [MAN §4]); the 20-bit-DAC dither delta lives in the chain.
    {
        .id = ModelId::S1100,
        .shipping = true,
        .timeFactorMinPct = superset::kTimeFactorMin,
        .timeFactorMaxPct = superset::kTimeFactorMax,
        .bandwidthMinKHz = superset::kBandwidthMinKHz,
        .bandwidthMaxKHz = superset::kBandwidthMaxKHz,
        .variableClock = false,
        .monoSum = false,
        .chain = CharacterChainKind::FixedRate16Bit,
    },
    // S3000 (1992) — RESERVED slot, v1.1 (ADR-004): id + chain slot exist,
    // NO behavior at v1. Ranges mirror the S1000 so that an accidental
    // S3000 snapshot still clamps sanely; isShipping is the real gate.
    {
        .id = ModelId::S3000,
        .shipping = false,
        .timeFactorMinPct = superset::kTimeFactorMin,
        .timeFactorMaxPct = superset::kTimeFactorMax,
        .bandwidthMinKHz = superset::kBandwidthMinKHz,
        .bandwidthMaxKHz = superset::kBandwidthMaxKHz,
        .variableClock = false,
        .monoSum = false,
        .chain = CharacterChainKind::S3000Reserved,
    },
}};

static_assert(kSpecs[0].id == ModelId::S900 && kSpecs[1].id == ModelId::S950
                  && kSpecs[2].id == ModelId::S1000
                  && kSpecs[3].id == ModelId::S1100
                  && kSpecs[4].id == ModelId::S3000,
              "kSpecs must be indexed by ModelId declaration order");

} // namespace

const ModelSpec& ModelSpec::get(ModelId id) noexcept
{
    auto index = static_cast<std::size_t>(id);
    if (index >= kModelCount)
        index = static_cast<std::size_t>(ModelId::S1000);  // defensive default
    return kSpecs[index];
}

bool ModelSpec::isShipping(ModelId id) noexcept
{
    return get(id).shipping;
}

bool ModelSpec::isInert(ParamId param) const noexcept
{
    const bool varClockModel = variableClock;  // S900/S950
    const bool isS900 = (id == ModelId::S900);

    switch (param)
    {
        // INTELL only — inert on every model until INTELL ships (v1.1,
        // ADR-001; dsp-engine.md §2/§4). Stored for preset round-trip.
        case ParamId::Qual:
        case ParamId::Width:
            return true;

        // S1000/S1100 page field only [MAN §3 p.45].
        case ParamId::StretchMode:
            return varClockModel;

        // "CYCLIC & S950" rows (§2): inert only in S900 repitch mode (§6).
        case ParamId::CycleLen:
        case ParamId::HopMode:
            return isS900;

        // S950 only [MAN §2 p.30].
        case ParamId::Material:
            return id != ModelId::S950;

        // S900/S950 only (§2 bandwidth row).
        case ParamId::Bandwidth:
            return !varClockModel;

        // S1000/S1100 only [MAN §3 p.81].
        case ParamId::SampleRateSel:
            return varClockModel;

        // Apply to all models (timeFactor is the varispeed rate on the S900,
        // ADR-003; mode-scoped params like fxWindow stay model-applicable).
        case ParamId::TimeFactor:
        case ParamId::Transpose:
        case ParamId::Character:
        case ParamId::Norm:
        case ParamId::TempoSync:
        case ParamId::FxWindow:
        case ParamId::OutTrim:
            return false;
    }
    return false;  // unreachable for valid ParamId
}

double ModelSpec::modelRateHz(const ParamSnapshot& params) const noexcept
{
    if (variableClock)
    {
        // rate = 2.5 x bandwidth [DRR F4]; bandwidth in kHz => Hz factor 2500.
        const double bw = std::clamp(params.bandwidth, bandwidthMinKHz,
                                     bandwidthMaxKHz);
        return 2500.0 * bw;
    }
    return params.sampleRateSel == engine::SampleRateSel::Fs22050 ? 22050.0
                                                                  : 44100.0;
}

ParamSnapshot ModelSpec::clamp(ParamSnapshot params) const noexcept
{
    // timeFactor: superset 25–2000 [MAN §3]; S950 high end 999 [MAN §2].
    // ADR-006 causality: FX FREE mode clamps the low end to 100% —
    // compression needs future input (same rule clamps S900 rate <= 1, §6).
    double tMin = timeFactorMinPct;
    if (params.pluginMode == engine::PluginMode::Fx
        && params.fxWindow == engine::FxWindow::Free)
        tMin = std::max(tMin, 100.0);
    params.timeFactor = std::clamp(params.timeFactor, tMin, timeFactorMaxPct);

    if (!isInert(ParamId::CycleLen))
        params.cycleLen = std::clamp(params.cycleLen, superset::kCycleLenMin,
                                     superset::kCycleLenMax);

    params.transpose = std::clamp(params.transpose, superset::kTransposeMin,
                                  superset::kTransposeMax);

    if (!isInert(ParamId::Bandwidth))
        params.bandwidth =
            std::clamp(params.bandwidth, bandwidthMinKHz, bandwidthMaxKHz);

    params.outTrim = std::clamp(params.outTrim, superset::kOutTrimMinDb,
                                superset::kOutTrimMaxDb);

    // Inert params (qual/width everywhere at v1; per-model fields above when
    // inert) pass through unchanged so presets/state round-trip exactly.
    // Enum/bool fields carry no range to clamp.
    return params;
}

} // namespace mws::model
