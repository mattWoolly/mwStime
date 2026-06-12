// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "Faceplate.h"

#include "lookandfeel/SeriesLookAndFeel.h"

namespace mws::ui {

namespace {

using geometry::RegionRect;

float scaleFor(int componentWidth) noexcept
{
    return (float) componentWidth / (float) geometry::kBaseWidth;
}

juce::Rectangle<float> mapRegion(const RegionRect& r, float sx, float sy) noexcept
{
    return { (float) r.x * sx, (float) r.y * sy, (float) r.w * sx, (float) r.h * sy };
}

// --- chassis ---------------------------------------------------------------

void drawChassis(juce::Graphics& g, const FaceplateSpec& spec,
                 juce::Rectangle<float> bounds)
{
    // Brushed-metal vertical sheen (PI).
    g.setGradientFill(juce::ColourGradient(spec.chassis.brighter(0.05f),
                                           bounds.getTopLeft(),
                                           spec.chassis.darker(0.07f),
                                           bounds.getBottomLeft(), false));
    g.fillRect(bounds);
}

void drawEdgeBevel(juce::Graphics& g, const FaceplateSpec& spec,
                   juce::Rectangle<float> bounds, float scale)
{
    const float bevel = juce::jmax(2.0f, 3.0f * scale);

    // Light catches the top/left edges; shadow falls bottom/right (PI).
    g.setColour(spec.chassis.brighter(0.35f));
    g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), bevel);
    g.setColour(spec.chassis.brighter(0.18f));
    g.fillRect(bounds.getX(), bounds.getY(), bevel, bounds.getHeight());
    g.setColour(spec.chassisEdge);
    g.fillRect(bounds.getX(), bounds.getBottom() - bevel, bounds.getWidth(), bevel);
    g.setColour(spec.chassisEdge.brighter(0.15f));
    g.fillRect(bounds.getRight() - bevel, bounds.getY(), bevel, bounds.getHeight());

    // Hard outer outline.
    g.setColour(spec.chassisEdge.darker(0.4f));
    g.drawRect(bounds, 1.0f);
}

// --- flavor ----------------------------------------------------------------

void drawScrew(juce::Graphics& g, const FaceplateSpec& spec, float cx, float cy,
               float radius)
{
    const auto base = spec.chassis.darker(0.45f);
    g.setGradientFill(juce::ColourGradient(base.brighter(0.5f), cx - radius * 0.4f,
                                           cy - radius * 0.4f, base.darker(0.4f),
                                           cx + radius * 0.6f, cy + radius * 0.6f, true));
    g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    g.setColour(spec.chassisEdge.darker(0.3f));
    g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.0f);

    // Slot at a jaunty period angle (PI).
    juce::Path slot;
    slot.addRoundedRectangle(-radius * 0.7f, -radius * 0.12f, radius * 1.4f,
                             radius * 0.24f, radius * 0.1f);
    g.setColour(spec.chassisEdge.darker(0.55f));
    g.fillPath(slot, juce::AffineTransform::rotation(0.6f).translated(cx, cy));
}

void drawVents(juce::Graphics& g, const FaceplateSpec& spec,
               juce::Rectangle<float> area)
{
    // Horizontal cooling slots between the jog well and the selector panel (PI).
    constexpr int slots = 6;
    const float slotH = area.getHeight() / (slots * 2.0f - 1.0f);
    for (int i = 0; i < slots; ++i)
    {
        const auto r = juce::Rectangle<float>(area.getX(),
                                              area.getY() + (float) i * 2.0f * slotH,
                                              area.getWidth(), slotH);
        g.setColour(spec.chassis.darker(0.5f));
        g.fillRoundedRectangle(r, slotH * 0.5f);
        g.setColour(spec.chassis.brighter(0.2f).withAlpha(0.6f));
        g.fillRect(r.getX(), r.getBottom(), r.getWidth(), 1.0f);
    }
}

// --- header strip ----------------------------------------------------------

void drawHeader(juce::Graphics& g, const FaceplateSpec& spec,
                juce::Rectangle<float> header, float scale)
{
    auto area = header.reduced(4.0f * scale, 2.0f * scale);

    // Power LED + label (§1 region 1).
    {
        auto ledArea = area.removeFromLeft(area.getHeight());
        const float r = juce::jmin(ledArea.getWidth(), ledArea.getHeight()) * 0.28f;
        const auto c = ledArea.getCentre();
        g.setColour(spec.accent.withAlpha(0.35f));  // glow halo
        g.fillEllipse(c.x - r * 1.8f, c.y - r * 1.8f, r * 3.6f, r * 3.6f);
        g.setColour(spec.accent.brighter(0.3f));
        g.fillEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f);
        g.setColour(spec.chassisEdge);
        g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.0f);

        g.setColour(spec.legend.withAlpha(0.9f));
        g.setFont(SeriesLookAndFeel::legendFont(7.5f * scale));
        g.drawText("POWER", ledArea.translated(0.0f, ledArea.getHeight() * 0.32f),
                   juce::Justification::centredBottom);
    }

    // Hamburger menu placeholder (about/scale/manual — wired later).
    {
        auto menu = area.removeFromRight(area.getHeight() * 1.1f).reduced(3.0f * scale);
        g.setColour(spec.legend.withAlpha(0.18f));
        g.fillRoundedRectangle(menu, 3.0f * scale);
        g.setColour(spec.legend.withAlpha(0.85f));
        g.drawRoundedRectangle(menu, 3.0f * scale, 1.0f);
        const auto bar = menu.reduced(menu.getWidth() * 0.25f, menu.getHeight() * 0.28f);
        for (int i = 0; i < 3; ++i)
            g.fillRoundedRectangle(bar.getX(),
                                   bar.getY() + (float) i * bar.getHeight() * 0.5f
                                       - 0.75f * scale,
                                   bar.getWidth(), 1.5f * scale, 0.75f * scale);
    }

    // Product name (left) — plain text, our own mark.
    auto nameArea = area.removeFromLeft(area.getWidth() * 0.22f);
    g.setColour(spec.legend);
    g.setFont(SeriesLookAndFeel::wordmarkFont(15.0f * scale));
    g.drawText("mwStime", nameArea, juce::Justification::centredLeft);

    // Model wordmark — plain text descriptor only, no Akai logo (ADR-005).
    g.setColour(spec.legend.withAlpha(0.92f));
    g.setFont(SeriesLookAndFeel::wordmarkFont(12.5f * scale));
    g.drawText(juce::String(spec.wordmark), area, juce::Justification::centred);

    // Hairline under the header.
    g.setColour(spec.chassisEdge.withAlpha(0.7f));
    g.fillRect(header.getX(), header.getBottom() + 1.0f, header.getWidth(), 1.0f);
}

// --- region frames -----------------------------------------------------------

/// Recessed panel frame: inset shadow top/left, light catch bottom/right.
void drawRecessedFrame(juce::Graphics& g, const FaceplateSpec& spec,
                       juce::Rectangle<float> r, float corner)
{
    g.setColour(spec.chassis.darker(0.18f));
    g.fillRoundedRectangle(r, corner);
    g.setColour(spec.chassisEdge);
    g.drawRoundedRectangle(r, corner, 1.2f);
    g.setColour(spec.chassis.brighter(0.25f).withAlpha(0.8f));
    g.drawLine(r.getX() + corner, r.getBottom() - 0.5f, r.getRight() - corner,
               r.getBottom() - 0.5f, 1.0f);
}

void drawLcdBezel(juce::Graphics& g, const FaceplateSpec& spec,
                  juce::Rectangle<float> r, float scale)
{
    const float corner = 5.0f * scale;

    // Outer bezel.
    g.setColour(spec.chassisEdge);
    g.fillRoundedRectangle(r, corner);
    g.setColour(spec.chassisEdge.darker(0.45f));
    g.drawRoundedRectangle(r, corner, 1.2f);

    // Glass: LCD background + backlight radial glow (contents are task 040).
    const auto glass = r.reduced(7.0f * scale);
    g.setColour(spec.lcdBack);
    g.fillRoundedRectangle(glass, corner * 0.6f);
    g.setGradientFill(juce::ColourGradient(spec.lcdGlow.withMultipliedAlpha(0.5f),
                                           glass.getCentreX(), glass.getCentreY(),
                                           spec.lcdGlow.withAlpha(0.0f),
                                           glass.getX(), glass.getY(), true));
    g.fillRoundedRectangle(glass, corner * 0.6f);

    // Top inset shadow on the glass.
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRect(glass.getX(), glass.getY(), glass.getWidth(), 2.0f * scale);
}

void drawFloppySlot(juce::Graphics& g, const FaceplateSpec& spec,
                    juce::Rectangle<float> r, float scale)
{
    drawRecessedFrame(g, spec, r, 4.0f * scale);

    // The drive slot itself.
    const auto slot = juce::Rectangle<float>(r.getX() + r.getWidth() * 0.08f,
                                             r.getCentreY() - 3.0f * scale,
                                             r.getWidth() * 0.66f, 6.0f * scale);
    g.setColour(spec.chassisEdge.darker(0.6f));
    g.fillRoundedRectangle(slot, 2.0f * scale);
    g.setColour(spec.chassis.brighter(0.2f));
    g.drawRoundedRectangle(slot, 2.0f * scale, 1.0f);

    // Eject button (PI flavor).
    const auto eject = juce::Rectangle<float>(r.getRight() - r.getWidth() * 0.16f,
                                              r.getCentreY() - 4.0f * scale,
                                              r.getWidth() * 0.08f, 8.0f * scale);
    g.setColour(spec.chassis.brighter(0.12f));
    g.fillRoundedRectangle(eject, 1.5f * scale);
    g.setColour(spec.chassisEdge);
    g.drawRoundedRectangle(eject, 1.5f * scale, 1.0f);

    g.setColour(spec.legend.withAlpha(0.55f));
    g.setFont(SeriesLookAndFeel::legendFont(8.0f * scale));
    g.drawText("DISK", r.reduced(6.0f * scale).removeFromTop(10.0f * scale),
               juce::Justification::topLeft);
}

void drawRegionCaption(juce::Graphics& g, const FaceplateSpec& spec, const char* text,
                       juce::Rectangle<float> r, float scale)
{
    g.setColour(spec.legend.withAlpha(0.55f));
    g.setFont(SeriesLookAndFeel::legendFont(8.0f * scale));
    g.drawText(text, r.reduced(6.0f * scale).removeFromTop(10.0f * scale),
               juce::Justification::topLeft);
}

} // namespace

// ---------------------------------------------------------------------------

juce::Rectangle<float> scaledRegion(const geometry::RegionRect& region,
                                    int componentWidth, int componentHeight) noexcept
{
    return mapRegion(region, (float) componentWidth / (float) geometry::kBaseWidth,
                     (float) componentHeight / (float) geometry::kBaseHeight);
}

juce::Image renderFaceplateStaticLayer(const FaceplateSpec& spec, int width, int height)
{
    juce::Image image(juce::Image::ARGB, juce::jmax(1, width), juce::jmax(1, height),
                      true, juce::SoftwareImageType());
    juce::Graphics g(image);

    const float sx = (float) width / (float) geometry::kBaseWidth;
    const float sy = (float) height / (float) geometry::kBaseHeight;
    const float scale = scaleFor(width);
    const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height);

    drawChassis(g, spec, bounds);
    drawEdgeBevel(g, spec, bounds, scale);

    // Corner screws (flavor).
    const float screwR = 5.0f * scale;
    const float inset = 11.0f * scale;
    drawScrew(g, spec, inset, inset, screwR);
    drawScrew(g, spec, (float) width - inset, inset, screwR);
    drawScrew(g, spec, inset, (float) height - inset, screwR);
    drawScrew(g, spec, (float) width - inset, (float) height - inset, screwR);

    drawHeader(g, spec, mapRegion(geometry::kHeader, sx, sy), scale);
    drawLcdBezel(g, spec, mapRegion(geometry::kLcd, sx, sy), scale);

    drawRecessedFrame(g, spec, mapRegion(geometry::kSoftKeys, sx, sy), 4.0f * scale);

    const auto waveform = mapRegion(geometry::kWaveform, sx, sy);
    drawRecessedFrame(g, spec, waveform, 4.0f * scale);
    drawRegionCaption(g, spec, "WAVEFORM / DROP SAMPLE HERE", waveform, scale);

    drawRecessedFrame(g, spec, mapRegion(geometry::kCursorKeys, sx, sy), 3.0f * scale);

    const auto jog = mapRegion(geometry::kJogWheel, sx, sy);
    drawRecessedFrame(g, spec, jog, 6.0f * scale);
    // Jog well: a recessed circle the wheel (task 042) sits in.
    {
        const auto well = jog.reduced(juce::jmin(jog.getWidth(), jog.getHeight()) * 0.12f);
        const float d = juce::jmin(well.getWidth(), well.getHeight());
        const auto circle = well.withSizeKeepingCentre(d, d);
        g.setColour(spec.chassis.darker(0.35f));
        g.fillEllipse(circle);
        g.setColour(spec.chassisEdge);
        g.drawEllipse(circle, 1.2f);
    }

    const auto selector = mapRegion(geometry::kModelSelector, sx, sy);
    drawRecessedFrame(g, spec, selector, 4.0f * scale);
    drawRegionCaption(g, spec, "MODEL SELECTOR", selector, scale);

    drawFloppySlot(g, spec, mapRegion(geometry::kFloppySlot, sx, sy), scale);

    // Vent slots between the jog well and the selector panel (flavor, PI).
    drawVents(g, spec,
              juce::Rectangle<float>(520.0f * sx, 262.0f * sy, 96.0f * sx, 102.0f * sy));

    return image;
}

// ---------------------------------------------------------------------------

Faceplate::Faceplate()
{
    setOpaque(true);
    setInterceptsMouseClicks(false, true);  // static chassis; children interact
}

void Faceplate::setModel(model::ModelId id)
{
    if (modelId == id)
        return;
    modelId = id;
    repaint();  // cache key mismatch forces a rebuild on the next paint
}

void Faceplate::resized()
{
    // Cache is keyed on size — the next paint rebuilds it (ui-design §4).
}

void Faceplate::rebuildCacheIfNeeded()
{
    if (! cache.isNull() && cache.getWidth() == getWidth()
        && cache.getHeight() == getHeight() && cachedModel == modelId)
        return;

#if JUCE_DEBUG
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
#endif

    cache = renderFaceplateStaticLayer(spec(), getWidth(), getHeight());
    cachedModel = modelId;

#if JUCE_DEBUG
    DBG("Faceplate: static layer rebuild ("
        << getWidth() << "x" << getHeight() << ", "
        << juce::String(model::toString(modelId).data()) << ") took "
        << juce::String(juce::Time::getMillisecondCounterHiRes() - t0, 3) << " ms");
#endif
}

void Faceplate::paint(juce::Graphics& g)
{
    rebuildCacheIfNeeded();

#if JUCE_DEBUG
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
#endif

    g.drawImageAt(cache, 0, 0);

#if JUCE_DEBUG
    // ui-design §4 budget: cached static-layer repaint < 2 ms at 1.0 scale.
    DBG("Faceplate: cached blit took "
        << juce::String(juce::Time::getMillisecondCounterHiRes() - t0, 3) << " ms");
#endif
}

} // namespace mws::ui
