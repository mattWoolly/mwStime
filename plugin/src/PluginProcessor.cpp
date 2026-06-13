// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace mws::plugin {

PluginProcessor::PluginProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()),
      params(apvts)
{
    // Listen for the non-automatable, latency-relevant inputs (dsp-engine.md
    // §7.4). These only change from the UI (message thread); the listener
    // reconfigures the FX engine off the audio thread (architecture.md §4) and
    // flags a latency re-report. Automatable params (timeFactor/cycleLen/hopMode)
    // are NOT listened for — they never change the reported latency.
    apvts.addParameterListener(paramid::model, this);
    apvts.addParameterListener(paramid::bandwidth, this);
    apvts.addParameterListener(paramid::sampleRateSel, this);
    apvts.addParameterListener(paramid::character, this);
}

PluginProcessor::~PluginProcessor()
{
    apvts.removeParameterListener(paramid::model, this);
    apvts.removeParameterListener(paramid::bandwidth, this);
    apvts.removeParameterListener(paramid::sampleRateSel, this);
    apvts.removeParameterListener(paramid::character, this);
}

void PluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Prepare the FX engine for the current parameters (preallocates the 30 s
    // history ring on this, the message thread) and report the exact ADR-006 /
    // §7.4 latency. setLatencySamples must be called from prepareToPlay (and on
    // model/bandwidth/FS change — handled in parameterChanged).
    const auto snapshot = params.makeSnapshot();
    const int latency = engine.prepareFx(sampleRate, samplesPerBlock,
                                         static_cast<std::size_t>(juce::jmax(
                                             1, getTotalNumInputChannels())),
                                         snapshot);
    setLatencySamples(latency);
}

void PluginProcessor::releaseResources() {}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Stereo in/out only for now (task 027); wider layouts are future work.
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

mws::engine::RealtimeStretcher::TransportInfo PluginProcessor::readTransport() noexcept
{
    mws::engine::RealtimeStretcher::TransportInfo t;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            t.playing = pos->getIsPlaying();
            if (const auto ppq = pos->getPpqPosition())
                t.ppqPosition = *ppq;
            if (const auto bpm = pos->getBpm())
                t.bpm = *bpm;
            if (const auto sig = pos->getTimeSignature())
            {
                t.timeSigNumerator = sig->numerator;
                t.timeSigDenominator = sig->denominator;
            }
        }
    }
    return t;
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                   juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages); // MIDI voice is task 035

    // Clear any output channels beyond the input count (defensive — the bus is
    // fixed stereo). No allocation on this path.
    for (int channel = getTotalNumInputChannels();
         channel < getTotalNumOutputChannels(); ++channel)
        buffer.clear(channel, 0, buffer.getNumSamples());

    const auto snapshot = params.makeSnapshot();

    if (snapshot.pluginMode == mws::engine::PluginMode::Sample)
    {
        // SAMPLE mode: the SamplePlayer is task 034. Until then the FX path is
        // bypassed and the dry signal passes through (explicit per ADR-006 mode
        // default — never silence).
        return;
    }

    // FX mode: run the RealtimeStretcher over the in-place buffer.
    engine.processFxBlock(buffer.getArrayOfWritePointers(),
                          getTotalNumInputChannels(), buffer.getNumSamples(),
                          snapshot, readTransport());
}

void PluginProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(parameterID, newValue);

    // Message thread (a non-automatable UI change). If the change actually
    // altered the model/bandwidth/FS/character configuration, the FX engine is
    // reconfigured (a fresh prepared RealtimeStretcher is published for the
    // audio thread to adopt — the task-030 RCU/graveyard handoff) and the new
    // latency is reported. If prepareToPlay has not run yet, only flag it; the
    // latency will be reported when prepareToPlay computes it.
    const auto snapshot = params.makeSnapshot();
    if (engine.reconfigureFxIfNeeded(snapshot))
        setLatencySamples(engine.fxLatencySamples());
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor(*this);
}

void PluginProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String PluginProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void PluginProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // APVTS + versioned non-parameter tree (task 029). The trailing nullptr
    // is the task-032 extension point: once the file loader caches a
    // pre-encoded FLAC blob, it is passed here verbatim — never encoded on
    // this (message) thread (architecture.md §6).
    state::writePluginState(apvts.copyState(), stateTree, destData,
                            /*embeddedAudioBlob=*/nullptr);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto restored = state::readPluginState(data, sizeInBytes);
    if (!restored.valid)
        return;

    if (restored.apvtsState.isValid() && restored.apvtsState.hasType(apvts.state.getType()))
        apvts.replaceState(restored.apvtsState);

    stateTree = restored.stateTree; // already migrated + defaults-filled

    // Re-render-on-load (architecture.md §6 determinism rule): render
    // metadata present means a render existed — request a deterministic
    // re-render instead of restoring stored output. The worker that services
    // this arrives in task 030; the callback defaults to a no-op.
    if (state::hasRenderMetadata(stateTree) && onReRenderRequested)
        onReRenderRequested();
}

} // namespace mws::plugin

// JUCE plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new mws::plugin::PluginProcessor();
}
