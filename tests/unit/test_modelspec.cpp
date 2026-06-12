// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 009 — mws::model data layer: ModelId, ModelSpec tables, ParamSnapshot
// + engine-side clamping. Every range/default asserted here traces to the
// unified parameter table in docs/design/dsp-engine.md §2 (which IS the spec),
// the engine inventory in §1, architecture.md §2.1/§6, ADR-003 and ADR-004.
//
// Test-case names begin with the tag word so `ctest -R modelspec` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"

using mws::engine::FxWindow;
using mws::engine::HopMode;
using mws::engine::Material;
using mws::engine::ParamId;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::SampleRateSel;
using mws::engine::StretchMode;
using mws::engine::TempoSync;
using mws::model::CharacterChainKind;
using mws::model::ModelId;
using mws::model::ModelSpec;

// ---------------------------------------------------------------------------
// ModelId
// ---------------------------------------------------------------------------

TEST_CASE("modelspec: ModelId enumerates all five models with name strings",
          "[modelspec]")
{
    // S3000 slot is reserved at v1 (ADR-004) — the id must exist.
    STATIC_REQUIRE(mws::model::kModelCount == 5);

    REQUIRE(mws::model::toString(ModelId::S900) == "S900");
    REQUIRE(mws::model::toString(ModelId::S950) == "S950");
    REQUIRE(mws::model::toString(ModelId::S1000) == "S1000");
    REQUIRE(mws::model::toString(ModelId::S1100) == "S1100");
    REQUIRE(mws::model::toString(ModelId::S3000) == "S3000");
}

TEST_CASE("modelspec: S3000 id exists but is not shipping at v1 (ADR-004)",
          "[modelspec]")
{
    REQUIRE_FALSE(ModelSpec::isShipping(ModelId::S3000));

    REQUIRE(ModelSpec::isShipping(ModelId::S900));
    REQUIRE(ModelSpec::isShipping(ModelId::S950));
    REQUIRE(ModelSpec::isShipping(ModelId::S1000));
    REQUIRE(ModelSpec::isShipping(ModelId::S1100));
}

// ---------------------------------------------------------------------------
// ParamSnapshot — POD + defaults (dsp-engine.md §2 table)
// ---------------------------------------------------------------------------

TEST_CASE("modelspec: ParamSnapshot is a trivially copyable audio-thread POD",
          "[modelspec]")
{
    // architecture.md §4: the audio thread snapshots params by plain copy.
    STATIC_REQUIRE(std::is_trivially_copyable_v<ParamSnapshot>);
    STATIC_REQUIRE(std::is_standard_layout_v<ParamSnapshot>);
}

TEST_CASE("modelspec: defaults match the dsp-engine.md section 2 table exactly",
          "[modelspec]")
{
    const ParamSnapshot p{};

    CHECK(p.timeFactor == 100.0);                       // default 100 [MAN §5]
    CHECK(p.cycleLen == 1000);                          // Akaizer/S3000/SPOD x3
    CHECK(p.stretchMode == StretchMode::Cyclic);        // INTELL greyed at v1
    CHECK(p.hopMode == HopMode::Classic);               // hardware-faithful
    CHECK(p.transpose == 0.0);
    CHECK(p.qual == 10);                                // [MAN §5 screen]
    CHECK(p.width == 10);                               // [MAN §5 screen]
    CHECK(p.material == Material::Pol2);                // breaks/loops primary use
    CHECK(p.bandwidth == 19.2);                         // default "max" (superset)
    CHECK(p.sampleRateSel == SampleRateSel::Fs44100);   // FS default 44.1
    CHECK(p.character == true);                         // CHARACTER ON
    CHECK(p.norm == false);                             // NORM OFF (authentic)
    CHECK(p.tempoSync == TempoSync::Off);
    CHECK(p.fxWindow == FxWindow::OneBar);              // WINDOW default 1 bar
    CHECK(p.outTrim == 0.0);
    CHECK(p.model == ModelId::S1000);                   // MODEL default S1000
    CHECK(p.pluginMode == PluginMode::Fx);              // FX-first (locked)
}

TEST_CASE("modelspec: qual and width exist and round-trip even though inert at v1",
          "[modelspec]")
{
    // INTELL is deferred (ADR-001, dsp-engine.md §2/§4) but presets/state must
    // round-trip qual/width — the §9 "Jungle Amen 300" preset stores
    // qual 20 / width 10 inert on an S1100.
    ParamSnapshot p{};
    p.model = ModelId::S1100;
    p.qual = 20;
    p.width = 10;

    const ParamSnapshot out = ModelSpec::get(p.model).clamp(p);
    CHECK(out.qual == 20);
    CHECK(out.width == 10);

    // Inert at v1 on every model (INTELL only).
    for (const ModelId m : mws::model::kAllModels)
    {
        CHECK(ModelSpec::get(m).isInert(ParamId::Qual));
        CHECK(ModelSpec::get(m).isInert(ParamId::Width));
    }
}

// ---------------------------------------------------------------------------
// Superset ranges (architecture.md §6 — fixed, never remapped at runtime)
// ---------------------------------------------------------------------------

TEST_CASE("modelspec: superset ranges are the fixed host-facing constants",
          "[modelspec]")
{
    namespace ss = mws::model::superset;
    STATIC_REQUIRE(ss::kTimeFactorMin == 25.0);
    STATIC_REQUIRE(ss::kTimeFactorMax == 2000.0);
    STATIC_REQUIRE(ss::kCycleLenMin == 20);
    STATIC_REQUIRE(ss::kCycleLenMax == 2000);
    STATIC_REQUIRE(ss::kTransposeMin == -24.0);
    STATIC_REQUIRE(ss::kTransposeMax == 24.0);
    STATIC_REQUIRE(ss::kQualMin == 1);
    STATIC_REQUIRE(ss::kQualMax == 99);
    STATIC_REQUIRE(ss::kWidthMin == 1);
    STATIC_REQUIRE(ss::kWidthMax == 99);
    STATIC_REQUIRE(ss::kBandwidthMinKHz == 3.0);
    STATIC_REQUIRE(ss::kBandwidthMaxKHz == 19.2);
    STATIC_REQUIRE(ss::kOutTrimMinDb == -24.0);
    STATIC_REQUIRE(ss::kOutTrimMaxDb == 12.0);
}

// ---------------------------------------------------------------------------
// Clamping — the single authority (engines, LCD model, FX)
// ---------------------------------------------------------------------------

TEST_CASE("modelspec: S950 clamps timeFactor 1500 to 999; superset untouched",
          "[modelspec]")
{
    ParamSnapshot p{};
    p.model = ModelId::S950;
    p.timeFactor = 1500.0;

    const ParamSnapshot out = ModelSpec::get(ModelId::S950).clamp(p);
    CHECK(out.timeFactor == 999.0);                     // [MAN §2]

    // The host-facing superset range is NOT remapped by clamping
    // (architecture.md §6 — VST3 hosts cache parameter info).
    STATIC_REQUIRE(mws::model::superset::kTimeFactorMax == 2000.0);

    // S1000/S1100 keep the full 25–2000 range [MAN §3].
    ParamSnapshot q{};
    q.model = ModelId::S1000;
    q.timeFactor = 1500.0;
    CHECK(ModelSpec::get(ModelId::S1000).clamp(q).timeFactor == 1500.0);
}

TEST_CASE("modelspec: S900 stretch-only params are inert and pass through clamp "
          "unchanged",
          "[modelspec]")
{
    const ModelSpec& s900 = ModelSpec::get(ModelId::S900);

    // dsp-engine.md §6 / ADR-003: in S900 mode stretchMode, cycleLen, qual,
    // width are inert (hopMode applies to "CYCLIC & S950" only — §2 table).
    CHECK(s900.isInert(ParamId::StretchMode));
    CHECK(s900.isInert(ParamId::CycleLen));
    CHECK(s900.isInert(ParamId::HopMode));
    CHECK(s900.isInert(ParamId::Qual));
    CHECK(s900.isInert(ParamId::Width));

    // timeFactor still applies (it is the varispeed rate, ADR-003).
    CHECK_FALSE(s900.isInert(ParamId::TimeFactor));

    // Clamp must leave inert params byte-for-byte unchanged, even out of range.
    ParamSnapshot p{};
    p.model = ModelId::S900;
    p.cycleLen = 5000;     // outside even the superset range
    p.qual = 120;
    p.width = 0;
    p.stretchMode = StretchMode::Intell;
    p.hopMode = HopMode::Revised;

    const ParamSnapshot out = s900.clamp(p);
    CHECK(out.cycleLen == 5000);
    CHECK(out.qual == 120);
    CHECK(out.width == 0);
    CHECK(out.stretchMode == StretchMode::Intell);
    CHECK(out.hopMode == HopMode::Revised);
}

TEST_CASE("modelspec: bandwidth clamps at 16.0 on S900, 19.2 allowed on S950",
          "[modelspec]")
{
    ParamSnapshot p{};
    p.bandwidth = 19.2;

    // S900 rate ceiling 40 kHz [MAN §1] and rate = 2.5 x bandwidth [DRR F4]
    // => BW <= 16.0.
    p.model = ModelId::S900;
    CHECK(ModelSpec::get(ModelId::S900).clamp(p).bandwidth == 16.0);

    // S950 spec: bandwidth 3–19.2 kHz [MAN §2 p.74].
    p.model = ModelId::S950;
    CHECK(ModelSpec::get(ModelId::S950).clamp(p).bandwidth == 19.2);

    // Low end is 3.0 on both.
    p.bandwidth = 1.0;
    CHECK(ModelSpec::get(ModelId::S900).clamp(p).bandwidth == 3.0);
    p.model = ModelId::S950;
    CHECK(ModelSpec::get(ModelId::S950).clamp(p).bandwidth == 3.0);

    // bandwidth is inert on the fixed-rate models — passes through unchanged.
    p.model = ModelId::S1000;
    p.bandwidth = 1.0;
    CHECK(ModelSpec::get(ModelId::S1000).isInert(ParamId::Bandwidth));
    CHECK(ModelSpec::get(ModelId::S1000).clamp(p).bandwidth == 1.0);
}

TEST_CASE("modelspec: clamp bounds the generally applicable params", "[modelspec]")
{
    ParamSnapshot p{};
    p.model = ModelId::S1000;
    p.cycleLen = 5000;
    p.transpose = 36.0;
    p.outTrim = 20.0;
    p.timeFactor = 10.0;

    const ParamSnapshot out = ModelSpec::get(ModelId::S1000).clamp(p);
    CHECK(out.cycleLen == 2000);    // 20–2000 [AKZ §2.1, ADR-001 deviation]
    CHECK(out.transpose == 24.0);   // ±24 st [AKZ §2.1]
    CHECK(out.outTrim == 12.0);     // -24–+12 dB
    CHECK(out.timeFactor == 25.0);  // 25–2000 [MAN §3]
}

TEST_CASE("modelspec: FX FREE mode clamps the timeFactor low end to 100 (ADR-006)",
          "[modelspec]")
{
    // dsp-engine.md §2 timeFactor row: "FX FREE mode engine-clamps low end to
    // 100 (ADR-006)" — compression needs future input; only SYNC windows
    // permit it. Same causality clamp for the S900 rate <= 1 rule (§6).
    ParamSnapshot p{};
    p.model = ModelId::S1000;
    p.pluginMode = PluginMode::Fx;
    p.fxWindow = FxWindow::Free;
    p.timeFactor = 50.0;
    CHECK(ModelSpec::get(ModelId::S1000).clamp(p).timeFactor == 100.0);

    // SYNC windows allow compression.
    p.fxWindow = FxWindow::OneBar;
    CHECK(ModelSpec::get(ModelId::S1000).clamp(p).timeFactor == 50.0);

    // SAMPLE mode (offline render) allows the full range.
    p.pluginMode = PluginMode::Sample;
    p.fxWindow = FxWindow::Free;
    CHECK(ModelSpec::get(ModelId::S1000).clamp(p).timeFactor == 50.0);
}

// ---------------------------------------------------------------------------
// Model rate rules + character configuration (dsp-engine.md §1, §8)
// ---------------------------------------------------------------------------

TEST_CASE("modelspec: model rate rules — 2.5 x bandwidth (varclock) and fixed FS",
          "[modelspec]")
{
    ParamSnapshot p{};

    // S950: rate = 2.5 x bandwidth [DRR F4]; 19.2 kHz BW => 48 kHz rate.
    p.model = ModelId::S950;
    p.bandwidth = 19.2;
    CHECK(ModelSpec::get(ModelId::S950).modelRateHz(p) == 48000.0);
    p.bandwidth = 3.0;  // floor: 7.5 kHz rate
    CHECK(ModelSpec::get(ModelId::S950).modelRateHz(p) == 7500.0);

    // S900: rate ceiling 40 kHz [MAN §1] — BW is clamped to 16.0 first.
    p.model = ModelId::S900;
    p.bandwidth = 19.2;
    CHECK(ModelSpec::get(ModelId::S900).modelRateHz(p) == 40000.0);

    // S1000/S1100: fixed 44.1 / 22.05 kHz per sampleRateSel [MAN §3 p.81].
    p.model = ModelId::S1000;
    p.sampleRateSel = SampleRateSel::Fs44100;
    CHECK(ModelSpec::get(ModelId::S1000).modelRateHz(p) == 44100.0);
    p.sampleRateSel = SampleRateSel::Fs22050;
    CHECK(ModelSpec::get(ModelId::S1000).modelRateHz(p) == 22050.0);
    p.model = ModelId::S1100;
    CHECK(ModelSpec::get(ModelId::S1100).modelRateHz(p) == 22050.0);
}

TEST_CASE("modelspec: mono-sum flag set for S900/S950 only", "[modelspec]")
{
    // The S950 is a mono machine [MAN §2]; S900/S950 modes sum to mono
    // (dsp-engine.md §1, §5).
    CHECK(ModelSpec::get(ModelId::S900).monoSum);
    CHECK(ModelSpec::get(ModelId::S950).monoSum);
    CHECK_FALSE(ModelSpec::get(ModelId::S1000).monoSum);
    CHECK_FALSE(ModelSpec::get(ModelId::S1100).monoSum);
}

TEST_CASE("modelspec: character-chain selector per model", "[modelspec]")
{
    // dsp-engine.md §8.1 / §8.2; §8.3 is reserved for v1.1 (ADR-004).
    CHECK(ModelSpec::get(ModelId::S900).chain == CharacterChainKind::VarClock12Bit);
    CHECK(ModelSpec::get(ModelId::S950).chain == CharacterChainKind::VarClock12Bit);
    CHECK(ModelSpec::get(ModelId::S1000).chain
          == CharacterChainKind::FixedRate16Bit);
    CHECK(ModelSpec::get(ModelId::S1100).chain
          == CharacterChainKind::FixedRate16Bit);
    CHECK(ModelSpec::get(ModelId::S3000).chain
          == CharacterChainKind::S3000Reserved);
}
