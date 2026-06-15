// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "SeriesLookAndFeel.h"

namespace mws::ui {

namespace {

/// Key-cap colour: the chassis nudged toward the legend colour, so caps read
/// slightly lighter on dark chassis (S900/S950) and slightly darker on light
/// chassis (S1000/S1100) — the period contrast direction either way (PI).
juce::Colour keyCapColour(const FaceplateSpec& spec)
{
    return spec.chassis.interpolatedWith(spec.legend, 0.12f);
}

} // namespace

SeriesLookAndFeel::SeriesLookAndFeel()
{
    setSpec(faceplateSpecFor(model::ModelId::S1000));
}

void SeriesLookAndFeel::setSpec(const FaceplateSpec& spec)
{
    current = &spec;
    applyPalette();
}

void SeriesLookAndFeel::applyPalette()
{
    const auto& s = *current;

    setColour(juce::ResizableWindow::backgroundColourId, s.chassis);
    setColour(juce::Label::textColourId, s.legend);

    setColour(juce::TextButton::buttonColourId, keyCapColour(s));
    setColour(juce::TextButton::buttonOnColourId, s.accent);
    setColour(juce::TextButton::textColourOffId, s.legend);
    setColour(juce::TextButton::textColourOnId, s.legend);
    setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::DrawableButton::backgroundOnColourId, s.accent.withAlpha(0.35f));

    setColour(juce::Slider::rotarySliderFillColourId, s.accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, s.chassisEdge);
    setColour(juce::Slider::thumbColourId, s.legend);
    setColour(juce::Slider::trackColourId, s.chassisEdge);
    setColour(juce::Slider::backgroundColourId, s.chassis.darker(0.25f));

    setColour(juce::TooltipWindow::backgroundColourId, s.lcdBack);
    setColour(juce::TooltipWindow::textColourId, s.lcdInk);
}

juce::Font SeriesLookAndFeel::legendFont(float height)
{
    // Stock sans-serif only — no font files in the repo (ADR-005).
    return juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(),
                                        height, juce::Font::plain));
}

juce::Font SeriesLookAndFeel::wordmarkFont(float height)
{
    auto font = juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(),
                                             height, juce::Font::bold));
    // Letter-spaced like the period front-panel badges (PI).
    return font.withExtraKerningFactor(0.18f);
}

void SeriesLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& backgroundColour,
                                             bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    const float corner = juce::jmin(4.0f, bounds.getHeight() * 0.2f);

    auto cap = backgroundColour;
    if (shouldDrawButtonAsDown)
        cap = cap.darker(0.25f);
    else if (shouldDrawButtonAsHighlighted)
        cap = cap.brighter(0.08f);

    // Mode-disabled (greyed) keys read as visibly greyed (task 057,
    // ui-design §6.4): the whole cap desaturates toward the chassis so the user
    // sees the key is mode-gated, not broken — paired with the dimmed
    // legend/text. The SoftKeyBar keeps the JUCE button enabled (so a press
    // still reaches pressKey for the gated-key LCD hint) and flags the greyed
    // state via kModeDisabledProp, which we read here (a plain !isEnabled()
    // also greys, e.g. for non-soft-key buttons).
    const bool greyed =
        ! button.isEnabled()
        || static_cast<bool>(button.getProperties().getWithDefault(
               "mwsSoftKeyModeDisabled", false));
    if (greyed)
        cap = current->chassis.interpolatedWith(cap.withSaturation(0.0f), 0.6f);

    // Key-cap body with a vertical sheen — period moulded plastic (PI).
    g.setGradientFill(juce::ColourGradient(cap.brighter(0.10f), bounds.getTopLeft(),
                                           cap.darker(0.12f), bounds.getBottomLeft(),
                                           false));
    g.fillRoundedRectangle(bounds, corner);

    // Bevel: light top edge, shadow bottom edge (flipped when pressed).
    const auto topEdge = shouldDrawButtonAsDown ? cap.darker(0.5f) : cap.brighter(0.45f);
    const auto bottomEdge = shouldDrawButtonAsDown ? cap.brighter(0.25f) : cap.darker(0.55f);
    g.setColour(topEdge);
    g.drawLine(bounds.getX() + corner, bounds.getY() + 0.75f,
               bounds.getRight() - corner, bounds.getY() + 0.75f, 1.2f);
    g.setColour(bottomEdge);
    g.drawLine(bounds.getX() + corner, bounds.getBottom() - 0.75f,
               bounds.getRight() - corner, bounds.getBottom() - 0.75f, 1.2f);

    // Surrounding recess outline.
    g.setColour(current->chassisEdge.withAlpha(0.85f));
    g.drawRoundedRectangle(bounds, corner, 1.0f);

    // Latched state: accent indicator bar along the cap top (PI).
    if (button.getToggleState())
    {
        g.setColour(current->accent);
        g.fillRoundedRectangle(bounds.getX() + corner, bounds.getY() + 2.0f,
                               bounds.getWidth() - 2.0f * corner, 2.5f, 1.0f);
    }
}

void SeriesLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                       bool /*shouldDrawButtonAsHighlighted*/,
                                       bool shouldDrawButtonAsDown)
{
    // Greyed text for a disabled OR mode-gated (kModeDisabledProp) key — the
    // 0.4 dim matches SoftKeyBar::kDisabledDim (task 057).
    const bool greyed =
        ! button.isEnabled()
        || static_cast<bool>(button.getProperties().getWithDefault(
               "mwsSoftKeyModeDisabled", false));
    const auto textColour =
        button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                  : juce::TextButton::textColourOffId)
            .withMultipliedAlpha(greyed ? 0.4f : 1.0f);

    g.setColour(textColour);
    g.setFont(legendFont(juce::jmin(13.0f, (float) button.getHeight() * 0.5f)));

    auto area = button.getLocalBounds().reduced(2);
    if (shouldDrawButtonAsDown)
        area.translate(0, 1);  // pressed caps sink (PI)
    g.drawFittedText(button.getButtonText(), area, juce::Justification::centred, 1);
}

void SeriesLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width,
                                         int height, float sliderPosProportional,
                                         float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider& /*slider*/)
{
    const auto& s = *current;
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle =
        rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // Recessed accent arc behind the knob.
    {
        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle,
                          rotaryEndAngle, true);
        g.setColour(s.chassisEdge);
        g.strokePath(arc, juce::PathStrokeType(2.5f));

        juce::Path fill;
        fill.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle,
                           angle, true);
        g.setColour(s.accent);
        g.strokePath(fill, juce::PathStrokeType(2.5f));
    }

    // Knob body: dark collet cap with a radial sheen (period plastic, PI).
    const float knobRadius = radius * 0.78f;
    const auto knobBase = s.chassis.darker(0.65f);
    g.setGradientFill(juce::ColourGradient(
        knobBase.brighter(0.35f), centre.x - knobRadius * 0.4f,
        centre.y - knobRadius * 0.4f, knobBase.darker(0.3f),
        centre.x + knobRadius * 0.7f, centre.y + knobRadius * 0.7f, true));
    g.fillEllipse(centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f,
                  knobRadius * 2.0f);
    g.setColour(s.chassisEdge);
    g.drawEllipse(centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f,
                  knobRadius * 2.0f, 1.2f);

    // Pointer line in legend colour.
    juce::Path pointer;
    pointer.addRoundedRectangle(-1.5f, -knobRadius + 2.0f, 3.0f, knobRadius * 0.45f, 1.0f);
    g.setColour(s.legend);
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre));
}

void SeriesLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width,
                                         int height, float sliderPos,
                                         float /*minSliderPos*/, float /*maxSliderPos*/,
                                         juce::Slider::SliderStyle style,
                                         juce::Slider& slider)
{
    const auto& s = *current;
    const bool vertical = style == juce::Slider::LinearVertical;
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();

    // Recessed slot track.
    auto track = vertical ? bounds.withSizeKeepingCentre(4.0f, bounds.getHeight() - 8.0f)
                          : bounds.withSizeKeepingCentre(bounds.getWidth() - 8.0f, 4.0f);
    g.setColour(s.chassis.darker(0.55f));
    g.fillRoundedRectangle(track, 2.0f);
    g.setColour(s.chassisEdge);
    g.drawRoundedRectangle(track, 2.0f, 1.0f);

    // Rectangular fader cap with a centre accent line (PI).
    const auto capColour = keyCapColour(s);
    auto cap = vertical
                   ? juce::Rectangle<float>(bounds.getX() + 2.0f, sliderPos - 5.0f,
                                            bounds.getWidth() - 4.0f, 10.0f)
                   : juce::Rectangle<float>(sliderPos - 5.0f, bounds.getY() + 2.0f, 10.0f,
                                            bounds.getHeight() - 4.0f);
    g.setColour(capColour);
    g.fillRoundedRectangle(cap, 2.0f);
    g.setColour(s.chassisEdge);
    g.drawRoundedRectangle(cap, 2.0f, 1.0f);
    g.setColour(slider.isEnabled() ? s.accent : s.legend.withAlpha(0.4f));
    if (vertical)
        g.fillRect(cap.getX() + 1.5f, cap.getCentreY() - 0.75f, cap.getWidth() - 3.0f, 1.5f);
    else
        g.fillRect(cap.getCentreX() - 0.75f, cap.getY() + 1.5f, 1.5f, cap.getHeight() - 3.0f);
}

} // namespace mws::ui
