// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "PluginEditor.h"

#include <mws/core/Version.h>

namespace mws::plugin {

PluginEditor::PluginEditor(PluginProcessor& processor)
    : juce::AudioProcessorEditor(processor)
{
    setSize(1000, 380);
}

void PluginEditor::paint(juce::Graphics& g)
{
    // Placeholder paint — the skeuomorphic S-series faceplate is later work.
    // Referencing mwstime_core here proves the core/plugin layering links.
    g.fillAll(juce::Colour(0xff3a3d40)); // S-series grey chassis placeholder

    g.setColour(juce::Colour(0xff7bd96a)); // green LCD placeholder
    g.setFont(juce::FontOptions(24.0f));
    const auto engineVersion = mws::core::engineVersion();
    g.drawText("mwStime — engine v"
                   + juce::String(engineVersion.data(), engineVersion.size()),
               getLocalBounds(), juce::Justification::centred);
}

void PluginEditor::resized() {}

} // namespace mws::plugin
