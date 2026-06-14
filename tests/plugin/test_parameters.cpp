// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 028 — APVTS parameter layout vs the dsp-engine.md §2 table, row by
// row: every ID exists, superset ranges/defaults match, the non-automatable
// set is exact, LCD hardware-unit strings round-trip, and the ParamSnapshot
// bridge reflects parameter changes. Tag: [parameters].

#include <algorithm>
#include <iterator>
#include <type_traits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include "state/Parameters.h"

using Catch::Approx;
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

    juce::RangedAudioParameter& param(const char* id)
    {
        auto* p = apvts.getParameter(id);
        REQUIRE(p != nullptr);
        return *p;
    }

    template <typename T>
    T& paramAs(const char* id)
    {
        auto* p = dynamic_cast<T*>(apvts.getParameter(id));
        REQUIRE(p != nullptr);
        return *p;
    }

    /// Unnormalized default of a parameter.
    float plainDefault(const char* id)
    {
        auto& p = param(id);
        return p.convertFrom0to1(p.getDefaultValue());
    }

    /// LCD text for an unnormalized value.
    juce::String text(const char* id, float plainValue)
    {
        auto& p = param(id);
        return p.getText(p.convertTo0to1(plainValue), 64);
    }

    /// Unnormalized value parsed back from LCD text.
    float plainFromText(const char* id, const juce::String& s)
    {
        auto& p = param(id);
        return p.convertFrom0to1(p.getValueForText(s));
    }

    /// Host-style write of an unnormalized value.
    void set(const char* id, float plainValue)
    {
        auto& p = param(id);
        p.setValueNotifyingHost(p.convertTo0to1(plainValue));
    }
};

constexpr const char* kAllIds[] = {
    pid::model,     pid::pluginMode, pid::timeFactor,    pid::cycleLen,
    pid::stretchMode, pid::hopMode,  pid::transpose,     pid::qual,
    pid::width,     pid::material,   pid::autoCycle,     pid::bandwidth,
    pid::sampleRateSel, pid::character, pid::norm,       pid::tempoSync,
    pid::fxWindow,  pid::outTrim,    pid::embedAudio,
};

} // namespace

TEST_CASE("parameters: every dsp-engine §2 row exists, and nothing else", "[parameters]")
{
    Fixture f;

    for (auto* id : kAllIds)
    {
        INFO("missing parameter id: " << id);
        CHECK(f.apvts.getParameter(id) != nullptr);
    }

    // The 18 §2 table rows + embedAudio (§2 prose non-automatable set).
    CHECK(f.proc.getParameters().size() == 19);
}

TEST_CASE("parameters: float superset ranges, steps and defaults match §2", "[parameters]")
{
    Fixture f;

    SECTION("timeFactor 25.00–2000.00 step 0.01 default 100")
    {
        auto& p = f.paramAs<juce::AudioParameterFloat>(pid::timeFactor);
        CHECK(p.getNormalisableRange().start == Approx(25.0f));
        CHECK(p.getNormalisableRange().end == Approx(2000.0f));
        CHECK(p.getNormalisableRange().interval == Approx(0.01f));
        CHECK(f.plainDefault(pid::timeFactor) == Approx(100.0f));
    }

    SECTION("transpose -24.00–+24.00 step 0.01 default 0")
    {
        auto& p = f.paramAs<juce::AudioParameterFloat>(pid::transpose);
        CHECK(p.getNormalisableRange().start == Approx(-24.0f));
        CHECK(p.getNormalisableRange().end == Approx(24.0f));
        CHECK(p.getNormalisableRange().interval == Approx(0.01f));
        CHECK(f.plainDefault(pid::transpose) == Approx(0.0f).margin(1.0e-4)); // sub-cent snap residue
    }

    SECTION("bandwidth 3.0–19.2 step 0.1 default max (superset; S900 clamps at engine)")
    {
        auto& p = f.paramAs<juce::AudioParameterFloat>(pid::bandwidth);
        CHECK(p.getNormalisableRange().start == Approx(3.0f));
        CHECK(p.getNormalisableRange().end == Approx(19.2f));
        CHECK(p.getNormalisableRange().interval == Approx(0.1f));
        CHECK(f.plainDefault(pid::bandwidth) == Approx(19.2f));
    }

    SECTION("outTrim -24–+12 step 0.1 default 0")
    {
        auto& p = f.paramAs<juce::AudioParameterFloat>(pid::outTrim);
        CHECK(p.getNormalisableRange().start == Approx(-24.0f));
        CHECK(p.getNormalisableRange().end == Approx(12.0f));
        CHECK(p.getNormalisableRange().interval == Approx(0.1f));
        CHECK(f.plainDefault(pid::outTrim) == Approx(0.0f).margin(1.0e-4)); // sub-step snap residue
    }
}

TEST_CASE("parameters: int ranges and defaults match §2", "[parameters]")
{
    Fixture f;

    SECTION("cycleLen 20–2000 default 1000")
    {
        auto& p = f.paramAs<juce::AudioParameterInt>(pid::cycleLen);
        CHECK(p.getRange().getStart() == 20);
        CHECK(p.getRange().getEnd() == 2000);
        CHECK(f.plainDefault(pid::cycleLen) == Approx(1000.0f));
    }

    SECTION("qual 1–99 default 10 (inert/greyed at v1)")
    {
        auto& p = f.paramAs<juce::AudioParameterInt>(pid::qual);
        CHECK(p.getRange().getStart() == 1);
        CHECK(p.getRange().getEnd() == 99);
        CHECK(f.plainDefault(pid::qual) == Approx(10.0f));
    }

    SECTION("width 1–99 default 10 (inert/greyed at v1)")
    {
        auto& p = f.paramAs<juce::AudioParameterInt>(pid::width);
        CHECK(p.getRange().getStart() == 1);
        CHECK(p.getRange().getEnd() == 99);
        CHECK(f.plainDefault(pid::width) == Approx(10.0f));
    }
}

TEST_CASE("parameters: choice lists and defaults match §2 (and core enum order)", "[parameters]")
{
    Fixture f;

    SECTION("model — kAllModels order, default S1000, S3000 slot reserved")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::model);
        CHECK(p.choices == juce::StringArray{ "S900", "S950", "S1000", "S1100", "S3000" });
        CHECK(p.getIndex() == 2);
    }

    SECTION("pluginMode — FX-first")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::pluginMode);
        CHECK(p.choices == juce::StringArray{ "FX", "SAMPLE" });
        CHECK(p.getIndex() == 0);
    }

    SECTION("stretchMode — CYCLIC default, INTELL present but reserved (UI-constrained)")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::stretchMode);
        CHECK(p.choices == juce::StringArray{ "CYCLIC", "INTELL" });
        CHECK(p.getIndex() == 0);
    }

    SECTION("hopMode — CLASSIC default")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::hopMode);
        CHECK(p.choices == juce::StringArray{ "CLASSIC", "REVISED" });
        CHECK(p.getIndex() == 0);
    }

    SECTION("material — POL2 default")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::material);
        CHECK(p.choices == juce::StringArray{ "MON1", "POL2" });
        CHECK(p.getIndex() == 1);
    }

    SECTION("sampleRateSel — 44.1 default")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::sampleRateSel);
        CHECK(p.choices == juce::StringArray{ "44.1kHz", "22.05kHz" });
        CHECK(p.getIndex() == 0);
    }

    SECTION("tempoSync — OFF default")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::tempoSync);
        CHECK(p.choices == juce::StringArray{ "OFF", "HOST" });
        CHECK(p.getIndex() == 0);
    }

    SECTION("fxWindow — FxWindow enum order, default 1 BAR")
    {
        auto& p = f.paramAs<juce::AudioParameterChoice>(pid::fxWindow);
        CHECK(p.choices
              == juce::StringArray{ "1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS",
                                    "4 BARS", "8 BARS", "FREE" });
        CHECK(p.getIndex() == 2); // mws::engine::FxWindow::OneBar
    }
}

TEST_CASE("parameters: bool defaults match §2", "[parameters]")
{
    Fixture f;

    CHECK(f.paramAs<juce::AudioParameterBool>(pid::character).get() == true);
    CHECK(f.paramAs<juce::AudioParameterBool>(pid::norm).get() == false);
    CHECK(f.paramAs<juce::AudioParameterBool>(pid::autoCycle).get() == false);
    CHECK(f.paramAs<juce::AudioParameterBool>(pid::embedAudio).get() == true);
}

TEST_CASE("parameters: non-automatable set is exactly {model, pluginMode, sampleRateSel, embedAudio, bandwidth, stretchMode}", "[parameters]")
{
    Fixture f;

    // bandwidth is non-automatable GLOBALLY (it changes reported latency,
    // dsp-engine §2 bandwidth row / §7.4 — documented deviation from the
    // mode-conditional wording: host parameter info cannot change per mode).
    // stretchMode is non-automatable so no host/automation path can move it
    // off CYCLIC: INTELL is deferred to v1.1 and must be unreachable (ADR-001
    // res.#2 plausible-fake gate; task 054 / QA F1+F3).
    constexpr const char* nonAutomatable[] = {
        pid::model, pid::pluginMode, pid::sampleRateSel, pid::embedAudio,
        pid::bandwidth, pid::stretchMode,
    };

    for (auto* id : kAllIds)
    {
        const bool expectAutomatable =
            std::none_of(std::begin(nonAutomatable), std::end(nonAutomatable),
                         [id](const char* n) { return juce::String(id) == n; });
        INFO("parameter: " << id);
        CHECK(f.param(id).isAutomatable() == expectAutomatable);
    }
}

TEST_CASE("parameters: LCD hardware-unit strings render exactly", "[parameters]")
{
    Fixture f;

    // The five canonical examples from the task/§2 table:
    CHECK(f.text(pid::timeFactor, 300.0f) == "300%");
    CHECK(f.text(pid::cycleLen, 1000.0f) == "1000");
    CHECK(f.text(pid::transpose, 12.0f) == "+12.00 st");
    CHECK(f.text(pid::bandwidth, 19.2f) == "19.2kHz");
    CHECK(f.text(pid::fxWindow, f.param(pid::fxWindow).convertFrom0to1(
                                    f.param(pid::fxWindow).getDefaultValue()))
          == "1 BAR");

    // More representative values:
    CHECK(f.text(pid::timeFactor, 150.5f) == "150.50%");
    CHECK(f.text(pid::transpose, -7.5f) == "-7.50 st");
    CHECK(f.text(pid::transpose, 0.0f) == "0.00 st");
    CHECK(f.text(pid::bandwidth, 3.0f) == "3.0kHz");
    CHECK(f.text(pid::outTrim, 0.0f) == "0.0dB");
    CHECK(f.text(pid::outTrim, 6.0f) == "+6.0dB");
    CHECK(f.text(pid::outTrim, -24.0f) == "-24.0dB");
    CHECK(f.text(pid::character, 1.0f) == "ON");
    CHECK(f.text(pid::norm, 0.0f) == "OFF");
}

TEST_CASE("parameters: hardware-unit strings round-trip back to values", "[parameters]")
{
    Fixture f;

    CHECK(f.plainFromText(pid::timeFactor, "300%") == Approx(300.0f));
    CHECK(f.plainFromText(pid::timeFactor, "150.50%") == Approx(150.5f));
    CHECK(f.plainFromText(pid::transpose, "+12.00 st") == Approx(12.0f));
    CHECK(f.plainFromText(pid::transpose, "-7.50 st") == Approx(-7.5f));
    CHECK(f.plainFromText(pid::bandwidth, "19.2kHz") == Approx(19.2f));
    CHECK(f.plainFromText(pid::bandwidth, "3.0kHz") == Approx(3.0f));
    CHECK(f.plainFromText(pid::outTrim, "+6.0dB") == Approx(6.0f));
    CHECK(f.plainFromText(pid::outTrim, "-24.0dB") == Approx(-24.0f));
    CHECK(f.plainFromText(pid::cycleLen, "1000") == Approx(1000.0f));
    CHECK(f.plainFromText(pid::fxWindow, "1 BAR")
          == Approx(f.param(pid::fxWindow).convertFrom0to1(
              f.param(pid::fxWindow).getDefaultValue())));
}

TEST_CASE("parameters: snapshot defaults equal the ParamSnapshot POD defaults", "[parameters]")
{
    Fixture f;
    mws::plugin::Parameters params(f.apvts);

    const auto s = mws::plugin::makeSnapshot(params);
    const mws::engine::ParamSnapshot d{}; // §2 defaults, byte for byte

    CHECK(s.timeFactor == Approx(d.timeFactor));
    CHECK(s.cycleLen == d.cycleLen);
    CHECK(s.stretchMode == d.stretchMode);
    CHECK(s.hopMode == d.hopMode);
    CHECK(s.transpose == Approx(d.transpose).margin(1.0e-4)); // sub-cent snap residue at 0
    CHECK(s.qual == d.qual);
    CHECK(s.width == d.width);
    CHECK(s.material == d.material);
    CHECK(s.bandwidth == Approx(d.bandwidth));
    CHECK(s.sampleRateSel == d.sampleRateSel);
    CHECK(s.character == d.character);
    CHECK(s.norm == d.norm);
    CHECK(s.tempoSync == d.tempoSync);
    CHECK(s.fxWindow == d.fxWindow);
    CHECK(s.outTrim == Approx(d.outTrim).margin(1.0e-4)); // sub-step snap residue at 0
    CHECK(s.model == d.model);
    CHECK(s.pluginMode == d.pluginMode);
    CHECK(params.autoCyclePending() == false);
}

TEST_CASE("parameters: snapshot reflects host parameter changes", "[parameters]")
{
    Fixture f;
    mws::plugin::Parameters params(f.apvts);

    f.set(pid::timeFactor, 300.0f);
    f.set(pid::cycleLen, 420.0f);
    f.set(pid::transpose, -12.0f);
    f.set(pid::bandwidth, 16.0f);
    f.set(pid::outTrim, -6.0f);
    f.set(pid::qual, 20.0f);
    f.set(pid::width, 33.0f);
    f.paramAs<juce::AudioParameterChoice>(pid::model) = 1;       // S950
    f.paramAs<juce::AudioParameterChoice>(pid::pluginMode) = 1;  // SAMPLE
    f.paramAs<juce::AudioParameterChoice>(pid::stretchMode) = 1; // INTELL (reserved) — must be coerced
    f.paramAs<juce::AudioParameterChoice>(pid::hopMode) = 1;     // REVISED
    f.paramAs<juce::AudioParameterChoice>(pid::material) = 0;    // MON1
    f.paramAs<juce::AudioParameterChoice>(pid::sampleRateSel) = 1; // 22.05
    f.paramAs<juce::AudioParameterChoice>(pid::tempoSync) = 1;   // HOST
    f.paramAs<juce::AudioParameterChoice>(pid::fxWindow) = 6;    // FREE
    f.paramAs<juce::AudioParameterBool>(pid::character) = false;
    f.paramAs<juce::AudioParameterBool>(pid::norm) = true;
    f.paramAs<juce::AudioParameterBool>(pid::autoCycle) = true;

    const auto s = params.makeSnapshot();
    using namespace mws::engine;

    CHECK(s.timeFactor == Approx(300.0));
    CHECK(s.cycleLen == 420);
    CHECK(s.transpose == Approx(-12.0));
    CHECK(s.bandwidth == Approx(16.0));
    CHECK(s.outTrim == Approx(-6.0));
    CHECK(s.qual == 20);
    CHECK(s.width == 33);
    CHECK(s.model == mws::model::ModelId::S950);
    CHECK(s.pluginMode == PluginMode::Sample);
    // INTELL is deferred to v1.1 and genuinely unreachable at v1: even when the
    // raw parameter is forced to INTELL (e.g. a stale preset/automation value),
    // the snapshot coerces it to CYCLIC so the engine never renders a guess
    // under the authentic name (ADR-001 res.#2; task 054 / QA F1).
    CHECK(s.stretchMode == StretchMode::Cyclic);
    CHECK(s.hopMode == HopMode::Revised);
    CHECK(s.material == Material::Mon1);
    CHECK(s.sampleRateSel == SampleRateSel::Fs22050);
    CHECK(s.tempoSync == TempoSync::Host);
    CHECK(s.fxWindow == FxWindow::Free);
    CHECK(s.character == false);
    CHECK(s.norm == true);
    CHECK(params.autoCyclePending() == true);

    // The snapshot type itself stays audio-thread safe (plain copy).
    static_assert(std::is_trivially_copyable_v<mws::engine::ParamSnapshot>);
}

TEST_CASE("parameters: INTELL is unreachable — the engine snapshot is always CYCLIC at v1",
          "[parameters]")
{
    // INTELL is deferred to v1.1 and must be genuinely unreachable at v1: the
    // LCD field is greyed, the parameter is non-automatable, AND makeSnapshot()
    // coerces it. This proves the last line of defense: no jog/host/automation/
    // stale-preset value can hand the engine an INTELL snapshot, so the engine
    // can never render a guess as silently-CYCLIC audio under the authentic name
    // (ADR-001 res.#2 plausible-fake gate; task 054 / QA F1+F3).
    using namespace mws::engine;
    Fixture f;
    mws::plugin::Parameters params(f.apvts);

    // The default snapshot is CYCLIC...
    CHECK(params.makeSnapshot().stretchMode == StretchMode::Cyclic);

    // ...and even forcing the raw parameter to INTELL (the host-cached choice
    // value an old session/preset could carry) still yields a CYCLIC snapshot.
    f.paramAs<juce::AudioParameterChoice>(pid::stretchMode) = 1; // INTELL
    REQUIRE(f.paramAs<juce::AudioParameterChoice>(pid::stretchMode).getIndex() == 1);
    CHECK(params.makeSnapshot().stretchMode == StretchMode::Cyclic);
}
