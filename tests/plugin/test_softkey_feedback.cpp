// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 057 — soft keys must not look dead. Closes the test gap that let the
// "function buttons don't work" bug ship: the existing [inputcluster]/[editor]
// tests call pressKey(i) directly, bypassing the visible-button mouse path and
// the default-state enablement. These tests instead:
//
//   · drive REAL mouse clicks on the laid-out F1–F8 child buttons (resolved via
//     SoftKeyBar::getComponentAt, then mouseDown/mouseUp on the button — the
//     exact onStateChange -> pressKey path a user click takes), in BOTH FX and
//     SAMPLE modes, with the bar wired to the real ui::softKeyEnabled matrix.
//     Enabled keys fire their action exactly once; disabled keys fire nothing.
//   · assert mode-disabled keys are VISIBLY greyed (keyLegendAlpha < 1.0 — the
//     value paint() applies, so the disabled style is carried legibly).
//   · assert F1/F2/F7 produce their single-press LCD feedback at the
//     EditorActions + LcdPageModel level (the headless seam the editor uses):
//     F1 -> TIME page notice, F2 (no sample) -> "** LOAD SAMPLE **", F7 tap ->
//     tap progress, and a greyed FX-mode key -> "** SAMPLE MODE KEY **".
//
// Test-case names begin with the tag word `softkey` so `ctest -R softkey`
// matches (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/EditorActions.h"
#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/LcdPageModel.h"
#include "ui/SoftKeyBar.h"
#include "ui/lookandfeel/SeriesLookAndFeel.h"

#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::ui::LcdPageModel;
using mws::ui::LcdRenderInfo;
using mws::ui::SoftKeyBar;
namespace sk = mws::ui::softkey;

namespace {

struct JuceFixture {
    juce::ScopedJuceInitialiser_GUI init;
};

ParamSnapshot snap(ModelId model, PluginMode mode)
{
    ParamSnapshot p;
    p.model = model;
    p.pluginMode = mode;
    return p;
}

/// Lay the bar out exactly like PluginEditor::resized (the union of the §1
/// soft-key + cursor frames at 1.0 scale) so getComponentAt resolves to the
/// real F-key bounds.
void layoutLikeEditor(SoftKeyBar& bar)
{
    namespace geo = mws::ui::geometry;
    const auto keysTop = mws::ui::scaledRegion(geo::kSoftKeys, geo::kBaseWidth,
                                               geo::kBaseHeight);
    const auto cursorBottom = mws::ui::scaledRegion(geo::kCursorKeys, geo::kBaseWidth,
                                                    geo::kBaseHeight);
    bar.setBounds(keysTop.getUnion(cursorBottom).toNearestInt());
    // The editor adds the bar with addAndMakeVisible; getComponentAt only
    // descends into a VISIBLE parent, so mirror that here.
    bar.setVisible(true);
}

/// Drive a REAL press+release on soft key `index` through the VISIBLE button —
/// the button is resolved by getComponentAt at its on-screen cell centre (so a
/// layout/hit regression fails the test), then pressed via Button::setState,
/// which fires onStateChange exactly as a user mouse click does (juce_Button.cpp:
/// updateState -> setState -> sendStateMessage). This is the SoftKeyBar's real
/// onStateChange -> pressKey path, NOT the direct pressKey() the existing
/// [inputcluster]/[editor] tests use. Returns false if no child button resolves
/// at the cell centre (the gap that let the dead-looking keys ship).
bool realClickSoftKey(SoftKeyBar& bar, int index)
{
    // Centre of the key cap cell. The bar spans the union of the soft-key row
    // (geo::kSoftKeys, the top of the bar) and the cursor row below; the cap
    // occupies the top kCapFraction of the soft-key row. Resolve the cap centre
    // from those geometry fractions so getComponentAt lands on the F-key button.
    namespace geo = mws::ui::geometry;
    const float softRowFrac =
        (float) geo::kSoftKeys.h
        / (float) (geo::kCursorKeys.y + geo::kCursorKeys.h - geo::kSoftKeys.y);
    constexpr float kCapFraction = 0.62f;  // matches SoftKeyBar::resized
    const float cellW = (float) bar.getWidth() / (float) SoftKeyBar::kNumSoftKeys;
    const juce::Point<int> centre{
        juce::roundToInt(cellW * ((float) index + 0.5f)),
        juce::roundToInt((float) bar.getHeight() * softRowFrac * kCapFraction * 0.5f)
    };

    auto* hit = bar.getComponentAt(centre);
    auto* button = dynamic_cast<juce::Button*>(hit);
    if (button == nullptr)
        return false;

    // The resolved button must be the matching F-key (its text is "F<n>").
    if (button->getButtonText() != "F" + juce::String(index + 1))
        return false;

    button->setState(juce::Button::buttonDown);    // press  -> onStateChange(down)
    button->setState(juce::Button::buttonNormal);  // release-> onStateChange(up)
    return true;
}

/// Wire `bar` to the real per-mode/per-model enable matrix, counting fired
/// actions per index and disabled-key hits per index.
struct Harness {
    SoftKeyBar bar;
    std::array<int, sk::kCount> fired{};
    std::array<int, sk::kCount> disabledHits{};

    Harness(ModelId model, PluginMode mode)
    {
        const auto p = snap(model, mode);
        const auto& spec = ModelSpec::get(model);
        for (int i = 0; i < sk::kCount; ++i)
            bar.setKeyEnabled(i, mws::ui::softKeyEnabled(i, p, spec));
        bar.onSoftKey = [this](int i) {
            if (i >= 0 && i < sk::kCount)
                ++fired[(std::size_t) i];
        };
        bar.onDisabledKey = [this](int i) {
            if (i >= 0 && i < sk::kCount)
                ++disabledHits[(std::size_t) i];
        };
        layoutLikeEditor(bar);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Real clicks over F1–F8 in BOTH modes (the closed test gap)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(JuceFixture,
                 "softkey: a real click on the laid-out button fires the action "
                 "exactly once (SAMPLE mode, every key live)",
                 "[softkey]")
{
    Harness h(ModelId::S1000, PluginMode::Sample);

    // SAMPLE mode on a stretch model: every TIME-page key is enabled. F8 is the
    // 600 ms hold (it does not fire on a click), so click F1..F7 and assert each
    // fires its action exactly once through the REAL mouse path.
    for (int i = sk::kTime; i <= sk::kSync; ++i)
    {
        REQUIRE(h.bar.isKeyEnabled(i));
        REQUIRE(realClickSoftKey(h.bar, i));
        CHECK(h.fired[(std::size_t) i] == 1);
    }

    // No disabled-key feedback fired (nothing was greyed).
    for (int i = 0; i < sk::kCount; ++i)
        CHECK(h.disabledHits[(std::size_t) i] == 0);
}

TEST_CASE_METHOD(JuceFixture,
                 "softkey: real clicks in FX mode fire only the live keys; greyed "
                 "GO/PLAY/A-B/ZONE emit no action and are visibly disabled",
                 "[softkey]")
{
    Harness h(ModelId::S1000, PluginMode::Fx);
    const auto fx = snap(ModelId::S1000, PluginMode::Fx);
    const auto& spec = ModelSpec::get(ModelId::S1000);

    // Click every soft key F1..F7 (F8 is the hold key). The live keys fire
    // once; the FX-greyed keys (ZONE/GO/PLAY/A-B) fire NO action but DO report a
    // disabled-key press (the editor turns that into a "SAMPLE MODE KEY" hint).
    for (int i = sk::kTime; i <= sk::kSync; ++i)
        REQUIRE(realClickSoftKey(h.bar, i));

    for (int i = sk::kTime; i <= sk::kSync; ++i)
    {
        const bool enabled = mws::ui::softKeyEnabled(i, fx, spec);
        if (enabled)
        {
            CHECK(h.fired[(std::size_t) i] == 1);
            CHECK(h.disabledHits[(std::size_t) i] == 0);
            CHECK(h.bar.keyLegendAlpha(i) == 1.0f);  // live key drawn full-bright
        }
        else
        {
            CHECK(h.fired[(std::size_t) i] == 0);          // gated: no action
            CHECK(h.disabledHits[(std::size_t) i] == 1);   // but a visible press
            // Visibly greyed: the legend alpha paint() applies is dimmed.
            CHECK(h.bar.keyLegendAlpha(i) < 1.0f);
            CHECK(h.bar.keyLegendAlpha(i) == SoftKeyBar::kDisabledDim);
        }
    }

    // Exactly the §6.4 grey set went disabled.
    CHECK_FALSE(h.bar.isKeyEnabled(sk::kZone));
    CHECK_FALSE(h.bar.isKeyEnabled(sk::kGo));
    CHECK_FALSE(h.bar.isKeyEnabled(sk::kPlay));
    CHECK_FALSE(h.bar.isKeyEnabled(sk::kAb));
    CHECK(h.bar.isKeyEnabled(sk::kTime));
    CHECK(h.bar.isKeyEnabled(sk::kSync));
}

TEST_CASE_METHOD(JuceFixture,
                 "softkey: keyLegendAlpha reports the visible disabled (greyed) "
                 "style; enabled keys are full-bright",
                 "[softkey]")
{
    SoftKeyBar bar;

    for (int i = 0; i < SoftKeyBar::kNumSoftKeys; ++i)
        CHECK(bar.keyLegendAlpha(i) == 1.0f);  // all enabled by default

    bar.setKeyEnabled(sk::kGo, false);
    CHECK(bar.keyLegendAlpha(sk::kGo) == SoftKeyBar::kDisabledDim);
    CHECK(bar.keyLegendAlpha(sk::kGo) < 1.0f);
    CHECK(bar.keyLegendAlpha(sk::kTime) == 1.0f);  // unaffected neighbour

    bar.setKeyEnabled(sk::kGo, true);
    CHECK(bar.keyLegendAlpha(sk::kGo) == 1.0f);

    // Out-of-range is not UB.
    CHECK(bar.keyLegendAlpha(-1) == 0.0f);
    CHECK(bar.keyLegendAlpha(SoftKeyBar::kNumSoftKeys) == 0.0f);
}

// ---------------------------------------------------------------------------
// Single-press LCD feedback (F1 page, F2 no-sample, F7 tap, greyed key)
// ---------------------------------------------------------------------------

TEST_CASE("softkey: F1 TIME press shows the TIME page notice on the LCD",
          "[softkey]")
{
    const auto p = snap(ModelId::S1000, PluginMode::Sample);
    const auto& spec = ModelSpec::get(ModelId::S1000);

    const auto hint = mws::ui::softKeyPressHint(sk::kTime, p, spec, {});
    CHECK(hint == mws::ui::softkeyhint::kTimePage);

    // The page model renders the hint on the message line so the press has a
    // visible result (the editor stores it in LcdRenderInfo::softKeyHint).
    LcdRenderInfo render;
    render.softKeyHint = hint;
    const auto page = LcdPageModel::build(p, spec, {}, render);
    CHECK(page.textJoined().find(mws::ui::softkeyhint::kTimePage) != std::string::npos);
}

TEST_CASE("softkey: F2 autC with no sample loaded shows the LOAD SAMPLE hint "
          "instead of silently writing the fallback",
          "[softkey]")
{
    const auto p = snap(ModelId::S1000, PluginMode::Sample);
    const auto& spec = ModelSpec::get(ModelId::S1000);

    mws::ui::SoftKeyPressContext noSample;  // sampleLoaded == false
    const auto hint = mws::ui::softKeyPressHint(sk::kAutC, p, spec, noSample);
    CHECK(hint == mws::ui::softkeyhint::kLoadSample);

    LcdRenderInfo render;
    render.softKeyHint = hint;
    const auto page = LcdPageModel::build(p, spec, {}, render);
    CHECK(page.textJoined().find(mws::ui::softkeyhint::kLoadSample) != std::string::npos);

    // With a sample loaded the detector visibly updates the cycle field, so F2
    // produces no notice (the action itself is the feedback).
    mws::ui::SoftKeyPressContext withSample;
    withSample.sampleLoaded = true;
    CHECK(mws::ui::softKeyPressHint(sk::kAutC, p, spec, withSample).empty());
}

TEST_CASE("softkey: F7 SYNC tap shows per-tap feedback — a count on the first "
          "tap, then the running BPM",
          "[softkey]")
{
    const auto p = snap(ModelId::S1000, PluginMode::Sample);
    const auto& spec = ModelSpec::get(ModelId::S1000);

    // First tap: no BPM yet (one tap defines no interval), so the user still
    // gets visible confirmation that the tap registered.
    mws::ui::SoftKeyPressContext firstTap;
    firstTap.tapCount = 1;
    firstTap.tapBpm = 0.0;
    const auto first = mws::ui::softKeyPressHint(sk::kSync, p, spec, firstTap);
    CHECK(first == "** TAP 1 **");

    // Second tap settles a BPM: the readout shows it.
    mws::ui::SoftKeyPressContext secondTap;
    secondTap.tapCount = 2;
    secondTap.tapBpm = 120.0;
    const auto second = mws::ui::softKeyPressHint(sk::kSync, p, spec, secondTap);
    CHECK(second == "** SYNC 120 BPM **");

    // Renders on the LCD message line.
    LcdRenderInfo render;
    render.softKeyHint = second;
    const auto page = LcdPageModel::build(p, spec, {}, render);
    CHECK(page.textJoined().find("120 BPM") != std::string::npos);
}

TEST_CASE("softkey: a greyed FX-mode key reports the SAMPLE-mode hint so it "
          "reads as mode-gated, not broken",
          "[softkey]")
{
    const auto fx = snap(ModelId::S1000, PluginMode::Fx);
    const auto& spec = ModelSpec::get(ModelId::S1000);

    // GO/PLAY/A-B/ZONE are SAMPLE-mode keys: pressing them in FX surfaces the
    // discoverability hint (ui-design §6.4).
    for (int i : { sk::kZone, sk::kGo, sk::kPlay, sk::kAb })
    {
        REQUIRE_FALSE(mws::ui::softKeyEnabled(i, fx, spec));
        CHECK(mws::ui::softKeyPressHint(i, fx, spec, {})
              == mws::ui::softkeyhint::kSampleModeOnly);
    }

    // Always-live keys never emit the SAMPLE-mode hint.
    CHECK(mws::ui::softKeyPressHint(sk::kSync, fx, spec, {})
          != mws::ui::softkeyhint::kSampleModeOnly);
}
