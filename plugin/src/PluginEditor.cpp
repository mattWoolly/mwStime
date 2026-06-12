// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "PluginEditor.h"

namespace mws::plugin {

PluginEditor::PluginEditor(PluginProcessor& processor)
    : juce::AudioProcessorEditor(processor)
{
    lookAndFeel.setSpec(faceplate.spec());
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(faceplate);

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
}

} // namespace mws::plugin
