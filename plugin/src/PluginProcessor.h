// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "EngineHost.h"
#include "state/Parameters.h"
#include "state/StateBlobCache.h"
#include "state/StateTree.h"

namespace mws::plugin {

/// The plugin shell (task 027) + the full APVTS parameter layout (task 028,
/// dsp-engine.md §2) + versioned non-parameter state with migrations (task 029)
/// + the FX-mode RealtimeStretcher path with latency reporting (task 033).
/// Stereo in/out, MIDI input accepted but ignored for now; processBlock is
/// allocation/lock/IO-free (architecture.md §4). SAMPLE-mode playback (034) is
/// dry passthrough for now (explicit per ADR-006 mode default).
class PluginProcessor final : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

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

    /// The threading + FX engine backbone (tasks 030/033). Editor/UI tasks
    /// reach the render worker, the FX scope FIFO, etc. through here.
    [[nodiscard]] EngineHost& engineHost() noexcept { return engine; }

    /// The embedded-audio state-blob cache (task 032). The FileLoader (owned by
    /// the editor/load-flow tasks) installs blobCache().sampleChangedHook() so
    /// the FLAC blob is pre-encoded off the message thread; getStateInformation
    /// only memcpys it (architecture.md §6).
    [[nodiscard]] state::StateBlobCache& blobCache() noexcept { return blobCache_; }

private:
    /// APVTS listener (message thread): a non-automatable, latency-relevant
    /// change (model/bandwidth/FS/character) reconfigures the FX engine and
    /// re-reports the latency. Automatable params never reach this path.
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    /// Reads the host transport (AudioPlayHead) into the engine's plain
    /// TransportInfo struct for SYNC mode (no JUCE inside the engine — §5.2).
    [[nodiscard]] mws::engine::RealtimeStretcher::TransportInfo readTransport() noexcept;

    juce::AudioProcessorValueTreeState apvts;
    Parameters params;
    juce::ValueTree stateTree = state::createDefault();
    std::function<void()> onReRenderRequested;

    EngineHost engine;
    state::StateBlobCache blobCache_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace mws::plugin
