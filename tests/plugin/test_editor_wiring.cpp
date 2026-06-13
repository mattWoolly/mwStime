// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 045 — PluginEditor assembly: the LCD field-editing system and the
// UI-FIFO → LCD page binding, headless (docs/design/ui-design.md §6.2 steps
// 1–2; §2 LcdPageModel as the single source of truth; architecture.md §4
// render-done/progress via lock-free FIFO → timer poll). Covered:
//   · field cursor traversal order matches the 041 field map (greyed
//     qual/width skipped);
//   · jog delta on timeFactor writes the param with the correct hardware step,
//     fine (Shift) mode included;
//   · double-click direct text entry commit / cancel round-trip;
//   · a worker FIFO event refreshes the LCD cells through the page model
//     (no ad-hoc strings — LCD content comes only from LcdPageModel).
//
// Test-case names begin with the tag word so `ctest -R editor` matches
// (plan/backlog/README.md test-selection rules). The field editor and page
// binding hold no GUI state, so they run against a plain APVTS + LcdDisplay
// without a window (a ScopedJuceInitialiser_GUI is still needed for the
// LcdDisplay component and JUCE message infrastructure).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "EngineHost.h"
#include "state/Parameters.h"
#include "ui/ControlPanel.h"
#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/JogWheel.h"
#include "ui/LcdDisplay.h"
#include "ui/LcdFieldEditor.h"
#include "ui/LcdPageBinding.h"
#include "ui/LcdPageModel.h"
#include "ui/SoftKeyBar.h"
#include "ui/WaveformView.h"
#include "ui/lookandfeel/SeriesLookAndFeel.h"

#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

using Catch::Approx;
using mws::engine::ParamId;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::ui::CursorDir;
using mws::ui::LcdDisplay;
using mws::ui::LcdField;
using mws::ui::LcdFieldEditor;
using mws::ui::LcdFieldKind;
using mws::ui::LcdPage;
using mws::ui::LcdPageModel;
using mws::ui::LcdRenderInfo;
using mws::ui::LcdSampleInfo;
namespace pid = mws::plugin::paramid;

namespace {

/// Minimal host-side AudioProcessor so the APVTS has an owner (same pattern as
/// test_controlpanel.cpp / test_parameters.cpp).
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
    LcdFieldEditor editor{ apvts };

    /// The S1000 SAMPLE-mode TIME page (the canonical field map).
    LcdPage s1000Page()
    {
        ParamSnapshot p;
        p.model = ModelId::S1000;
        p.pluginMode = PluginMode::Sample;
        LcdSampleInfo s;
        s.name = "AMEN_165";
        s.zoneEnd = 131071;
        return LcdPageModel::build(p, ModelSpec::get(ModelId::S1000), s, {});
    }

    float hostValue(const char* id)
    {
        auto* p = apvts.getParameter(id);
        REQUIRE(p != nullptr);
        return p->convertFrom0to1(p->getValue());
    }

    void setHost(const char* id, float denorm)
    {
        auto* p = apvts.getParameter(id);
        REQUIRE(p != nullptr);
        p->setValueNotifyingHost(p->convertTo0to1(denorm));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Field cursor traversal — matches the 041 field map; greyed fields skipped
// ---------------------------------------------------------------------------

TEST_CASE("editor: field cursor traversal follows the 041 field map and skips "
          "greyed fields",
          "[editor]")
{
    Fixture f;
    const LcdPage page = f.s1000Page();
    f.editor.setPage(page);

    // The 041 S1000 field map order is: ZoneStart, ZoneEnd, CycleLen,
    // TimeFactor, StretchMode, Qual(greyed), Width(greyed), NewName. The cursor
    // parks on the first editable field (ZoneStart, index 0).
    REQUIRE(f.editor.focusedIndex() == 0);
    REQUIRE(f.editor.focusedField() != nullptr);
    CHECK(f.editor.focusedField()->kind == LcdFieldKind::ZoneStart);

    // Walk forward across every editable field, recording the visited indices.
    std::vector<int> visited{ f.editor.focusedIndex() };
    for (int i = 0; i < 5; ++i)
    {
        f.editor.moveCursor(CursorDir::Right);
        visited.push_back(f.editor.focusedIndex());
    }

    // ZoneStart(0) → ZoneEnd(1) → CycleLen(2) → TimeFactor(3) → StretchMode(4)
    // → NewName(7): Qual(5)/Width(6) are greyed and skipped.
    CHECK(visited == std::vector<int>{ 0, 1, 2, 3, 4, 7 });

    // Down behaves like Right (next field); Up/Left go to the previous one.
    f.editor.focusField(7);
    f.editor.moveCursor(CursorDir::Down);  // wraps back to the first editable
    CHECK(f.editor.focusedIndex() == 0);
    f.editor.moveCursor(CursorDir::Up);    // wraps to the last editable (NewName)
    CHECK(f.editor.focusedIndex() == 7);
    f.editor.moveCursor(CursorDir::Left);  // previous editable skips Width/Qual
    CHECK(f.editor.focusedIndex() == 4);

    // The greyed qual/width fields are never landed on.
    f.editor.focusField(5);  // attempting to focus a greyed field is a no-op
    CHECK(f.editor.focusedIndex() == 4);
}

// ---------------------------------------------------------------------------
// Jog editing — correct hardware step on timeFactor, fine mode included
// ---------------------------------------------------------------------------

TEST_CASE("editor: jog edits the focused timeFactor field by the hardware step "
          "(coarse + fine)",
          "[editor]")
{
    Fixture f;
    f.setHost(pid::timeFactor, 300.0f);
    f.editor.setPage(f.s1000Page());

    // Focus the TimeFactor field (index 3 in the S1000 map).
    f.editor.focusField(3);
    REQUIRE(f.editor.focusedField()->kind == LcdFieldKind::Param);
    REQUIRE(f.editor.focusedField()->param == ParamId::TimeFactor);

    // Coarse step is 1% per detent: +5 detents → 305%.
    const auto coarse = LcdFieldEditor::stepFor(ParamId::TimeFactor);
    CHECK(coarse.coarse == Approx(1.0));
    CHECK(coarse.fine == Approx(0.01));

    f.editor.applyJog(5, /*fine*/ false);
    CHECK(f.hostValue(pid::timeFactor) == Approx(305.0));

    // A reverse detent decrements.
    f.editor.applyJog(-2, false);
    CHECK(f.hostValue(pid::timeFactor) == Approx(303.0));

    // Fine mode (Shift): 0.01% per detent — +4 fine detents → 303.04%.
    f.editor.applyJog(4, /*fine*/ true);
    CHECK(f.hostValue(pid::timeFactor) == Approx(303.04));
}

TEST_CASE("editor: jog respects the superset range and never lands on a greyed "
          "field",
          "[editor]")
{
    Fixture f;
    f.editor.setPage(f.s1000Page());

    // cycleLen jog: coarse 10 samples/detent (D-time idiom).
    const auto cyc = LcdFieldEditor::stepFor(ParamId::CycleLen);
    CHECK(cyc.coarse == Approx(10.0));
    f.setHost(pid::cycleLen, 1000.0f);
    f.editor.focusField(2);  // CycleLen
    f.editor.applyJog(3, false);
    CHECK(f.hostValue(pid::cycleLen) == Approx(1030.0));

    // Drive timeFactor against the 2000% superset ceiling: it clamps, never
    // exceeds (architecture.md §6 fixed superset range).
    f.setHost(pid::timeFactor, 1999.0f);
    f.editor.focusField(3);
    f.editor.applyJog(50, false);  // +50% would be 2049 → clamps to 2000
    CHECK(f.hostValue(pid::timeFactor) == Approx(2000.0));
}

// ---------------------------------------------------------------------------
// Direct text entry — double-click commit / cancel round-trip
// ---------------------------------------------------------------------------

TEST_CASE("editor: double-click text entry commits the typed value (ENT) and "
          "cancel leaves it unchanged",
          "[editor]")
{
    Fixture f;
    f.setHost(pid::timeFactor, 300.0f);
    f.editor.setPage(f.s1000Page());
    f.editor.focusField(3);  // TimeFactor

    // Double-click begins entry; the seed is the current hardware-unit text.
    f.editor.beginTextEntry();
    CHECK(f.editor.isEditingText());
    CHECK(f.editor.currentFieldText() == juce::String("300%"));

    // Commit a typed value (ENT). The parameter's value-from-string strips the
    // unit and writes it.
    f.editor.commitText("150");
    CHECK_FALSE(f.editor.isEditingText());
    CHECK(f.hostValue(pid::timeFactor) == Approx(150.0));

    // Cancel round-trip: begin, then cancel — the value is untouched.
    f.editor.beginTextEntry();
    CHECK(f.editor.isEditingText());
    f.editor.cancelText();
    CHECK_FALSE(f.editor.isEditingText());
    CHECK(f.hostValue(pid::timeFactor) == Approx(150.0));

    // Committing while NOT editing is ignored (no stray writes).
    f.editor.commitText("999");
    CHECK(f.hostValue(pid::timeFactor) == Approx(150.0));
}

// ---------------------------------------------------------------------------
// FIFO event → LCD cell refresh (architecture.md §4 timer poll)
// ---------------------------------------------------------------------------

TEST_CASE("editor: a worker FIFO event refreshes the LCD cells via the page "
          "model",
          "[editor]")
{
    Fixture f;
    LcdDisplay lcd;

    LcdRenderInfo render;

    // A Progress event folds into the render info as a percent...
    mws::plugin::WorkerEvent ev;
    ev.kind = mws::plugin::WorkerEvent::Kind::Progress;
    ev.progress = 0.42f;
    mws::ui::applyWorkerEvent(ev, render);
    CHECK(render.progressPercent == 42);

    // ...and the page model turns it into the §6.3 hardware progress line,
    // which renderPage writes into the LCD cells (LCD content comes ONLY from
    // the page model — no ad-hoc strings in the editor).
    ParamSnapshot p;
    p.model = ModelId::S1000;
    p.pluginMode = PluginMode::Sample;
    LcdSampleInfo sample;
    sample.name = "AMEN_165";
    LcdPage page = LcdPageModel::build(p, ModelSpec::get(ModelId::S1000), sample, render);
    mws::ui::renderPage(page, lcd, /*cursorFieldIndex*/ 3);

    // Read the bottom message row back out of the LCD cell grid.
    std::string row;
    for (int c = 0; c < LcdDisplay::kCols; ++c)
        row += lcd.cellChar(LcdDisplay::kRows - 1, c);
    CHECK(row.find("STRETCHING 42%") != std::string::npos);

    // The block cursor parked on the focused field (TimeFactor value cell).
    CHECK(lcd.hasCursor());

    // A Finished/NotEnoughMemory event raises the refusal flag → the refusal
    // line replaces the progress line on the next page build.
    mws::plugin::WorkerEvent done;
    done.kind = mws::plugin::WorkerEvent::Kind::Finished;
    done.outcome = mws::plugin::RenderOutcome::NotEnoughMemory;
    mws::ui::applyWorkerEvent(done, render);
    CHECK(render.progressPercent < 0);
    CHECK(render.notEnoughMemory);

    page = LcdPageModel::build(p, ModelSpec::get(ModelId::S1000), sample, render);
    mws::ui::renderPage(page, lcd, -1);
    std::string row2;
    for (int c = 0; c < LcdDisplay::kCols; ++c)
        row2 += lcd.cellChar(LcdDisplay::kRows - 1, c);
    CHECK(row2.find("NOT ENOUGH MEMORY") != std::string::npos);
    CHECK_FALSE(lcd.hasCursor());  // negative field index hides the cursor
}

// ---------------------------------------------------------------------------
// Visual-review snapshot (env-gated, same pattern as test_inputcluster.cpp;
// the repo stays free of raster assets, ADR-005). Set
// MWS_FACEPLATE_SNAPSHOT_DIR to dump a 1.0-scale PNG of the WHOLE assembled
// editor — Faceplate + LCD (driven by LcdPageModel) + soft keys + jog +
// waveform + control panel — at the §1 mockup positions, exactly the
// PluginEditor::resized layout. This is the manual layout check the task asks
// to attach to the PR.
// ---------------------------------------------------------------------------

TEST_CASE("editor: assembled-layout snapshot dump when "
          "MWS_FACEPLATE_SNAPSHOT_DIR is set",
          "[editor]")
{
    const auto dir =
        juce::SystemStats::getEnvironmentVariable("MWS_FACEPLATE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
    {
        SUCCEED("MWS_FACEPLATE_SNAPSHOT_DIR not set — snapshot dump skipped");
        return;
    }

    namespace geo = mws::ui::geometry;
    Fixture f;

    mws::ui::SeriesLookAndFeel lookAndFeel;
    juce::Component editor;
    editor.setLookAndFeel(&lookAndFeel);

    mws::ui::Faceplate plate;
    mws::ui::LcdDisplay lcd;
    mws::ui::SoftKeyBar bar;
    mws::ui::JogWheel jog;
    mws::ui::WaveformView wave;
    mws::ui::ControlPanel panel{ f.apvts };

    lcd.setSpec(plate.spec());
    wave.setSpec(plate.spec());

    editor.addAndMakeVisible(plate);
    editor.addAndMakeVisible(wave);
    editor.addAndMakeVisible(bar);
    editor.addAndMakeVisible(jog);
    editor.addAndMakeVisible(lcd);
    editor.addAndMakeVisible(panel);
    editor.setSize(geo::kBaseWidth, geo::kBaseHeight);  // 1.0 scale

    // Drive the LCD from the page model (no ad-hoc strings) and park the field
    // cursor on the time-factor field, exactly as the editor does.
    ParamSnapshot p;
    p.model = ModelId::S1000;
    p.pluginMode = PluginMode::Sample;
    p.timeFactor = 300.0;
    p.cycleLen = 1000;
    LcdSampleInfo sample;
    sample.name = "AMEN_165";
    sample.zoneEnd = 131071;
    sample.memPercent = 7;
    const auto page = LcdPageModel::build(p, ModelSpec::get(ModelId::S1000), sample, {});

    // §1 mockup positions (PluginEditor::resized).
    plate.setBounds(editor.getLocalBounds());
    const float scale = (float) editor.getWidth() / (float) geo::kBaseWidth;
    lcd.setBounds(mws::ui::scaledRegion(geo::kLcd, editor.getWidth(), editor.getHeight())
                      .reduced(7.0f * scale)
                      .toNearestInt());
    wave.setBounds(
        mws::ui::scaledRegion(geo::kWaveform, editor.getWidth(), editor.getHeight())
            .toNearestInt());
    const auto keysTop =
        mws::ui::scaledRegion(geo::kSoftKeys, editor.getWidth(), editor.getHeight());
    const auto cursorBottom =
        mws::ui::scaledRegion(geo::kCursorKeys, editor.getWidth(), editor.getHeight());
    bar.setBounds(keysTop.getUnion(cursorBottom).toNearestInt());
    jog.setBounds(
        mws::ui::scaledRegion(geo::kJogWheel, editor.getWidth(), editor.getHeight())
            .toNearestInt());
    panel.setBounds(mws::ui::scaledRegion(geo::kModelSelector, editor.getWidth(),
                                          editor.getHeight())
                        .getSmallestIntegerContainer());

    // Page binding renders after layout so cellBounds() has real metrics.
    mws::ui::renderPage(page, lcd, /*cursorFieldIndex*/ 3);

    const auto snapshot =
        editor.createComponentSnapshot(editor.getLocalBounds(), false, 1.0f);
    editor.setLookAndFeel(nullptr);
    REQUIRE_FALSE(snapshot.isNull());

    const auto outDir = juce::File(dir);
    REQUIRE(outDir.createDirectory().wasOk());
    const auto file = outDir.getChildFile("editor_assembly_S1000.png");
    file.deleteFile();
    juce::FileOutputStream stream(file);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(snapshot, stream));
    std::cout << "editor assembly snapshot: " << file.getFullPathName() << "\n";
}
