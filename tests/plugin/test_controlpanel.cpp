// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 044 — ModelSelector + ControlPanel: attachment round-trips (UI change
// ⇒ parameter change ⇒ UI reflects), per-model BANDWIDTH/FS row visibility,
// the reserved blank disabled fifth badge (ADR-004), WINDOW enablement in FX
// mode only, and the ui-design §7 tooltips. Tag: [controlpanel] — test-case
// names begin with the tag word (plan/backlog/README.md test-selection rules).
//
// UI clicks are simulated with setToggleState(..., sendNotificationSync) —
// Button::triggerClick is timer-deferred, but the sync click message drives
// the exact same Listener/onClick path the mouse does, synchronously on the
// message thread (which the test thread is, via ScopedJuceInitialiser_GUI).

#include <iostream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include "state/Parameters.h"
#include "ui/ControlPanel.h"
#include "ui/ModelSelector.h"
#include "ui/lookandfeel/SeriesLookAndFeel.h"

#include "mws/model/ModelSpec.h"

using Catch::Approx;
using mws::model::ModelId;
namespace pid = mws::plugin::paramid;

namespace {

/// Minimal host-side AudioProcessor so the APVTS has an owner (same pattern
/// as test_parameters.cpp).
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
    mws::ui::ControlPanel panel{ apvts };

    Fixture() { panel.setSize(290, 230); }  // §1 region-4 frame at 1.0 scale

    juce::RangedAudioParameter& param(const char* id)
    {
        auto* p = apvts.getParameter(id);
        REQUIRE(p != nullptr);
        return *p;
    }

    /// Sets a parameter from the "host" side (normalized), as automation or
    /// preset restore would.
    void setFromHost(const char* id, float denormalized)
    {
        auto& p = param(id);
        p.setValueNotifyingHost(p.convertTo0to1(denormalized));
    }

    /// The parameter's current value in hardware units (denormalized).
    float hostValue(const char* id)
    {
        auto& p = param(id);
        return p.convertFrom0to1(p.getValue());
    }

    /// Simulates a user click latching a radio/toggle button on.
    static void clickOn(juce::Button& button)
    {
        button.setToggleState(true, juce::sendNotificationSync);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// ModelSelector — badges, reserved fifth slot, model parameter writes
// ---------------------------------------------------------------------------

TEST_CASE("controlpanel: fifth badge present, blank, disabled (ADR-004 "
          "reserved slot)",
          "[controlpanel]")
{
    Fixture f;
    auto& selector = f.panel.modelSelector();

    STATIC_REQUIRE(mws::ui::ModelSelector::badgeCount() == 5);

    // Four shipping badges: enabled, labelled by model.
    CHECK(selector.badge(0).getButtonText() == "S900");
    CHECK(selector.badge(1).getButtonText() == "S950");
    CHECK(selector.badge(2).getButtonText() == "S1000");
    CHECK(selector.badge(3).getButtonText() == "S1100");
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(selector.badge(i).isEnabled());

    // The reserved S3000 slot: present, BLANK, disabled.
    auto& reserved = selector.badge(4);
    CHECK(reserved.getButtonText().isEmpty());
    CHECK_FALSE(reserved.isEnabled());
}

TEST_CASE("controlpanel: model selector defaults to S1000 and latches exactly "
          "one badge",
          "[controlpanel]")
{
    Fixture f;
    auto& selector = f.panel.modelSelector();

    CHECK(selector.selectedModel() == ModelId::S1000);

    int latched = 0;
    for (std::size_t i = 0; i < mws::ui::ModelSelector::badgeCount(); ++i)
        latched += selector.badge(i).getToggleState() ? 1 : 0;
    CHECK(latched == 1);
    CHECK(selector.badge(2).getToggleState());  // S1000
}

TEST_CASE("controlpanel: clicking a badge writes the non-automatable model "
          "parameter; host change re-latches the badges",
          "[controlpanel]")
{
    Fixture f;
    auto& selector = f.panel.modelSelector();

    // UI ⇒ param.
    Fixture::clickOn(selector.badge(1));  // S950
    CHECK(f.hostValue(pid::model) == Approx(1.0f));
    CHECK(selector.selectedModel() == ModelId::S950);

    // Param ⇒ UI.
    f.setFromHost(pid::model, 3.0f);  // S1100
    CHECK(selector.badge(3).getToggleState());
    CHECK_FALSE(selector.badge(1).getToggleState());
    CHECK(selector.selectedModel() == ModelId::S1100);
}

// ---------------------------------------------------------------------------
// Attachment round-trips — UI change ⇒ param change ⇒ UI reflects
// ---------------------------------------------------------------------------

TEST_CASE("controlpanel: MODE radio round-trips the pluginMode parameter",
          "[controlpanel]")
{
    Fixture f;

    // Default FX (locked, FX-first).
    CHECK(f.panel.modeFxButton().getToggleState());

    Fixture::clickOn(f.panel.modeSampleButton());
    CHECK(f.hostValue(pid::pluginMode) == Approx(1.0f));
    CHECK(f.panel.modeSampleButton().getToggleState());
    CHECK_FALSE(f.panel.modeFxButton().getToggleState());

    f.setFromHost(pid::pluginMode, 0.0f);  // back to FX from the host side
    CHECK(f.panel.modeFxButton().getToggleState());
    CHECK_FALSE(f.panel.modeSampleButton().getToggleState());
}

TEST_CASE("controlpanel: TIMING radio round-trips the hopMode parameter",
          "[controlpanel]")
{
    Fixture f;

    CHECK(f.panel.timingClassicButton().getToggleState());  // CLASSIC default

    Fixture::clickOn(f.panel.timingRevisedButton());
    CHECK(f.hostValue(pid::hopMode) == Approx(1.0f));

    f.setFromHost(pid::hopMode, 0.0f);
    CHECK(f.panel.timingClassicButton().getToggleState());
    CHECK_FALSE(f.panel.timingRevisedButton().getToggleState());
}

TEST_CASE("controlpanel: CHARACTER toggle round-trips and shows ON/OFF cap text",
          "[controlpanel]")
{
    Fixture f;

    CHECK(f.panel.characterButton().getToggleState());  // default ON
    CHECK(f.panel.characterButton().getButtonText() == "ON");

    f.panel.characterButton().setToggleState(false, juce::sendNotificationSync);
    CHECK(f.hostValue(pid::character) == Approx(0.0f));
    CHECK(f.panel.characterButton().getButtonText() == "OFF");

    f.setFromHost(pid::character, 1.0f);
    CHECK(f.panel.characterButton().getToggleState());
    CHECK(f.panel.characterButton().getButtonText() == "ON");
}

TEST_CASE("controlpanel: WINDOW combo round-trips the fxWindow parameter",
          "[controlpanel]")
{
    Fixture f;
    auto& box = f.panel.windowSelector();

    REQUIRE(box.getNumItems() == 7);  // 1/4..8 bars + FREE (dsp-engine §2)
    CHECK(box.getSelectedItemIndex() == 2);  // default 1 BAR
    CHECK(box.getItemText(6) == "FREE");

    box.setSelectedItemIndex(6, juce::sendNotificationSync);  // FREE
    CHECK(f.hostValue(pid::fxWindow) == Approx(6.0f));

    f.setFromHost(pid::fxWindow, 0.0f);  // 1/4 BAR from the host side
    CHECK(box.getSelectedItemIndex() == 0);
}

TEST_CASE("controlpanel: OUTPUT rotary round-trips the outTrim parameter over "
          "the -24..+12 dB superset",
          "[controlpanel]")
{
    Fixture f;
    auto& knob = f.panel.outputKnob();

    CHECK(knob.getMinimum() == Approx(-24.0));
    CHECK(knob.getMaximum() == Approx(12.0));
    CHECK(knob.getValue() == Approx(0.0).margin(1.0e-4));  // float-range epsilon

    knob.setValue(-6.0, juce::sendNotificationSync);
    CHECK(f.hostValue(pid::outTrim) == Approx(-6.0f));

    f.setFromHost(pid::outTrim, 12.0f);
    CHECK(knob.getValue() == Approx(12.0));
}

TEST_CASE("controlpanel: BANDWIDTH slider round-trips the non-automatable "
          "bandwidth parameter",
          "[controlpanel]")
{
    Fixture f;
    auto& slider = f.panel.bandwidthSlider();

    CHECK(slider.getMinimum() == Approx(3.0));
    CHECK(slider.getMaximum() == Approx(19.2));
    CHECK(slider.getValue() == Approx(19.2));  // default max

    slider.setValue(10.0, juce::sendNotificationSync);
    CHECK(f.hostValue(pid::bandwidth) == Approx(10.0f));

    f.setFromHost(pid::bandwidth, 16.0f);
    CHECK(slider.getValue() == Approx(16.0));
}

TEST_CASE("controlpanel: FS radio round-trips the non-automatable "
          "sampleRateSel parameter",
          "[controlpanel]")
{
    Fixture f;

    CHECK(f.panel.fs441Button().getToggleState());  // 44.1 default
    CHECK(f.panel.fs441Button().getButtonText() == "44.1kHz");
    CHECK(f.panel.fs2205Button().getButtonText() == "22.05kHz");

    Fixture::clickOn(f.panel.fs2205Button());
    CHECK(f.hostValue(pid::sampleRateSel) == Approx(1.0f));

    f.setFromHost(pid::sampleRateSel, 0.0f);
    CHECK(f.panel.fs441Button().getToggleState());
    CHECK_FALSE(f.panel.fs2205Button().getToggleState());
}

// ---------------------------------------------------------------------------
// Per-model visibility of the BANDWIDTH / FS context rows
// ---------------------------------------------------------------------------

TEST_CASE("controlpanel: BANDWIDTH row shows on S900/S950 only, FS row on "
          "S1000/S1100 only (dsp-engine §2 applies-to)",
          "[controlpanel]")
{
    Fixture f;

    // Default S1000: fixed-rate machine — FS visible, BANDWIDTH hidden.
    CHECK(f.panel.fsRowVisible());
    CHECK_FALSE(f.panel.bandwidthRowVisible());

    // Varclock models: BANDWIDTH visible, FS hidden.
    for (const float index : { 0.0f /* S900 */, 1.0f /* S950 */ })
    {
        f.setFromHost(pid::model, index);
        CHECK(f.panel.bandwidthRowVisible());
        CHECK_FALSE(f.panel.fsRowVisible());
    }

    // Fixed-rate models: FS visible, BANDWIDTH hidden.
    for (const float index : { 2.0f /* S1000 */, 3.0f /* S1100 */ })
    {
        f.setFromHost(pid::model, index);
        CHECK(f.panel.fsRowVisible());
        CHECK_FALSE(f.panel.bandwidthRowVisible());
    }
}

// ---------------------------------------------------------------------------
// WINDOW enablement — FX mode only (ui-design §1 "WINDOW [1 BAR] (FX)")
// ---------------------------------------------------------------------------

TEST_CASE("controlpanel: WINDOW selector enabled only in FX mode",
          "[controlpanel]")
{
    Fixture f;

    CHECK(f.panel.windowSelector().isEnabled());  // default mode is FX

    f.setFromHost(pid::pluginMode, 1.0f);  // SAMPLE
    CHECK_FALSE(f.panel.windowSelector().isEnabled());

    f.setFromHost(pid::pluginMode, 0.0f);  // FX
    CHECK(f.panel.windowSelector().isEnabled());
}

// ---------------------------------------------------------------------------
// Tooltips (ui-design §7: hardware unit + host-normalized value) and
// accessibility names
// ---------------------------------------------------------------------------

TEST_CASE("controlpanel: tooltips show the hardware-unit string and the "
          "host-normalized value",
          "[controlpanel]")
{
    Fixture f;

    f.setFromHost(pid::outTrim, 6.0f);

    const auto tip = f.panel.outputKnob().getTooltip();
    CHECK(tip.contains("+6.0dB"));  // LCD hardware-unit string (task 028)
    const auto norm = f.param(pid::outTrim).getValue();
    CHECK(tip.contains(juce::String(norm, 3)));  // host-normalized 0..1

    CHECK(f.panel.bandwidthSlider().getTooltip().contains("kHz"));
    CHECK(f.panel.characterButton().getTooltip().contains("ON"));
}

TEST_CASE("controlpanel: accessibility names set on every control",
          "[controlpanel]")
{
    Fixture f;

    CHECK(f.panel.modelSelector().badge(0).getTitle() == "S900");
    CHECK(f.panel.modelSelector().badge(4).getTitle() == "RESERVED");
    CHECK(f.panel.characterButton().getTitle() == "CHARACTER");
    CHECK(f.panel.windowSelector().getTitle() == "WINDOW");
    CHECK(f.panel.bandwidthSlider().getTitle() == "BANDWIDTH");
    CHECK(f.panel.outputKnob().getTitle() == "OUTPUT");
    CHECK(f.panel.modeFxButton().getTitle() == "FX");
    CHECK(f.panel.timingRevisedButton().getTitle() == "REVISED");
}

// ---------------------------------------------------------------------------
// Visual-review snapshots (env-gated; never part of a normal test run's
// outputs — same convention as test_faceplatespec.cpp). Set
// MWS_FACEPLATE_SNAPSHOT_DIR to dump a per-model PNG of the panel styled by
// SeriesLookAndFeel; the repo itself stays free of raster assets (ADR-005).
// ---------------------------------------------------------------------------

TEST_CASE("controlpanel: snapshot dump for visual review when "
          "MWS_FACEPLATE_SNAPSHOT_DIR is set",
          "[controlpanel]")
{
    const auto dir =
        juce::SystemStats::getEnvironmentVariable("MWS_FACEPLATE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
    {
        SUCCEED("MWS_FACEPLATE_SNAPSHOT_DIR not set — snapshot dump skipped");
        return;
    }

    Fixture f;
    mws::ui::SeriesLookAndFeel lnf;

    const auto outDir = juce::File(dir);
    REQUIRE(outDir.createDirectory().wasOk());

    for (const auto id : mws::model::kAllModels)
    {
        if (!mws::model::ModelSpec::isShipping(id))
            continue;  // S3000 badge is reserved/disabled — unselectable

        f.setFromHost(pid::model,
                      static_cast<float>(static_cast<int>(id)));
        lnf.setSpec(mws::ui::faceplateSpecFor(id));
        f.panel.setLookAndFeel(&lnf);
        f.panel.setSize(290 * 2, 230 * 2);  // 2x for legibility

        const auto snapshot =
            f.panel.createComponentSnapshot(f.panel.getLocalBounds(), false, 1.0f);
        REQUIRE_FALSE(snapshot.isNull());

        const auto file = outDir.getChildFile(
            "controlpanel_" + juce::String(mws::model::toString(id).data()) + ".png");
        file.deleteFile();
        juce::FileOutputStream stream(file);
        REQUIRE(stream.openedOk());
        juce::PNGImageFormat png;
        REQUIRE(png.writeImageToStream(snapshot, stream));
        std::cout << "controlpanel snapshot: " << file.getFullPathName() << "\n";
    }

    f.panel.setLookAndFeel(nullptr);
}
