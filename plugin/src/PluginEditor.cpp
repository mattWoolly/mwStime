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

PluginEditor::PluginEditor(PluginProcessor& owner)
    : juce::AudioProcessorEditor(owner),
      controlPanel(owner.parameterState())
{
    // Sync the faceplate to the model parameter the panel restored/holds.
    faceplate.setModel(controlPanel.modelSelector().selectedModel());
    lookAndFeel.setSpec(faceplate.spec());
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(faceplate);
    // Input cluster (task 042). Binding the keys/wheel to pages and LCD
    // fields is tasks 045/045b — only placement + rendering here.
    addAndMakeVisible(softKeyBar);
    addAndMakeVisible(jogWheel);

    lcd.setSpec(faceplate.spec());
    showMockupTimeStretchPage(lcd);
    addAndMakeVisible(lcd);

    addAndMakeVisible(controlPanel);

    // Instant restyle on model switch (cross-fade + clamp-restore is task 046).
    controlPanel.onModelChanged = [this](mws::model::ModelId id) {
        faceplate.setModel(id);
        lcd.setSpec(faceplate.spec());
        lookAndFeel.setSpec(faceplate.spec());
        sendLookAndFeelChange();
        repaint();
    };

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

    // §1 mockup positions via the shared geometry constants. The SoftKeyBar
    // owns BOTH the soft-key row and the cursor/ENT cluster (ui-design §2),
    // so its bounds are the union of the two frames; the transparent band in
    // between does not intercept clicks (the waveform region 043 sits there).
    namespace geo = ui::geometry;
    const auto keysTop = ui::scaledRegion(geo::kSoftKeys, getWidth(), getHeight());
    const auto cursorBottom =
        ui::scaledRegion(geo::kCursorKeys, getWidth(), getHeight());
    softKeyBar.setBounds(keysTop.getUnion(cursorBottom).toNearestInt());

    jogWheel.setBounds(
        ui::scaledRegion(geo::kJogWheel, getWidth(), getHeight()).toNearestInt());

    controlPanel.setBounds(
        ui::scaledRegion(geo::kModelSelector, getWidth(), getHeight())
            .getSmallestIntegerContainer());
}

} // namespace mws::plugin
