// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ParamSnapshot — the POD parameter snapshot consumed by the audio thread and
// the offline renderer. Mirrors the unified parameter table in
// docs/design/dsp-engine.md §2 (that table IS the spec): one fixed superset
// range per parameter; the active ModelSpec clamps at the engine
// (architecture.md §6) and the LCD shows the clamped hardware value.
//
// Trivially copyable by hard requirement: the audio thread snapshots params
// by plain copy every block (architecture.md §4) — no heap, no indirection.

#pragma once

#include <cstdint>
#include <type_traits>

#include "mws/model/ModelId.h"

namespace mws::engine {

/// STRETCH MODE — S1000/S1100 page field [MAN §3 p.45].
/// INTELL is greyed at v1 (deferred to v1.1, dsp-engine.md §4 / ADR-001).
enum class StretchMode : std::uint8_t { Cyclic, Intell };

/// TIMING — hop arithmetic [AKZ §2.2]. CLASSIC (integer hop) is
/// hardware-faithful; REVISED uses fractional hops (sample-exact timing).
enum class HopMode : std::uint8_t { Classic, Revised };

/// MON1/POL2 — S950 material switch [MAN §2 p.30].
enum class Material : std::uint8_t { Mon1, Pol2 };

/// FS — S1000/S1100 fixed model sample rate [MAN §3 p.81].
enum class SampleRateSel : std::uint8_t { Fs44100, Fs22050 };

/// SYNC — FX-mode host tempo sync (modern UX, locked decision).
enum class TempoSync : std::uint8_t { Off, Host };

/// WINDOW — FX-mode capture/resync window (ADR-006). Default 1 bar.
enum class FxWindow : std::uint8_t {
    QuarterBar, HalfBar, OneBar, TwoBars, FourBars, EightBars, Free,
};

/// MODE — FX-first hybrid (locked decision; ADR-006).
enum class PluginMode : std::uint8_t { Fx, Sample };

/// Identifies a ParamSnapshot field for per-model applicability queries
/// (ModelSpec::isInert). Matches the §2 table rows that live in the snapshot.
enum class ParamId : std::uint8_t {
    TimeFactor,
    CycleLen,
    StretchMode,
    HopMode,
    Transpose,
    Qual,
    Width,
    Material,
    Bandwidth,
    SampleRateSel,
    Character,
    Norm,
    TempoSync,
    FxWindow,
    OutTrim,
};

/// The audio-thread parameter snapshot. Field defaults are the dsp-engine.md
/// §2 table defaults, byte for byte. Validated/clamped per ModelSpec — never
/// consume a raw snapshot in an engine without `ModelSpec::clamp` first.
struct ParamSnapshot {
    /// TIME FACTOR, % of original length, superset 25.00–2000.00. Default 100
    /// [MAN §5 example screen]. S950 engine-clamps to 999 [MAN §2]; FX FREE
    /// engine-clamps the low end to 100 (ADR-006).
    double timeFactor = 100.0;

    /// CYCLE LENGTH (S1000/S1100) / D-TIME (S950), samples at model rate,
    /// 20–2000 [AKZ §2.1 convention, ADR-001 deviation]. Default 1000.
    int cycleLen = 1000;

    /// STRETCH MODE. CYCLIC default; INTELL greyed until v1.1.
    StretchMode stretchMode = StretchMode::Cyclic;

    /// TIMING. CLASSIC default (hardware-faithful).
    HopMode hopMode = HopMode::Classic;

    /// TRANSPOSE, semitones, -24.00–+24.00 in 1-cent steps. Default 0.
    double transpose = 0.0;

    /// QUAL, 1–99, INTELL only — **inert at v1** (INTELL deferred, ADR-001)
    /// but stored so presets/state round-trip it (the §9 "Jungle Amen 300"
    /// preset stores qual 20 inert). Default 10 [MAN §5 screen].
    int qual = 10;

    /// WIDTH, 1–99, INTELL only — inert at v1, stored for round-trip.
    /// Default 10 [MAN §5 screen].
    int width = 10;

    /// MON1/POL2, S950 only. POL2 default (breaks/loops are the primary use).
    Material material = Material::Pol2;

    /// BANDWIDTH, kHz, superset 3.0–19.2 (S950); S900 engine-clamps to 16.0
    /// (rate ceiling 40 kHz [MAN §1], rate = 2.5 x bandwidth [DRR F4]).
    /// Default max.
    double bandwidth = 19.2;

    /// FS, S1000/S1100 only. Default 44.1 kHz.
    SampleRateSel sampleRateSel = SampleRateSel::Fs44100;

    /// CHARACTER — modern-UX bypass of the §8 chain. Default ON.
    bool character = true;

    /// NORM — SAMPLE-render peak normalization. Default OFF (authentic; no
    /// manual documents normalization).
    bool norm = false;

    /// SYNC — FX-mode host tempo sync. Default OFF.
    TempoSync tempoSync = TempoSync::Off;

    /// WINDOW — FX-mode capture/resync window. Default 1 bar (ADR-006).
    FxWindow fxWindow = FxWindow::OneBar;

    /// OUTPUT trim, dB, -24–+12. Default 0.
    double outTrim = 0.0;

    /// MODEL. Default S1000 (locked + ADR-004). Non-automatable.
    model::ModelId model = model::ModelId::S1000;

    /// MODE. Default FX (FX-first, locked). Non-automatable.
    PluginMode pluginMode = PluginMode::Fx;
};

static_assert(std::is_trivially_copyable_v<ParamSnapshot>,
              "ParamSnapshot is copied raw on the audio thread");
static_assert(std::is_standard_layout_v<ParamSnapshot>,
              "ParamSnapshot is a plain POD by contract");

} // namespace mws::engine
