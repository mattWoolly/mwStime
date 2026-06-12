// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 042 — SoftKeyBar (F1–F8 + cursor/ENT) and JogWheel, headless
// (docs/design/ui-design.md §2, §6.3, §6.4, §7). Hold-gesture timing runs on a
// fake clock (599 ms must NOT fire, 600 ms must — §6.3 hold-F8-ABORT (PI));
// jog delta accumulation covers coarse, fine (Shift) scaling and the velocity
// multiplier; disabled keys (FX-mode grey state, §6.4) must emit nothing.
//
// Test-case names begin with the tag word so `ctest -R inputcluster` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <utility>
#include <vector>

#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/JogWheel.h"
#include "ui/SoftKeyBar.h"
#include "ui/lookandfeel/SeriesLookAndFeel.h"

using mws::ui::CursorDir;
using mws::ui::JogWheel;
using mws::ui::SoftKeyBar;

namespace {

/// Components need the JUCE GUI runtime even headlessly.
struct JuceFixture {
    juce::ScopedJuceInitialiser_GUI init;
};

} // namespace

// ---------------------------------------------------------------------------
// SoftKeyBar — basic emission + runtime relabelling
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: soft key press emits its index; no hold by default",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    std::vector<int> fired;
    bar.onSoftKey = [&](int i) { fired.push_back(i); };

    bar.pressKey(0);
    bar.releaseKey(0);
    bar.pressKey(6);
    bar.releaseKey(6);

    REQUIRE(fired == std::vector<int>{ 0, 6 });
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: captions are settable at runtime (page model "
                 "drives them later)",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    // Defaults are the §1 mockup TIME-page set.
    CHECK(bar.keyLabel(0) == "TIME");
    CHECK(bar.keyLabel(7) == "ABORT");

    bar.setKeyLabel(2, "TUNE");
    CHECK(bar.keyLabel(2) == "TUNE");

    // Out-of-range indices are ignored, not UB.
    bar.setKeyLabel(99, "NOPE");
    bar.setKeyLabel(-1, "NOPE");
    CHECK(bar.keyLabel(99).isEmpty());
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: disabled keys emit nothing (FX mode greys "
                 "GO/PLAY/A-B, ui-design §6.4)",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    int hits = 0;
    bar.onSoftKey = [&](int) { ++hits; };

    bar.setKeyEnabled(3, false);  // GO
    REQUIRE_FALSE(bar.isKeyEnabled(3));

    bar.pressKey(3);
    bar.releaseKey(3);
    CHECK(hits == 0);

    // Disabled hold keys never even start the gesture.
    juce::int64 now = 0;
    bar.setTimeSource([&] { return now; });
    bar.setKeyRequiresHold(3, 600);
    bar.pressKey(3);
    now = 1000;
    bar.updateHoldProgress();
    CHECK(hits == 0);
    CHECK(bar.holdProgress(3) == 0.0f);

    // Re-enabling restores emission.
    bar.setKeyEnabled(3, true);
    bar.setKeyRequiresHold(3, 0);
    bar.pressKey(3);
    CHECK(hits == 1);
}

// ---------------------------------------------------------------------------
// SoftKeyBar — hold gesture (F8 ABORT, ui-design §6.3: hold >= 600 ms (PI))
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: hold key does not fire at 599 ms, fires once at "
                 "600 ms (F8 ABORT)",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    juce::int64 now = 0;
    bar.setTimeSource([&] { return now; });

    // F8 ships configured as the 600 ms ABORT hold (§6.3); make it explicit.
    bar.setKeyRequiresHold(7, 600);
    REQUIRE(bar.keyHoldMs(7) == 600);

    std::vector<int> fired;
    bar.onSoftKey = [&](int i) { fired.push_back(i); };

    bar.pressKey(7);
    CHECK(fired.empty());  // hold keys never fire on press

    now = 599;
    bar.updateHoldProgress();
    CHECK(fired.empty());  // 599 ms: not yet
    CHECK(bar.holdProgress(7) > 0.9f);  // ...but the visual cue is nearly full

    now = 600;
    bar.updateHoldProgress();
    REQUIRE(fired == std::vector<int>{ 7 });  // 600 ms: fires

    // Continuing to hold must not re-fire.
    now = 5000;
    bar.updateHoldProgress();
    CHECK(fired.size() == 1);

    // Release resets the visual progress cue.
    bar.releaseKey(7);
    CHECK(bar.holdProgress(7) == 0.0f);
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: releasing before the hold threshold cancels — "
                 "press must be continuous",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    juce::int64 now = 0;
    bar.setTimeSource([&] { return now; });
    bar.setKeyRequiresHold(7, 600);

    int hits = 0;
    bar.onSoftKey = [&](int) { ++hits; };

    bar.pressKey(7);
    now = 300;
    bar.updateHoldProgress();
    bar.releaseKey(7);  // let go half way

    now = 1000;  // time passing after release must not fire
    bar.updateHoldProgress();
    CHECK(hits == 0);
    CHECK(bar.holdProgress(7) == 0.0f);

    // A fresh press needs the FULL duration again (no carry-over).
    bar.pressKey(7);
    now = 1599;
    bar.updateHoldProgress();
    CHECK(hits == 0);
    now = 1600;
    bar.updateHoldProgress();
    CHECK(hits == 1);
}

// ---------------------------------------------------------------------------
// SoftKeyBar — cursor cluster + ENT, with keyboard mirroring (ui-design §7)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: cursor cluster and ENT emit navigation events",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    std::vector<CursorDir> dirs;
    int enters = 0;
    bar.onCursor = [&](CursorDir d) { dirs.push_back(d); };
    bar.onEnter = [&] { ++enters; };

    bar.triggerCursor(CursorDir::Left);
    bar.triggerCursor(CursorDir::Right);
    bar.triggerCursor(CursorDir::Up);
    bar.triggerCursor(CursorDir::Down);
    bar.triggerEnter();

    REQUIRE(dirs
            == std::vector<CursorDir>{ CursorDir::Left, CursorDir::Right, CursorDir::Up,
                                       CursorDir::Down });
    CHECK(enters == 1);
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: keyboard arrows and Enter mirror the cursor/ENT "
                 "keys (ui-design §7)",
                 "[inputcluster]")
{
    SoftKeyBar bar;

    std::vector<CursorDir> dirs;
    int enters = 0;
    bar.onCursor = [&](CursorDir d) { dirs.push_back(d); };
    bar.onEnter = [&] { ++enters; };

    CHECK(bar.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
    CHECK(bar.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    CHECK(bar.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(bar.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
    CHECK(bar.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));

    REQUIRE(dirs
            == std::vector<CursorDir>{ CursorDir::Left, CursorDir::Right, CursorDir::Up,
                                       CursorDir::Down });
    CHECK(enters == 1);

    // Unrelated keys are not consumed.
    CHECK_FALSE(bar.keyPressed(juce::KeyPress('a')));
}

// ---------------------------------------------------------------------------
// JogWheel — delta accumulation, fine scaling, velocity, keyboard mirroring
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: jog wheel accumulates rotation into whole steps "
                 "with remainder carry",
                 "[inputcluster]")
{
    JogWheel jog;

    int total = 0;
    int calls = 0;
    jog.onDelta = [&](int steps, bool) { total += steps; ++calls; };

    // 0.6 of a detent: nothing yet.
    jog.rotateBy(JogWheel::kRadiansPerStep * 0.6f, false);
    CHECK(calls == 0);

    // Another 0.6: crosses one detent, 0.2 carries over.
    jog.rotateBy(JogWheel::kRadiansPerStep * 0.6f, false);
    CHECK(total == 1);

    // 2.9 more detents of angle: 0.2 carry + 2.9 = 3.1 → 3 steps.
    jog.rotateBy(JogWheel::kRadiansPerStep * 2.9f, false);
    CHECK(total == 4);

    // Endless: reverse direction yields negative steps.
    jog.rotateBy(-JogWheel::kRadiansPerStep * 2.0f, false);
    CHECK(total == 2);
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: jog fine mode (Shift) scales steps down by the "
                 "fine factor and flags fine",
                 "[inputcluster]")
{
    JogWheel jog;

    int total = 0;
    bool sawFine = false;
    bool sawCoarse = false;
    jog.onDelta = [&](int steps, bool fine) {
        total += steps;
        (fine ? sawFine : sawCoarse) = true;
    };

    // One full fine-factor sweep of angle = kFineFactor coarse steps...
    jog.rotateBy(JogWheel::kRadiansPerStep * (float) JogWheel::kFineFactor, false);
    CHECK(total == JogWheel::kFineFactor);
    CHECK(sawCoarse);

    // ...but only ONE step in fine mode, flagged fine.
    total = 0;
    jog.rotateBy(JogWheel::kRadiansPerStep * (float) JogWheel::kFineFactor, true);
    CHECK(total == 1);
    CHECK(sawFine);
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: jog velocity multiplier scales coarse deltas, "
                 "is clamped, and never affects fine mode",
                 "[inputcluster]")
{
    JogWheel jog;

    int total = 0;
    jog.onDelta = [&](int steps, bool) { total += steps; };

    // One detent of physical angle at 4x velocity → 4 steps.
    jog.rotateBy(JogWheel::kRadiansPerStep, false, 4.0f);
    CHECK(total == 4);

    // Velocity is clamped to the maximum multiplier.
    total = 0;
    jog.rotateBy(JogWheel::kRadiansPerStep, false, 1000.0f);
    CHECK(total == (int) JogWheel::kMaxVelocityMultiplier);

    // Fine mode ignores velocity: precision gestures stay 1:1.
    total = 0;
    jog.rotateBy(JogWheel::kRadiansPerStep * (float) JogWheel::kFineFactor, true, 1000.0f);
    CHECK(total == 1);
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: jog Up/Down keys mirror the wheel; Shift makes "
                 "them fine (ui-design §7)",
                 "[inputcluster]")
{
    JogWheel jog;

    std::vector<std::pair<int, bool>> deltas;
    jog.onDelta = [&](int steps, bool fine) { deltas.emplace_back(steps, fine); };

    CHECK(jog.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(jog.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
    CHECK(jog.keyPressed(
        juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));

    REQUIRE(deltas
            == std::vector<std::pair<int, bool>>{
                   { 1, false }, { -1, false }, { 1, true } });

    // Unrelated keys are not consumed.
    CHECK_FALSE(jog.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
}

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: the drawn dimple angle follows interaction",
                 "[inputcluster]")
{
    JogWheel jog;

    const float before = jog.dimpleAngle();
    jog.rotateBy(0.5f, false);
    CHECK(jog.dimpleAngle() > before);

    const float afterDrag = jog.dimpleAngle();
    jog.nudge(-2, false);  // keyboard nudges rotate the dimple too
    CHECK(jog.dimpleAngle() < afterDrag);
}

// ---------------------------------------------------------------------------
// Accessibility (ui-design §7: screen-reader names on everything)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: soft keys, cursor/ENT keys and jog wheel expose "
                 "accessibility names",
                 "[inputcluster]")
{
    SoftKeyBar bar;
    JogWheel jog;

    // Every child button of the bar (8 soft keys + 4 cursors + ENT) is named.
    int namedButtons = 0;
    for (auto* child : bar.getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child))
        {
            CHECK(button->getTitle().isNotEmpty());
            ++namedButtons;
        }
    CHECK(namedButtons == SoftKeyBar::kNumSoftKeys + 5);

    // Soft-key accessibility names track runtime captions.
    bar.setKeyLabel(0, "TUNE");
    bool foundCaption = false;
    for (auto* child : bar.getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child))
            foundCaption = foundCaption || button->getTitle().contains("TUNE");
    CHECK(foundCaption);

    CHECK(jog.getTitle().isNotEmpty());
}

// ---------------------------------------------------------------------------
// Visual-review snapshot (env-gated, same pattern as test_faceplatespec.cpp;
// the repo stays free of raster assets, ADR-005). Set
// MWS_FACEPLATE_SNAPSHOT_DIR to dump a 1.0-scale PNG of the input cluster
// mounted on the faceplate at the §1 mockup positions — exactly the
// PluginEditor::resized layout.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "inputcluster: snapshot dump for visual review when "
                 "MWS_FACEPLATE_SNAPSHOT_DIR is set",
                 "[inputcluster]")
{
    const auto dir =
        juce::SystemStats::getEnvironmentVariable("MWS_FACEPLATE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
    {
        SUCCEED("MWS_FACEPLATE_SNAPSHOT_DIR not set — snapshot dump skipped");
        return;
    }

    namespace geo = mws::ui::geometry;

    mws::ui::SeriesLookAndFeel lookAndFeel;
    juce::Component editor;
    editor.setLookAndFeel(&lookAndFeel);

    mws::ui::Faceplate plate;
    SoftKeyBar bar;
    JogWheel jog;
    editor.addAndMakeVisible(plate);
    editor.addAndMakeVisible(bar);
    editor.addAndMakeVisible(jog);
    editor.setSize(geo::kBaseWidth, geo::kBaseHeight);  // 1.0 scale

    plate.setBounds(editor.getLocalBounds());
    const auto keysTop =
        mws::ui::scaledRegion(geo::kSoftKeys, editor.getWidth(), editor.getHeight());
    const auto cursorBottom =
        mws::ui::scaledRegion(geo::kCursorKeys, editor.getWidth(), editor.getHeight());
    bar.setBounds(keysTop.getUnion(cursorBottom).toNearestInt());
    jog.setBounds(mws::ui::scaledRegion(geo::kJogWheel, editor.getWidth(),
                                        editor.getHeight())
                      .toNearestInt());

    // Show the §6.4 grey state and the dimple off its rest position.
    bar.setKeyEnabled(3, false);  // GO greys in FX mode
    jog.rotateBy(0.8f, false);

    const auto snapshot =
        editor.createComponentSnapshot(editor.getLocalBounds(), false, 1.0f);
    editor.setLookAndFeel(nullptr);
    REQUIRE_FALSE(snapshot.isNull());

    const auto outDir = juce::File(dir);
    REQUIRE(outDir.createDirectory().wasOk());
    const auto file = outDir.getChildFile("inputcluster_S1000.png");
    file.deleteFile();
    juce::FileOutputStream stream(file);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(snapshot, stream));
    std::cout << "inputcluster snapshot: " << file.getFullPathName() << "\n";
}
