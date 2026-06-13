// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "PluginEditor.h"

namespace mws::plugin {

namespace {

/// Static transcription of the ui-design §1 mockup TIME-STRETCH page — the
/// task-040 visual check. Page formatting from real parameter state is
/// LcdPageModel (task 041); cursor-key navigation is task 045.
void showMockupTimeStretchPage(ui::LcdDisplay& lcd)
{
    lcd.setLine(0, "TIME-STRETCH        sample: AMEN_165");
    lcd.setLine(2, "stretch zone:      0  to:  131071");
    lcd.setLine(3, "Cycle length:   1000  time factor: 300%");
    lcd.setLine(4, "stretch mode: CYCLIC qual: --  width: --");
    lcd.setLine(5, "new sample: AMEN_165*ST        mem:  7%");

    // qual/width grey out — "INTELL only" at v1; the hardware itself greys
    // them in CYCLIC mode [MAN §3 p.47].
    constexpr std::string_view row4 = "stretch mode: CYCLIC qual: --  width: --";
    for (int col = 21; col < (int) row4.size(); ++col)
        lcd.setCell(4, col, row4[(size_t) col], ui::LcdDisplay::Style::greyed);

    // Field cursor parked on the time-factor value (block-cursor visual).
    lcd.setCursor(3, 35);
}

} // namespace

PluginEditor::PluginEditor(PluginProcessor& processor)
    : juce::AudioProcessorEditor(processor)
{
    lookAndFeel.setSpec(faceplate.spec());
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(faceplate);

    lcd.setSpec(faceplate.spec());
    showMockupTimeStretchPage(lcd);
    addAndMakeVisible(lcd);

    // 1000×380 base canvas (ui-design §1, §5); resizability is task 047.
    setSize(ui::geometry::kBaseWidth, ui::geometry::kBaseHeight);
}

PluginEditor::~PluginEditor()
{
    setLookAndFeel(nullptr);
}

void PluginEditor::resized()
{
    faceplate.setBounds(getLocalBounds());

    // The LCD sits on the bezel's glass: the kLcd region inset by the same
    // 7 px (base-canvas) lip the Faceplate uses for the glass.
    const float scale = (float) getWidth() / (float) ui::geometry::kBaseWidth;
    lcd.setBounds(ui::scaledRegion(ui::geometry::kLcd, getWidth(), getHeight())
                      .reduced(7.0f * scale)
                      .toNearestInt());
}

} // namespace mws::plugin
