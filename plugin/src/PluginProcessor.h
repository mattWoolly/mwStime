// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "state/Parameters.h"
#include "state/StateTree.h"

namespace mws::plugin {

/// Passthrough plugin shell (task 027) + the full APVTS parameter layout
/// (task 028, dsp-engine.md §2) + versioned non-parameter state with
/// migrations (task 029, architecture.md §6). Stereo in/out, MIDI input
/// accepted but ignored for now; no allocation in processBlock. The engine
/// arrives in tasks 030+.
class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    /// The unified §2 parameter set (fixed superset ranges, task 028).
    juce::AudioProcessorValueTreeState& parameterState() noexcept { return apvts; }

    /// The non-parameter state tree (task 029 schema, architecture.md §6).
    /// Message thread only.
    juce::ValueTree& nonParameterState() noexcept { return stateTree; }

    /// Re-render-on-load hook (architecture.md §6 determinism rule): invoked
    /// from setStateInformation when the restored state carries render
    /// metadata, so the session re-renders deterministically instead of
    /// restoring stored output. The render worker (task 030) installs the
    /// real handler; until then it stays a no-op.
    void setReRenderRequestCallback(std::function<void()> callback)
    {
        onReRenderRequested = std::move(callback);
    }

    /// Audio-thread snapshot bridge (plain atomic loads, architecture.md §4).
    [[nodiscard]] mws::engine::ParamSnapshot makeParamSnapshot() const noexcept
    {
        return params.makeSnapshot();
    }

private:
    juce::AudioProcessorValueTreeState apvts;
    Parameters params;
    juce::ValueTree stateTree = state::createDefault();
    std::function<void()> onReRenderRequested;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace mws::plugin
