// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "../ui/ControlPanel.h"
#include "../ui/Faceplate.h"
#include "../ui/JogWheel.h"
#include "../ui/LcdDisplay.h"
#include "../ui/SoftKeyBar.h"
#include "../ui/lookandfeel/SeriesLookAndFeel.h"
#include "PluginProcessor.h"

namespace mws::plugin {

/// 1000×380 editor showing the cached vector S-series faceplate (task 039;
/// docs/design/ui-design.md), the LCD hero component (task 040), the
/// input cluster — soft keys F1–F8, cursor/ENT keys, jog wheel (task 042) —
/// and the right-hand model-selector/control panel (task 044) at the §1
/// mockup positions. Assembly + page binding + resize in 045/045b/047.
class PluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor(PluginProcessor& owner);
    ~PluginEditor() override;

    void resized() override;

private:
    ui::SeriesLookAndFeel lookAndFeel;
    ui::Faceplate faceplate;    // defaults to S1000 (the canonical look)
    ui::LcdDisplay lcd;         // dynamic layer on the faceplate's LCD glass
    ui::SoftKeyBar softKeyBar;  // F1–F8 + cursor/ENT cluster (task 042)
    ui::JogWheel jogWheel;      // endless data wheel (task 042)
    ui::ControlPanel controlPanel;         // model selector + right-panel controls (task 044)
    juce::TooltipWindow tooltips{ this };  // ui-design §7 tooltips

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

} // namespace mws::plugin
