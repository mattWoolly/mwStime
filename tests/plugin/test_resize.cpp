// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 047 — resizable editor (docs/design/ui-design.md §5; architecture.md §6
// UI state: scale factor in the state tree; ADR-005 resolution independence).
// Covered, all headless:
//   · the editor's ComponentBoundsConstrainer (configured exactly as
//     PluginEditor configures it) locks the fixed 1000/380 aspect and enforces
//     the 600×228 / 2000×760 (0.6×–2.0×) limits;
//   · the scale factor round-trips through writePluginState / readPluginState
//     (host session save → reload restores it — acceptance criterion);
//   · the scale↔size helpers map the documented 75/100/150/200% menu entries to
//     the right pixel sizes, and a reopened editor reads the persisted scale
//     back to the same size (restore-on-open).
//
// Test-case names begin with the tag word so `ctest -R resize` matches
// (plan/backlog/README.md test-selection rules). The size/scale math and the
// state read/write are pure, so this runs without a plugin window (a
// ScopedJuceInitialiser_GUI is still needed for the constrainer + ValueTree
// infrastructure).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

#include "state/Parameters.h"
#include "state/StateTree.h"
#include "ui/ControlPanel.h"
#include "ui/EditorResize.h"
#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/JogWheel.h"
#include "ui/LcdDisplay.h"
#include "ui/LcdPageBinding.h"
#include "ui/LcdPageModel.h"
#include "ui/SoftKeyBar.h"
#include "ui/WaveformView.h"
#include "ui/lookandfeel/SeriesLookAndFeel.h"

#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

using Catch::Approx;
namespace rz = mws::ui::resize;
namespace geo = mws::ui::geometry;
namespace st = mws::plugin::state;

namespace {

/// Minimal host-side AudioProcessor so the APVTS has an owner (same pattern as
/// test_state.cpp / test_editor_wiring.cpp).
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

} // namespace

// ---------------------------------------------------------------------------
// Constrainer: fixed aspect + min/max limits (ui-design §5, scope bullet 1)
// ---------------------------------------------------------------------------

TEST_CASE("resize: the constrainer locks the 1000/380 aspect ratio", "[resize]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    CHECK(rz::kAspectRatio == Approx(1000.0 / 380.0));
    CHECK(rz::kMinWidth == 600);
    CHECK(rz::kMinHeight == 228);
    CHECK(rz::kMaxWidth == 2000);
    CHECK(rz::kMaxHeight == 760);

    juce::ComponentBoundsConstrainer c;
    rz::configureConstrainer(c);

    // Drag toward a deliberately wrong (squashed) shape: the constrainer pulls
    // it back onto the fixed aspect. Start from the base size, request a much
    // wider-than-tall rectangle.
    const juce::Rectangle<int> previous(0, 0, geo::kBaseWidth, geo::kBaseHeight);
    const juce::Rectangle<int> limits(0, 0, 4000, 4000);

    juce::Rectangle<int> distorted(0, 0, 1500, 380);  // 3.95:1 — way off aspect
    c.checkBounds(distorted, previous, limits,
                  /*isStretchingTop*/ false, /*Left*/ false,
                  /*Bottom*/ true, /*Right*/ true);

    // After the constraint the width:height ratio is the fixed aspect.
    CHECK((double) distorted.getWidth() / (double) distorted.getHeight()
          == Approx(rz::kAspectRatio).margin(0.02));
}

TEST_CASE("resize: the constrainer enforces the 0.6×–2.0× size limits", "[resize]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::ComponentBoundsConstrainer c;
    rz::configureConstrainer(c);

    CHECK(c.getMinimumWidth() == rz::kMinWidth);
    CHECK(c.getMinimumHeight() == rz::kMinHeight);
    CHECK(c.getMaximumWidth() == rz::kMaxWidth);
    CHECK(c.getMaximumHeight() == rz::kMaxHeight);

    const juce::Rectangle<int> previous(0, 0, geo::kBaseWidth, geo::kBaseHeight);
    const juce::Rectangle<int> limits(0, 0, 8000, 8000);

    // Below the floor: clamps up to (at least) the 0.6× minimum, aspect kept.
    juce::Rectangle<int> tooSmall(0, 0, 100, 38);
    c.checkBounds(tooSmall, previous, limits, false, false, true, true);
    CHECK(tooSmall.getWidth() >= rz::kMinWidth);
    CHECK(tooSmall.getHeight() >= rz::kMinHeight);

    // Above the ceiling: clamps down to (at most) the 2.0× maximum, aspect kept.
    juce::Rectangle<int> tooBig(0, 0, 6000, 2280);
    c.checkBounds(tooBig, previous, limits, false, false, true, true);
    CHECK(tooBig.getWidth() <= rz::kMaxWidth);
    CHECK(tooBig.getHeight() <= rz::kMaxHeight);
}

// ---------------------------------------------------------------------------
// scale ↔ size helpers (the 75/100/150/200% hamburger menu entries, §1 r1)
// ---------------------------------------------------------------------------

TEST_CASE("resize: scale↔size maps the documented menu scales to base multiples",
          "[resize]")
{
    struct Case { double scale; int w; int h; };
    const Case cases[] = {
        { 0.6, 600, 228 },   // floor
        { 0.75, 750, 285 },  // 75% menu entry
        { 1.0, 1000, 380 },  // 100% — the base canvas
        { 1.5, 1500, 570 },  // 150% menu entry
        { 2.0, 2000, 760 },  // 200% — ceiling
    };
    for (const auto& tc : cases)
    {
        CHECK(rz::widthForScale(tc.scale) == tc.w);
        CHECK(rz::heightForScale(tc.scale) == tc.h);
        CHECK(rz::scaleForWidth(tc.w) == Approx(tc.scale));
    }

    // Out-of-range requests clamp to the documented limits.
    CHECK(rz::clampScale(0.1) == Approx(rz::kMinScale));
    CHECK(rz::clampScale(5.0) == Approx(rz::kMaxScale));
    CHECK(rz::clampScale(1.25) == Approx(1.25));
}

// ---------------------------------------------------------------------------
// Scale survives a host session save/reload (acceptance criterion 2):
// the editor writes uiState/scaleFactor on resize-end, and a fresh session
// (a new processor restoring the saved blob) reads the same scale back, so a
// reopened editor sizes itself to that scale.
// ---------------------------------------------------------------------------

TEST_CASE("resize: the scale factor round-trips through getState/setState", "[resize]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    NullProcessor proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };

    // The editor persists the scale exactly as PluginEditor::persistScale does:
    // clamp, then write into uiState/scaleFactor on the non-parameter tree.
    auto tree = st::createDefault();
    const double dialledScale = rz::scaleForWidth(rz::widthForScale(1.5));  // 150%
    {
        auto ui = tree.getChildWithName(st::id::uiState);
        ui.setProperty(st::id::scaleFactor, rz::clampScale(dialledScale), nullptr);
    }

    // Host saves the session.
    juce::MemoryBlock blob;
    st::writePluginState(apvts.copyState(), tree, blob);
    REQUIRE(blob.getSize() > 0);

    // Host reloads it into a fresh plugin instance.
    const auto restored = st::readPluginState(blob.getData(),
                                              static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);

    const auto restoredUi = restored.stateTree.getChildWithName(st::id::uiState);
    REQUIRE(restoredUi.isValid());
    const double restoredScale =
        static_cast<double>(restoredUi.getProperty(st::id::scaleFactor));
    CHECK(restoredScale == Approx(1.5));

    // Reopening the editor sizes itself to the persisted scale (restore-on-open):
    // the restored scale maps back to the same 1500×570 size it was saved at.
    CHECK(rz::widthForScale(restoredScale) == 1500);
    CHECK(rz::heightForScale(restoredScale) == 570);
}

TEST_CASE("resize: a clamped/garbage persisted scale restores to a legal size",
          "[resize]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    NullProcessor proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };

    // A session hand-edited (or written by a buggy build) to an absurd scale.
    auto tree = st::createDefault();
    tree.getChildWithName(st::id::uiState)
        .setProperty(st::id::scaleFactor, 99.0, nullptr);

    juce::MemoryBlock blob;
    st::writePluginState(apvts.copyState(), tree, blob);
    const auto restored =
        st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);

    const double raw = static_cast<double>(
        restored.stateTree.getChildWithName(st::id::uiState)
            .getProperty(st::id::scaleFactor));
    // The editor clamps the restored value before sizing, so the realized size
    // never exceeds the 2.0× ceiling.
    const double clamped = rz::clampScale(raw);
    CHECK(clamped == Approx(rz::kMaxScale));
    CHECK(rz::widthForScale(clamped) == rz::kMaxWidth);
    CHECK(rz::heightForScale(clamped) == rz::kMaxHeight);
}

// ---------------------------------------------------------------------------
// Visual-review snapshot dump at the resize extremes (env-gated, same pattern
// as test_editor_wiring.cpp's assembled-layout dump). Set
// MWS_FACEPLATE_SNAPSHOT_DIR to dump the WHOLE assembled editor at 0.6× and
// 2.0× — the HiDPI crispness / proportional-layout check the task asks to
// attach to the PR. The repo stays free of raster assets (ADR-005): nothing is
// dumped unless the env var is set. The layout math is the exact
// PluginEditor::resized scaledRegion mapping.
// ---------------------------------------------------------------------------

TEST_CASE("resize: assembled-layout snapshot dump at 0.6× and 2.0× when "
          "MWS_FACEPLATE_SNAPSHOT_DIR is set",
          "[resize]")
{
    using namespace mws;

    const auto dir =
        juce::SystemStats::getEnvironmentVariable("MWS_FACEPLATE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
    {
        SUCCEED("MWS_FACEPLATE_SNAPSHOT_DIR not set — resize snapshot dump skipped");
        return;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;
    NullProcessor proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };

    const auto outDir = juce::File(dir);
    REQUIRE(outDir.createDirectory().wasOk());

    for (const double scale : { rz::kMinScale, rz::kMaxScale })  // 0.6× and 2.0×
    {
        ui::SeriesLookAndFeel lookAndFeel;
        juce::Component editor;
        editor.setLookAndFeel(&lookAndFeel);

        ui::Faceplate plate;
        ui::LcdDisplay lcd;
        ui::SoftKeyBar bar;
        ui::JogWheel jog;
        ui::WaveformView wave;
        ui::ControlPanel panel{ apvts };

        lcd.setSpec(plate.spec());
        wave.setSpec(plate.spec());

        editor.addAndMakeVisible(plate);
        editor.addAndMakeVisible(wave);
        editor.addAndMakeVisible(bar);
        editor.addAndMakeVisible(jog);
        editor.addAndMakeVisible(lcd);
        editor.addAndMakeVisible(panel);

        editor.setSize(rz::widthForScale(scale), rz::heightForScale(scale));

        // §1 mockup positions — identical to PluginEditor::resized.
        plate.setBounds(editor.getLocalBounds());
        const float s = (float) editor.getWidth() / (float) geo::kBaseWidth;
        lcd.setBounds(ui::scaledRegion(geo::kLcd, editor.getWidth(), editor.getHeight())
                          .reduced(7.0f * s)
                          .toNearestInt());
        wave.setBounds(ui::scaledRegion(geo::kWaveform, editor.getWidth(),
                                        editor.getHeight())
                           .toNearestInt());
        const auto keysTop =
            ui::scaledRegion(geo::kSoftKeys, editor.getWidth(), editor.getHeight());
        const auto cursorBottom =
            ui::scaledRegion(geo::kCursorKeys, editor.getWidth(), editor.getHeight());
        bar.setBounds(keysTop.getUnion(cursorBottom).toNearestInt());
        jog.setBounds(ui::scaledRegion(geo::kJogWheel, editor.getWidth(),
                                       editor.getHeight())
                          .toNearestInt());
        panel.setBounds(ui::scaledRegion(geo::kModelSelector, editor.getWidth(),
                                         editor.getHeight())
                            .getSmallestIntegerContainer());

        engine::ParamSnapshot p;
        p.model = model::ModelId::S1000;
        p.pluginMode = engine::PluginMode::Sample;
        p.timeFactor = 300.0;
        p.cycleLen = 1000;
        ui::LcdSampleInfo sample;
        sample.name = "AMEN_165";
        sample.zoneEnd = 131071;
        sample.memPercent = 7;
        const auto page = ui::LcdPageModel::build(
            p, model::ModelSpec::get(model::ModelId::S1000), sample, {});
        ui::renderPage(page, lcd, /*cursorFieldIndex*/ 3);

        const auto snapshot =
            editor.createComponentSnapshot(editor.getLocalBounds(), false, 1.0f);
        editor.setLookAndFeel(nullptr);
        REQUIRE_FALSE(snapshot.isNull());

        const int pct = (int) std::lround(scale * 100.0);
        const auto file =
            outDir.getChildFile("editor_resize_S1000_" + juce::String(pct) + "pct.png");
        file.deleteFile();
        juce::FileOutputStream stream(file);
        REQUIRE(stream.openedOk());
        juce::PNGImageFormat png;
        REQUIRE(png.writeImageToStream(snapshot, stream));
        std::cout << "resize snapshot (" << pct << "%): " << file.getFullPathName()
                  << "\n";
    }
}
