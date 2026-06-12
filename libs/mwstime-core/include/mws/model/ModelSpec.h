// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ModelSpec — the per-model data tables that make models "data, not code"
// (docs/design/architecture.md §2.1; docs/design/dsp-engine.md §1, §2, §8).
//
// The host-facing parameter set uses FIXED superset ranges (never remapped at
// runtime — VST3 hosts cache parameter info; architecture.md §6). The active
// ModelSpec clamps at the engine via ModelSpec::clamp — the single clamping
// authority used by engines, the LCD model, and FX. The LCD displays the
// clamped hardware value.

#pragma once

#include <cstdint>

#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"

namespace mws::model {

/// Fixed host-facing superset ranges (dsp-engine.md §2 table; architecture.md
/// §6). These never change at runtime; per-model clamping happens at the
/// engine, inside these bounds.
namespace superset {

inline constexpr double kTimeFactorMin = 25.0;     ///< % [MAN §3]
inline constexpr double kTimeFactorMax = 2000.0;   ///< % [MAN §3]
inline constexpr int    kCycleLenMin = 20;         ///< samples [AKZ §2.1]
inline constexpr int    kCycleLenMax = 2000;       ///< samples [AKZ §2.1]
inline constexpr double kTransposeMin = -24.0;     ///< st [AKZ §2.1]
inline constexpr double kTransposeMax = 24.0;      ///< st [AKZ §2.1]
inline constexpr int    kQualMin = 1;              ///< [MAN §3 p.47]
inline constexpr int    kQualMax = 99;             ///< [MAN §3 p.47]
inline constexpr int    kWidthMin = 1;             ///< [MAN §3 p.47]
inline constexpr int    kWidthMax = 99;            ///< [MAN §3 p.47]
inline constexpr double kBandwidthMinKHz = 3.0;    ///< [MAN §2 p.74]
inline constexpr double kBandwidthMaxKHz = 19.2;   ///< S950 max [MAN §2 p.74]
inline constexpr double kOutTrimMinDb = -24.0;     ///< (PI)
inline constexpr double kOutTrimMaxDb = 12.0;      ///< (PI)

} // namespace superset

/// Which character chain (dsp-engine.md §8) a model routes through.
enum class CharacterChainKind : std::uint8_t {
    VarClock12Bit,   ///< §8.1 — S900/S950 variable-clock 12-bit chain
    FixedRate16Bit,  ///< §8.2 — S1000/S1100 fixed-rate 16-bit chain
    S3000Reserved,   ///< §8.3 — v1.1; reserved, NO behavior at v1 (ADR-004)
};

/// Per-model parameter table: engine clamp ranges, rate rules, character
/// configuration, and per-param applicability. One immutable instance per
/// ModelId, obtained via ModelSpec::get.
struct ModelSpec {
    ModelId id;

    /// False only for the reserved S3000 slot at v1 (ADR-004).
    bool shipping;

    // --- engine clamp ranges (always within the superset) -----------------
    double timeFactorMinPct;  ///< 25 everywhere
    double timeFactorMaxPct;  ///< 999 on the S950 [MAN §2]; 2000 elsewhere
    double bandwidthMinKHz;   ///< varclock models; 3.0 [MAN §2 p.74]
    double bandwidthMaxKHz;   ///< 16.0 S900 (rate <= 40 kHz [MAN §1]); 19.2 S950

    // --- model rate rules (dsp-engine.md §1) -------------------------------
    /// True for S900/S950: model rate = 2.5 x bandwidth, continuously
    /// variable [DRR F4]. False for the fixed-rate models (FS 44.1/22.05).
    bool variableClock;

    /// True for S900/S950: stereo input is summed to mono first (the S950 is
    /// a mono machine [MAN §2]; authentic, stated on the LCD).
    bool monoSum;

    /// Character-chain selector (dsp-engine.md §8).
    CharacterChainKind chain;

    // --- lookups ------------------------------------------------------------
    /// The immutable spec table entry for a model. The reserved S3000 entry
    /// exists (id + chain slot) but is not shipping — query isShipping.
    [[nodiscard]] static const ModelSpec& get(ModelId id) noexcept;

    /// ADR-004: S3000 slot reserved at v1 — false; true for the v1 four.
    [[nodiscard]] static bool isShipping(ModelId id) noexcept;

    // --- behavior -----------------------------------------------------------
    /// True when a parameter does not apply to this model at v1
    /// (dsp-engine.md §2 "Applies to" column; §6 for S900; qual/width are
    /// inert on every model until INTELL ships, ADR-001).
    [[nodiscard]] bool isInert(engine::ParamId param) const noexcept;

    /// Model sample rate in Hz for a snapshot: 2.5 x bandwidth (kHz) for the
    /// varclock models (bandwidth clamped to this model's range first);
    /// 44100/22050 per sampleRateSel for the fixed-rate models.
    [[nodiscard]] double
    modelRateHz(const engine::ParamSnapshot& params) const noexcept;

    /// THE single clamping authority (engines, LCD model, FX). Pure: clamps
    /// every applicable parameter into this model's engine range and returns
    /// the result; inert parameters pass through byte-for-byte unchanged
    /// (presets/state round-trip them). Also applies the ADR-006 causality
    /// clamp: in FX FREE mode the timeFactor low end is 100%
    /// (dsp-engine.md §2 timeFactor row, §3.5, §6).
    [[nodiscard]] engine::ParamSnapshot
    clamp(engine::ParamSnapshot params) const noexcept;
};

} // namespace mws::model
