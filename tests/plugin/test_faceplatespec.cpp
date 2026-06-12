// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 039 — FaceplateSpec table + faceplate geometry, headless. Palette hex
// values are asserted verbatim against the docs/design/ui-design.md §3 table;
// geometry sanity against FaceplateGeometry.h (regions inside canvas,
// non-overlapping); the cached static layer render against the ui-design §4
// < 2 ms repaint budget.
//
// Test-case names begin with the tag word so `ctest -R faceplatespec`
// matches (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <string_view>

#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/FaceplateSpec.h"

#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"

using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::ui::FaceplateSpec;
using mws::ui::LcdLayout;
namespace geo = mws::ui::geometry;

namespace {

const FaceplateSpec& spec(ModelId id) { return mws::ui::faceplateSpecFor(id); }

juce::Colour rgb(juce::uint32 argb) { return juce::Colour(argb); }

} // namespace

// ---------------------------------------------------------------------------
// Spec table presence
// ---------------------------------------------------------------------------

TEST_CASE("faceplatespec: table covers all five models in ModelId order",
          "[faceplatespec]")
{
    STATIC_REQUIRE(mws::ui::kFaceplateSpecs.size() == mws::model::kModelCount);

    for (const auto id : mws::model::kAllModels)
        REQUIRE(spec(id).id == id);
}

TEST_CASE("faceplatespec: four shipping specs present; S3000 slot reserved, "
          "not shipping (ADR-004)",
          "[faceplatespec]")
{
    REQUIRE(mws::ui::isShippingFaceplate(ModelId::S900));
    REQUIRE(mws::ui::isShippingFaceplate(ModelId::S950));
    REQUIRE(mws::ui::isShippingFaceplate(ModelId::S1000));
    REQUIRE(mws::ui::isShippingFaceplate(ModelId::S1100));

    // The slot exists (spec row present) but is flagged not-shipping.
    REQUIRE(spec(ModelId::S3000).id == ModelId::S3000);
    REQUIRE_FALSE(mws::ui::isShippingFaceplate(ModelId::S3000));
    REQUIRE(mws::ui::isShippingFaceplate(ModelId::S3000)
            == ModelSpec::isShipping(ModelId::S3000));
}

// ---------------------------------------------------------------------------
// Palette — ui-design §3 table hex values, verbatim
// ---------------------------------------------------------------------------

TEST_CASE("faceplatespec: S900 palette matches ui-design §3 verbatim",
          "[faceplatespec]")
{
    const auto& s = spec(ModelId::S900);
    CHECK(s.chassis == rgb(0xFF26282B));  // near-black charcoal
    CHECK(s.legend == rgb(0xFFE8E4D8));   // off-white
    CHECK(s.accent == rgb(0xFFD84B20));   // red-orange
    CHECK(s.lcdBack == rgb(0xFF2E3324));  // dark olive
    CHECK(s.lcdInk == rgb(0xFFFFB02E));   // amber
}

TEST_CASE("faceplatespec: S950 palette matches ui-design §3 verbatim",
          "[faceplatespec]")
{
    const auto& s = spec(ModelId::S950);
    CHECK(s.chassis == rgb(0xFF2C2E31));  // charcoal
    CHECK(s.legend == rgb(0xFFE8E4D8));   // off-white
    CHECK(s.accent == rgb(0xFFE09520));   // amber
    CHECK(s.lcdBack == rgb(0xFF333826));  // olive
    CHECK(s.lcdInk == rgb(0xFFFFB02E));   // amber
}

TEST_CASE("faceplatespec: S1000 palette matches ui-design §3 verbatim "
          "(the canonical grey + green look)",
          "[faceplatespec]")
{
    const auto& s = spec(ModelId::S1000);
    CHECK(s.chassis == rgb(0xFFB9B6AE));  // warm light grey
    CHECK(s.legend == rgb(0xFF2A2A2A));   // dark grey
    CHECK(s.accent == rgb(0xFF1F7A6B));   // teal-green
    CHECK(s.lcdBack == rgb(0xFF1A3A24));  // deep green
    CHECK(s.lcdInk == rgb(0xFF5CFF7A));   // green
}

TEST_CASE("faceplatespec: S1100 palette matches ui-design §3 verbatim",
          "[faceplatespec]")
{
    const auto& s = spec(ModelId::S1100);
    CHECK(s.chassis == rgb(0xFF9FA0A0));  // mid grey
    CHECK(s.legend == rgb(0xFF2A2A2A));   // dark grey
    CHECK(s.accent == rgb(0xFF27557E));   // blue
    CHECK(s.lcdBack == rgb(0xFF1A3A24));  // deep green
    CHECK(s.lcdInk == rgb(0xFF5CFF7A));   // green
}

TEST_CASE("faceplatespec: S3000 reserved-slot palette matches ui-design §3 "
          "verbatim",
          "[faceplatespec]")
{
    const auto& s = spec(ModelId::S3000);
    CHECK(s.chassis == rgb(0xFFC4C2BC));  // light grey
    CHECK(s.legend == rgb(0xFF2A2A2A));   // dark grey
    CHECK(s.accent == rgb(0xFF5A5470));   // violet-grey
    CHECK(s.lcdBack == rgb(0xFF1A3A24));  // deep green
    CHECK(s.lcdInk == rgb(0xFF5CFF7A));   // green
}

TEST_CASE("faceplatespec: derived colours are opaque chassis edge + translucent glow",
          "[faceplatespec]")
{
    for (const auto id : mws::model::kAllModels)
    {
        const auto& s = spec(id);
        CHECK(s.chassisEdge.isOpaque());
        CHECK(s.chassisEdge.getBrightness() < s.chassis.getBrightness());  // bevel shadow
        CHECK(s.lcdGlow.getAlpha() < 0xFF);  // glow is an overlay, never solid
        CHECK(s.lcdGlow.withAlpha(1.0f) == s.lcdInk.withAlpha(1.0f));  // glow = ink hue
    }
}

// ---------------------------------------------------------------------------
// Wordmarks + layout flags
// ---------------------------------------------------------------------------

TEST_CASE("faceplatespec: wordmarks are plain-text model descriptors, no Akai "
          "branding (ADR-005 clean room)",
          "[faceplatespec]")
{
    for (const auto id : mws::model::kAllModels)
    {
        const std::string_view wordmark = spec(id).wordmark;
        REQUIRE_FALSE(wordmark.empty());
        CHECK(wordmark.substr(0, mws::model::toString(id).size())
              == mws::model::toString(id));
        CHECK(wordmark.find("AKAI") == std::string_view::npos);
        CHECK(wordmark.find("Akai") == std::string_view::npos);
    }

    CHECK(std::string_view(spec(ModelId::S950).wordmark)
          == "S950 MIDI DIGITAL SAMPLER");  // §3 example, verbatim
    CHECK(std::string_view(spec(ModelId::S1000).wordmark)
          == "S1000 MIDI STEREO DIGITAL SAMPLER");  // §1 mockup header
}

TEST_CASE("faceplatespec: LCD layout class and mode/qual-width rows per model "
          "(ui-design §2, §3)",
          "[faceplatespec]")
{
    // S900/S950: 2-line display class, no CYCLIC/INTELL row, no qual/width.
    for (const auto id : { ModelId::S900, ModelId::S950 })
    {
        const auto& s = spec(id);
        CHECK(s.lcdLayout == LcdLayout::S950_2Line);
        CHECK_FALSE(s.showsModeRow);
        CHECK_FALSE(s.showsQualWidth);
    }

    // S1000-family: page layout with the mode row; qual/width greyed at v1.
    for (const auto id : { ModelId::S1000, ModelId::S1100, ModelId::S3000 })
    {
        const auto& s = spec(id);
        CHECK(s.lcdLayout == LcdLayout::S1000_Page);
        CHECK(s.showsModeRow);
        CHECK(s.showsQualWidth);
    }
}

TEST_CASE("faceplatespec: ParamVisibility follows the dsp-engine §2 applies-to "
          "column",
          "[faceplatespec]")
{
    // S900 — no timestretch (ADR-003); varispeed page: transpose + bandwidth.
    {
        const auto& v = spec(ModelId::S900).visibility;
        CHECK_FALSE(v.timeFactor);
        CHECK_FALSE(v.cycleLen);
        CHECK(v.transpose);
        CHECK_FALSE(v.material);
        CHECK(v.bandwidth);
        CHECK_FALSE(v.sampleRateSel);
    }
    // S950 — stretch + D-TIME + MON1/POL2 + bandwidth; no FS selector.
    {
        const auto& v = spec(ModelId::S950).visibility;
        CHECK(v.timeFactor);
        CHECK(v.cycleLen);
        CHECK(v.material);
        CHECK(v.bandwidth);
        CHECK_FALSE(v.sampleRateSel);
    }
    // S1000/S1100 — stretch + cycle + FS; no material/bandwidth (fixed-rate).
    for (const auto id : { ModelId::S1000, ModelId::S1100 })
    {
        const auto& v = spec(id).visibility;
        CHECK(v.timeFactor);
        CHECK(v.cycleLen);
        CHECK_FALSE(v.material);
        CHECK_FALSE(v.bandwidth);
        CHECK(v.sampleRateSel);
    }
}

// ---------------------------------------------------------------------------
// Geometry sanity — regions inside canvas, non-overlapping
// ---------------------------------------------------------------------------

TEST_CASE("faceplatespec: geometry constants sane — every region inside the "
          "1000x380 canvas",
          "[faceplatespec]")
{
    STATIC_REQUIRE(geo::kBaseWidth == 1000);
    STATIC_REQUIRE(geo::kBaseHeight == 380);
    STATIC_REQUIRE(geo::kCanvas.w == geo::kBaseWidth);
    STATIC_REQUIRE(geo::kCanvas.h == geo::kBaseHeight);

    for (const auto& region : geo::kAllRegions)
    {
        CHECK(region.w > 0);
        CHECK(region.h > 0);
        CHECK(geo::kCanvas.contains(region));
    }
}

TEST_CASE("faceplatespec: geometry constants sane — regions pairwise "
          "non-overlapping",
          "[faceplatespec]")
{
    for (std::size_t a = 0; a < geo::kAllRegions.size(); ++a)
        for (std::size_t b = a + 1; b < geo::kAllRegions.size(); ++b)
        {
            INFO("regions " << a << " and " << b << " overlap");
            CHECK_FALSE(geo::kAllRegions[a].intersects(geo::kAllRegions[b]));
        }
}

TEST_CASE("faceplatespec: scaledRegion maps base geometry onto a component size",
          "[faceplatespec]")
{
    // Identity at the 1.0-scale base size.
    const auto lcd1 = mws::ui::scaledRegion(geo::kLcd, geo::kBaseWidth, geo::kBaseHeight);
    CHECK(lcd1.getX() == (float) geo::kLcd.x);
    CHECK(lcd1.getWidth() == (float) geo::kLcd.w);

    // Proportional at 2x.
    const auto lcd2 =
        mws::ui::scaledRegion(geo::kLcd, geo::kBaseWidth * 2, geo::kBaseHeight * 2);
    CHECK(lcd2.getY() == (float) geo::kLcd.y * 2.0f);
    CHECK(lcd2.getHeight() == (float) geo::kLcd.h * 2.0f);
}

// ---------------------------------------------------------------------------
// Static layer render + repaint budget (ui-design §4: < 2 ms at 1.0 scale)
// ---------------------------------------------------------------------------

TEST_CASE("faceplatespec: cached static layer renders for every model and blits "
          "inside the 2 ms budget at 1.0 scale",
          "[faceplatespec]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    for (const auto id : mws::model::kAllModels)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto layer = mws::ui::renderFaceplateStaticLayer(spec(id), geo::kBaseWidth,
                                                               geo::kBaseHeight);
        const auto buildMs = juce::Time::getMillisecondCounterHiRes() - t0;

        REQUIRE_FALSE(layer.isNull());
        REQUIRE(layer.getWidth() == geo::kBaseWidth);
        REQUIRE(layer.getHeight() == geo::kBaseHeight);

        // The chassis colour must actually land on the plate (centre-left of
        // the header is plain chassis fill modulated by the sheen gradient).
        const auto painted = layer.getPixelAt(600, 240);
        CHECK(painted.isOpaque());

        // The cached-repaint path: blit the static layer into a target the
        // same way Faceplate::paint does, and hold it to the §4 budget.
        juce::Image target(juce::Image::ARGB, geo::kBaseWidth, geo::kBaseHeight, true,
                           juce::SoftwareImageType());
        double blitMs = 0.0;
        {
            juce::Graphics g(target);
            const auto b0 = juce::Time::getMillisecondCounterHiRes();
            g.drawImageAt(layer, 0, 0);
            blitMs = juce::Time::getMillisecondCounterHiRes() - b0;
        }

        std::cout << "faceplatespec timing [" << mws::model::toString(id)
                  << "]: static build " << buildMs << " ms, cached blit " << blitMs
                  << " ms\n";

        CHECK(blitMs < 2.0);  // ui-design §4 repaint budget at 1.0 scale
    }
}

// ---------------------------------------------------------------------------
// Visual-review snapshots (env-gated; never part of a normal test run's
// outputs). Set MWS_FACEPLATE_SNAPSHOT_DIR to dump a 1.0-scale PNG per model
// through the REAL Faceplate::paint path (cache build + blit). The repo
// itself stays free of raster assets (ADR-005) — these land outside it.
// ---------------------------------------------------------------------------

TEST_CASE("faceplatespec: snapshot dump for visual review when "
          "MWS_FACEPLATE_SNAPSHOT_DIR is set",
          "[faceplatespec]")
{
    const auto dir =
        juce::SystemStats::getEnvironmentVariable("MWS_FACEPLATE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
    {
        SUCCEED("MWS_FACEPLATE_SNAPSHOT_DIR not set — snapshot dump skipped");
        return;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto outDir = juce::File(dir);
    REQUIRE(outDir.createDirectory().wasOk());

    for (const auto id : mws::model::kAllModels)
    {
        mws::ui::Faceplate plate;
        plate.setModel(id);
        plate.setSize(geo::kBaseWidth, geo::kBaseHeight);  // 1.0 scale

        const auto snapshot =
            plate.createComponentSnapshot(plate.getLocalBounds(), false, 1.0f);
        REQUIRE_FALSE(snapshot.isNull());

        const auto file = outDir.getChildFile(
            "faceplate_" + juce::String(mws::model::toString(id).data()) + ".png");
        file.deleteFile();
        juce::FileOutputStream stream(file);
        REQUIRE(stream.openedOk());
        juce::PNGImageFormat png;
        REQUIRE(png.writeImageToStream(snapshot, stream));
        std::cout << "faceplatespec snapshot: " << file.getFullPathName() << "\n";
    }
}
