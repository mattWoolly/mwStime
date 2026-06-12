// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// SeriesLookAndFeel — the pure-vector period LookAndFeel (ADR-005;
// docs/design/ui-design.md §4). Everything is drawn with juce::Graphics/Path;
// no image assets, no font files (stock sans-serif only). Restyled per model
// from a FaceplateSpec — child widgets (soft keys 042, knobs 044, jog 042)
// pick the palette up from here.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../FaceplateSpec.h"

namespace mws::ui {

class SeriesLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    /// Defaults to the S1000 spec (the canonical grey + green look).
    SeriesLookAndFeel();

    /// Re-skins the LookAndFeel from a model's FaceplateSpec (palette only —
    /// geometry never forks per model, ui-design §3).
    void setSpec(const FaceplateSpec& spec);
    [[nodiscard]] const FaceplateSpec& spec() const noexcept { return *current; }

    // --- period typography (stock sans-serif, no font files — ADR-005) -----
    /// Small panel legend lettering (key labels, region captions).
    [[nodiscard]] static juce::Font legendFont(float height);
    /// Header wordmark lettering — bold, letter-spaced like the period badges.
    [[nodiscard]] static juce::Font wordmarkFont(float height);

    // --- drawing primitives -------------------------------------------------
    /// Rounded-rect key cap with top-light/bottom-shadow bevel (soft keys,
    /// cursor/ENT keys, selector badges).
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    /// Period collet knob: dark cap, ring, legend-colour pointer, accent arc.
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override;

    /// Recessed track + rectangular fader cap.
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override;

private:
    void applyPalette();

    const FaceplateSpec* current = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SeriesLookAndFeel)
};

} // namespace mws::ui
