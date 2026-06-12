// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "../ui/Faceplate.h"
#include "../ui/lookandfeel/SeriesLookAndFeel.h"
#include "PluginProcessor.h"

namespace mws::plugin {

/// 1000×380 editor showing the cached vector S-series faceplate (task 039;
/// docs/design/ui-design.md). Child components (LCD, soft keys, jog, waveform,
/// selector) arrive in tasks 040–044; assembly + resize in 045/047.
class PluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor(PluginProcessor& processor);
    ~PluginEditor() override;

    void resized() override;

private:
    ui::SeriesLookAndFeel lookAndFeel;
    ui::Faceplate faceplate;  // defaults to S1000 (the canonical look)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

} // namespace mws::plugin
