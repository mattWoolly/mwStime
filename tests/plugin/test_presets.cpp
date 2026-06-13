// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 038 — the four documented dsp-engine.md §9 factory presets ("Jungle
// Amen 300", "S950 vocal 200", "S900 half-speed", "Dred vox"). Three gates:
//   1. each preset's resulting ParamSnapshot equals the §9 values per field,
//   2. each preset's params match the corresponding tests/golden/cases.json
//      case entry (read in-test via the canonical loader — drift FAILS the
//      build, so presets and goldens can never silently diverge),
//   3. the JUCE program API round-trips through getState/setState (select a
//      program → serialize → restore → the program/params come back).
// Tag: [presets].

#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include "state/FactoryPresets.h"
#include "state/Parameters.h"
#include "state/StateTree.h"

// The canonical cases.json loader (tools/mwstime-render) — JUCE-free, depends
// only on mws/engine/Params.h. Reusing it means the cross-check shares ONE
// parameter vocabulary with the golden renderer.
#include "Cases.h"

using Catch::Approx;
namespace fp = mws::plugin::presets;
namespace st = mws::plugin::state;
namespace pid = mws::plugin::paramid;

namespace {

/// Minimal host-side AudioProcessor so the APVTS has an owner.
struct NullProcessor final : juce::AudioProcessor
{
    NullProcessor() = default;

    const juce::String getName() const override { return "null"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    NullProcessor proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };
    juce::ValueTree stateTree = st::createDefault();

    /// The snapshot the engine/audio thread would see from the current APVTS.
    [[nodiscard]] mws::engine::ParamSnapshot snapshot()
    {
        return mws::plugin::Parameters(apvts).makeSnapshot();
    }
};

/// Reads tests/golden/cases.json from disk (the path is a compile def supplied
/// by tests/plugin/CMakeLists.txt) — the in-test cross-check requirement.
std::string readCasesJson()
{
    const juce::File f{ MWS_CASES_JSON_PATH };
    REQUIRE(f.existsAsFile());
    return f.loadFileAsString().toStdString();
}

/// Field-by-field assert that a preset snapshot equals a §9 reference snapshot.
/// Compares EVERY ParamSnapshot field so a future stray default change is
/// caught, not just the deltas the preset deliberately sets.
///
/// The float fields carry a small absolute margin: when a snapshot has gone
/// through the APVTS (preset apply / state round-trip), the host-normalized
/// 0..1 float storage leaves sub-1e-6 residue on values like 0.0 — comparing
/// with a bare Approx(0.0) would spuriously fail. The hardware-unit step on
/// every float param is ≥ 0.01, so a 1e-4 margin never masks a real diff.
void requireSnapshotsEqual(const mws::engine::ParamSnapshot& got,
                           const mws::engine::ParamSnapshot& want)
{
    constexpr double kFloatMargin = 1.0e-4;
    CHECK(got.timeFactor == Approx(want.timeFactor).margin(kFloatMargin));
    CHECK(got.cycleLen == want.cycleLen);
    CHECK(got.stretchMode == want.stretchMode);
    CHECK(got.hopMode == want.hopMode);
    CHECK(got.transpose == Approx(want.transpose).margin(kFloatMargin));
    CHECK(got.qual == want.qual);
    CHECK(got.width == want.width);
    CHECK(got.material == want.material);
    CHECK(got.bandwidth == Approx(want.bandwidth).margin(kFloatMargin));
    CHECK(got.sampleRateSel == want.sampleRateSel);
    CHECK(got.character == want.character);
    CHECK(got.norm == want.norm);
    CHECK(got.tempoSync == want.tempoSync);
    CHECK(got.fxWindow == want.fxWindow);
    CHECK(got.outTrim == Approx(want.outTrim).margin(kFloatMargin));
    CHECK(got.model == want.model);
    // pluginMode is asserted separately by the mode-specific tests.
}

} // namespace

// ---------------------------------------------------------------------------
// 1. §9 verbatim — each preset's snapshot, field by field.
// ---------------------------------------------------------------------------

TEST_CASE("presets: 'Jungle Amen 300' matches dsp-engine §9 verbatim", "[presets]")
{
    using namespace mws::engine;
    const auto s = fp::snapshotFor("Jungle Amen 300");

    // S1100 CYCLIC, Cycle 1000, Time 300%, qual 20 / width 10 stored-but-inert.
    CHECK(s.model == mws::model::ModelId::S1100);
    CHECK(s.stretchMode == StretchMode::Cyclic);
    CHECK(s.cycleLen == 1000);
    CHECK(s.timeFactor == Approx(300.0));
    CHECK(s.qual == 20);   // stored but inert in CYCLIC (§9)
    CHECK(s.width == 10);  // stored but inert in CYCLIC (§9)
    CHECK(s.hopMode == HopMode::Classic);
}

TEST_CASE("presets: 'S950 vocal 200' matches dsp-engine §9 verbatim", "[presets]")
{
    using namespace mws::engine;
    const auto s = fp::snapshotFor("S950 vocal 200");

    // S950 STRETCH 200%, D-TIME 1000, POL2.
    CHECK(s.model == mws::model::ModelId::S950);
    CHECK(s.timeFactor == Approx(200.0));
    CHECK(s.cycleLen == 1000); // D-TIME ≡ cycle length in samples (§2/§5)
    CHECK(s.material == Material::Pol2);
    CHECK(s.hopMode == HopMode::Classic);
}

TEST_CASE("presets: 'S900 half-speed' matches dsp-engine §9 verbatim", "[presets]")
{
    using namespace mws::engine;
    const auto s = fp::snapshotFor("S900 half-speed");

    // S900 timeFactor 200% (varispeed; -12 st emerges from the engine), BW max
    // (16.0 kHz — the S900 ceiling, §2 / cases.json).
    CHECK(s.model == mws::model::ModelId::S900);
    CHECK(s.timeFactor == Approx(200.0));
    CHECK(s.bandwidth == Approx(16.0));
    CHECK(s.transpose == Approx(0.0)); // varispeed: pitch is a side-effect, not transpose
}

TEST_CASE("presets: 'Dred vox' matches dsp-engine §9 verbatim", "[presets]")
{
    using namespace mws::engine;
    const auto s = fp::snapshotFor("Dred vox");

    // S1000 CYCLIC, Cycle 200, Time 400% (PI extremes).
    CHECK(s.model == mws::model::ModelId::S1000);
    CHECK(s.stretchMode == StretchMode::Cyclic);
    CHECK(s.cycleLen == 200);
    CHECK(s.timeFactor == Approx(400.0));
    CHECK(s.hopMode == HopMode::Classic);
}

TEST_CASE("presets: the four §9 presets exist in §9 order; S3000 is NOT shipped", "[presets]")
{
    REQUIRE(fp::numPresets() == 4);
    CHECK(fp::presetName(0) == "Jungle Amen 300");
    CHECK(fp::presetName(1) == "S950 vocal 200");
    CHECK(fp::presetName(2) == "S900 half-speed");
    CHECK(fp::presetName(3) == "Dred vox");

    // S3000 "factory default" is v1.1 (ADR-004) — must not appear at v1.
    for (int i = 0; i < fp::numPresets(); ++i)
        CHECK_FALSE(fp::presetName(i).containsIgnoreCase("S3000"));
}

// ---------------------------------------------------------------------------
// 2. cases.json cross-check — presets must equal their golden case entries.
// ---------------------------------------------------------------------------

TEST_CASE("presets: every preset equals its tests/golden/cases.json entry", "[presets]")
{
    const std::string casesJson = readCasesJson();

    struct Pair { const char* preset; const char* caseId; };
    const Pair pairs[] = {
        { "Jungle Amen 300", "s1100_jungle_amen_300" },
        { "S950 vocal 200",  "s950_vocal_200" },
        { "S900 half-speed", "s900_half_speed" },
        { "Dred vox",        "s1000_dred_vox" },
    };

    for (const auto& [preset, caseId] : pairs)
    {
        const auto loaded = mwsrender::loadCase(casesJson, caseId);
        INFO("loading golden case '" << caseId << "': " << loaded.error);
        REQUIRE(loaded.ok());

        // The golden loader overlays case keys on the §2 defaults — the same
        // basis FactoryPresets builds on — so the snapshots must be identical.
        INFO("preset '" << preset << "' vs golden case '" << caseId << "'");
        requireSnapshotsEqual(fp::snapshotFor(preset), loaded.def.params);
    }
}

// ---------------------------------------------------------------------------
// 3. preset selection writes APVTS + non-param state; program API round-trip.
// ---------------------------------------------------------------------------

TEST_CASE("presets: applying a preset writes the snapshot through the APVTS", "[presets]")
{
    Fixture f;

    // Program 0 is "Default"; programs 1..N are the §9 presets (hosts expect
    // ≥ 1 program and a sensible 0). Apply "Dred vox".
    const int dredProgram = fp::programForPreset(3); // 1-based program index
    fp::applyProgram(dredProgram, f.apvts, f.stateTree);

    const auto s = f.snapshot();
    const auto want = fp::snapshotFor("Dred vox");
    requireSnapshotsEqual(s, want);
}

TEST_CASE("presets: program 0 is 'Default' and applies the §2 defaults", "[presets]")
{
    Fixture f;
    CHECK(fp::numPrograms() == 1 + fp::numPresets()); // Default + the four §9 presets
    CHECK(fp::programName(0) == "Default");

    // Dirty the state, then reset to Default → the §2 default snapshot returns.
    fp::applyProgram(fp::programForPreset(0), f.apvts, f.stateTree); // Jungle Amen
    fp::applyProgram(0, f.apvts, f.stateTree);                       // Default

    requireSnapshotsEqual(f.snapshot(), mws::engine::ParamSnapshot{});
}

TEST_CASE("presets: program names cover Default + the four §9 presets", "[presets]")
{
    CHECK(fp::programName(0) == "Default");
    CHECK(fp::programName(1) == "Jungle Amen 300");
    CHECK(fp::programName(2) == "S950 vocal 200");
    CHECK(fp::programName(3) == "S900 half-speed");
    CHECK(fp::programName(4) == "Dred vox");
}

TEST_CASE("presets: program API round-trips through getState/setState", "[presets]")
{
    // Select a program → serialize the FULL plugin state → restore into a fresh
    // APVTS → the program's parameters come back (architecture.md §6 state model).
    Fixture src;
    const int program = fp::programForPreset(0); // "Jungle Amen 300"
    fp::applyProgram(program, src.apvts, src.stateTree);

    juce::MemoryBlock blob;
    st::writePluginState(src.apvts.copyState(), src.stateTree, blob);
    REQUIRE(blob.getSize() > 0);

    Fixture dst;
    const auto restored = st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);
    REQUIRE(restored.apvtsState.isValid());
    dst.apvts.replaceState(restored.apvtsState);
    dst.stateTree = restored.stateTree;

    // The restored snapshot reproduces the preset exactly.
    requireSnapshotsEqual(dst.snapshot(), fp::snapshotFor("Jungle Amen 300"));
}

TEST_CASE("presets: preset selection updates APVTS and non-param mode atomically", "[presets]")
{
    Fixture f;

    // The §9 presets are authentic SAMPLE-mode (offline-render) settings: the
    // hardware timestretch operates on a loaded sample. Applying a preset must
    // set BOTH the pluginMode APVTS param AND the mirrored state-tree `mode`
    // property — never a partial state where they disagree.
    fp::applyProgram(fp::programForPreset(1), f.apvts, f.stateTree); // "S950 vocal 200"

    const auto s = f.snapshot();
    CHECK(s.pluginMode == mws::engine::PluginMode::Sample);
    CHECK(f.stateTree.getProperty(st::id::mode).toString() == "SAMPLE");
}
