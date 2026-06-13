// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CaptureStates — see CaptureStates.h.

#include "CaptureStates.h"

#include <stdexcept>
#include <unordered_map>

#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/FaceplateSpec.h"
#include "ui/LcdDisplay.h"
#include "ui/LcdPageBinding.h"
#include "ui/LcdPageModel.h"

#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"

namespace mws::uiscreenshot {

namespace {

using mws::engine::HopMode;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::SampleRateSel;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::ui::LcdDisplay;
using mws::ui::LcdPage;
using mws::ui::LcdPageModel;
using mws::ui::LcdRenderInfo;
using mws::ui::LcdSampleInfo;

// ---------------------------------------------------------------------------
// Fixed fake data (task 047b scope: "S1000 TIME page content fixed/fake data
// for determinism"). These are constants so the rendered LCD text is identical
// on every run — no clock, no random ids, no live engine feedback.
// ---------------------------------------------------------------------------

/// The canonical fixed sample readout used by every TIME-page capture. Mirrors
/// the ui-design §1 mockup screen (AMEN_165 / zone 0..131071 / 7% mem) so the
/// goldens read like the documented hardware page, but every field is a literal.
LcdSampleInfo fixedSample()
{
    LcdSampleInfo s;
    s.name = "AMEN_165";
    s.newName = "AMEN_165*ST";
    s.lengthFrames = 131072;
    s.zoneStart = 0;
    s.zoneEnd = 131071;
    s.memPercent = 7;
    s.monoSummed = false;
    return s;
}

/// A fixed SAMPLE-mode TIME-page parameter snapshot for `model`. CLASSIC,
/// character off, the §1 mockup-flavored CYCLIC/1000/300% jungle setting — all
/// literals, so the page text is deterministic.
ParamSnapshot fixedSampleParams(ModelId model)
{
    ParamSnapshot p;
    p.model = model;
    p.pluginMode = PluginMode::Sample;
    p.hopMode = HopMode::Classic;
    p.character = false;
    p.timeFactor = 300.0;
    p.cycleLen = 1000;
    p.transpose = 0.0;
    p.bandwidth = 19.2;
    p.sampleRateSel = SampleRateSel::Fs44100;
    return p;
}

/// The fixed FX-mode "TIME-STRETCH (REALTIME)" snapshot (ui-design §6.4): S1000,
/// FX, T=150% so the page shows a live (non-clamped) factor deterministically.
ParamSnapshot fixedFxParams()
{
    ParamSnapshot p = fixedSampleParams(ModelId::S1000);
    p.pluginMode = PluginMode::Fx;
    p.timeFactor = 150.0;
    return p;
}

/// No live engine feedback in a capture (no in-flight render, no clamp readout
/// that depends on transport): a zeroed/negative LcdRenderInfo keeps the page
/// static and reproducible.
LcdRenderInfo staticRenderInfo()
{
    LcdRenderInfo r;            // achievedLengthFrames = -1, progressPercent = -1
    r.sourceBpm = 0.0;
    r.hostBpm = 0.0;
    return r;
}

// ---------------------------------------------------------------------------
// Compositing: render the cached vector faceplate static layer, then paint the
// LcdDisplay (driven by an LcdPage) into the scaled LCD region on top — exactly
// how the live editor layers the dynamic LCD over the static chassis, but pure
// and headless (no Component tree, no timers).
// ---------------------------------------------------------------------------

/// Paint `lcd` into a software image of the given size using the JUCE software
/// renderer. The LcdDisplay is a juce::Component; paintEntireComponent drives its
/// paint() directly (no peer/window needed). No cursor/blink is ever set, so the
/// timer never starts and the output is deterministic.
juce::Image renderLcdImage(LcdDisplay& lcd, int width, int height)
{
    lcd.setBounds(0, 0, juce::jmax(1, width), juce::jmax(1, height));
    juce::Image image(juce::Image::ARGB, juce::jmax(1, width), juce::jmax(1, height),
                      true, juce::SoftwareImageType());
    juce::Graphics g(image);
    lcd.paintEntireComponent(g, false);
    return image;
}

/// Build the composite faceplate+LCD image for a model at the given scale, with
/// the LCD showing the given page.
juce::Image composite(ModelId model, double scale, const LcdPage& page)
{
    const auto& spec = mws::ui::faceplateSpecFor(model);
    const int width = (int) std::lround((double) mws::ui::geometry::kBaseWidth * scale);
    const int height = (int) std::lround((double) mws::ui::geometry::kBaseHeight * scale);

    // Static vector chassis (already a SoftwareImageType ARGB image, ADR-005).
    juce::Image image = mws::ui::renderFaceplateStaticLayer(spec, width, height);

    // The dynamic LCD layer, sized to the scaled LCD region the chassis drew.
    LcdDisplay lcd;
    lcd.setSpec(spec);
    mws::ui::renderPage(page, lcd, /*cursorFieldIndex*/ -1);  // no cursor → no blink

    const auto lcdRegion =
        mws::ui::scaledRegion(mws::ui::geometry::kLcd, width, height);
    const int lw = (int) std::lround(lcdRegion.getWidth());
    const int lh = (int) std::lround(lcdRegion.getHeight());
    const juce::Image lcdImage = renderLcdImage(lcd, lw, lh);

    juce::Graphics g(image);
    g.drawImageAt(lcdImage, (int) std::lround(lcdRegion.getX()),
                  (int) std::lround(lcdRegion.getY()));
    return image;
}

LcdPage pageForSampleModel(ModelId model)
{
    return LcdPageModel::build(fixedSampleParams(model), ModelSpec::get(model),
                               fixedSample(), staticRenderInfo());
}

// ---------------------------------------------------------------------------
// The state set (task 047b scope bullet 1).
// ---------------------------------------------------------------------------

const std::vector<CaptureState>& states()
{
    static const std::vector<CaptureState> kStates = {
        // Four shipping model faceplates at 1.0×, LCD driven by fixed fake data.
        { "faceplate_s900_time", "S900 faceplate, 1.0x, TIME page (varispeed + NO TIMESTRETCH notice)" },
        { "faceplate_s950_time", "S950 faceplate, 1.0x, 2-line STRETCH page" },
        { "faceplate_s1000_time", "S1000 faceplate, 1.0x, TIME page (CYCLIC/1000/300%)" },
        { "faceplate_s1100_time", "S1100 faceplate, 1.0x, TIME page (CYCLIC/1000/300%)" },
        // LCD content variants explicitly named in the scope.
        { "lcd_s950_2line", "S950 2-line LCD page only (40x2 display class)" },
        { "lcd_fx_realtime", "FX REALTIME LCD page only (S1000, TIME-STRETCH (REALTIME))" },
        // Scale checks on the canonical S1000 faceplate (ui-design §5).
        { "faceplate_s1000_scale06", "S1000 faceplate, 0.6x scale" },
        { "faceplate_s1000_scale20", "S1000 faceplate, 2.0x scale" },
    };
    return kStates;
}

/// Render just the LCD page (no chassis) into a base-canvas-LCD-sized image — for
/// the lcd_* states that isolate a display-class variant.
juce::Image lcdOnly(ModelId model, const LcdPage& page)
{
    const auto& spec = mws::ui::faceplateSpecFor(model);
    LcdDisplay lcd;
    lcd.setSpec(spec);
    mws::ui::renderPage(page, lcd, /*cursorFieldIndex*/ -1);
    // 1.0× LCD-region pixel size (deterministic, matches the in-chassis size).
    const int lw = mws::ui::geometry::kLcd.w;
    const int lh = mws::ui::geometry::kLcd.h;
    return renderLcdImage(lcd, lw, lh);
}

} // namespace

const std::vector<CaptureState>& captureStates() { return states(); }

bool isKnownState(const std::string& id)
{
    for (const auto& s : states())
        if (s.id == id)
            return true;
    return false;
}

juce::Image renderState(const std::string& id)
{
    if (id == "faceplate_s900_time")
        return composite(ModelId::S900, 1.0, pageForSampleModel(ModelId::S900));
    if (id == "faceplate_s950_time")
        return composite(ModelId::S950, 1.0, pageForSampleModel(ModelId::S950));
    if (id == "faceplate_s1000_time")
        return composite(ModelId::S1000, 1.0, pageForSampleModel(ModelId::S1000));
    if (id == "faceplate_s1100_time")
        return composite(ModelId::S1100, 1.0, pageForSampleModel(ModelId::S1100));

    if (id == "lcd_s950_2line")
        return lcdOnly(ModelId::S950, pageForSampleModel(ModelId::S950));
    if (id == "lcd_fx_realtime")
        return lcdOnly(ModelId::S1000,
                       LcdPageModel::build(fixedFxParams(), ModelSpec::get(ModelId::S1000),
                                           fixedSample(), staticRenderInfo()));

    if (id == "faceplate_s1000_scale06")
        return composite(ModelId::S1000, 0.6, pageForSampleModel(ModelId::S1000));
    if (id == "faceplate_s1000_scale20")
        return composite(ModelId::S1000, 2.0, pageForSampleModel(ModelId::S1000));

    throw std::runtime_error("unknown capture state: " + id);
}

} // namespace mws::uiscreenshot
