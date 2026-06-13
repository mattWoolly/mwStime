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
    const auto channels =
        static_cast<std::size_t>(juce::jmax(1, getTotalNumInputChannels()));
    const int latency = engine.prepareFx(sampleRate, samplesPerBlock, channels, snapshot);
    setLatencySamples(latency);

    // SAMPLE-mode playback voice + ZONE-preview stretcher (task 034). Allocates
    // here, off the audio thread.
    engine.prepareSample(sampleRate, samplesPerBlock, channels, snapshot);

    // Mode-switch crossfade scratch (one block of the old path's output).
    fadeScratch_.setSize(getTotalNumOutputChannels(), samplesPerBlock,
                         /*keepExistingContent=*/false, /*clearExtraSpace=*/true,
                         /*avoidReallocating=*/false);
    prevMode_ = snapshot.pluginMode;
    haveProcessed_ = false;
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
    const int numChannels = getTotalNumInputChannels();
    const int numFrames = buffer.getNumSamples();
    const auto mode = snapshot.pluginMode;

    // Mode switch (FX<->SAMPLE): on the first block after a flip, render the OLD
    // path into the fade scratch, then the NEW path in place, and cross-fade
    // them over this one block (click-free — task 034 scope; a NAMED tunable
    // invention flagged for the task-034b PI audit). One block only.
    const bool modeSwitched = haveProcessed_ && mode != prevMode_;
    if (modeSwitched && numFrames <= fadeScratch_.getNumSamples()
        && numChannels <= fadeScratch_.getNumChannels())
    {
        // Old path into the scratch (a copy of the dry input — both paths read
        // the same input buffer).
        for (int ch = 0; ch < numChannels; ++ch)
            fadeScratch_.copyFrom(ch, 0, buffer, ch, 0, numFrames);
        renderMode(prevMode_, fadeScratch_.getArrayOfWritePointers(), numChannels,
                   numFrames, snapshot);

        // New path in place.
        renderMode(mode, buffer.getArrayOfWritePointers(), numChannels, numFrames,
                   snapshot);

        // Equal-power one-block crossfade: old fades out, new fades in.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* out = buffer.getWritePointer(ch);
            const float* old = fadeScratch_.getReadPointer(ch);
            for (int i = 0; i < numFrames; ++i)
            {
                const float t = (numFrames > 1)
                                    ? static_cast<float>(i) / static_cast<float>(numFrames - 1)
                                    : 1.0f;
                out[i] = old[i] * (1.0f - t) + out[i] * t;
            }
        }
    }
    else
    {
        renderMode(mode, buffer.getArrayOfWritePointers(), numChannels, numFrames,
                   snapshot);
    }

    prevMode_ = mode;
    haveProcessed_ = true;
}

void PluginProcessor::renderMode(mws::engine::PluginMode mode, float* const* channelData,
                                 int numChannels, int numFrames,
                                 const mws::engine::ParamSnapshot& snapshot) noexcept
{
    if (mode == mws::engine::PluginMode::Sample)
    {
        // SAMPLE mode: the SamplePlayer / ZONE-preview path (task 034). It
        // GENERATES audio (PLAY / A-B / ZONE) — it does not pass the host input
        // through (architecture.md §4).
        engine.processSampleBlock(channelData, numChannels, numFrames, snapshot);
        return;
    }

    // FX mode: run the RealtimeStretcher over the in-place buffer.
    engine.processFxBlock(channelData, numChannels, numFrames, snapshot, readTransport());
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
    // Persist the SYNC source BPM (task 037): the engine holds the live value
    // (set by typed/tap entry or the filename auto-guess) — mirror it into the
    // state tree so a reloaded session restores it (architecture.md §6 field).
    stateTree.setProperty(state::id::sourceBPM, engine.sourceBpm(), nullptr);

    // APVTS + versioned non-parameter tree (task 029) + the task-032 embedded
    // FLAC blob. The blob was pre-encoded on the file-loader thread and is
    // merely MEMCPY'd here — getStateInformation NEVER encodes on this (message)
    // thread (architecture.md §6; the host-autosave-stall failure the design
    // avoids). cachedBlob() is a plain atomic load; it is empty when embedding
    // is off or the encoded FLAC was over the 16 MB cap (path-only persistence).
    const auto blob = blobCache_.cachedBlob();
    state::writePluginState(apvts.copyState(), stateTree, destData,
                            blob != nullptr ? blob.get() : nullptr);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto restored = state::readPluginState(data, sizeInBytes);
    if (!restored.valid)
        return;

    if (restored.apvtsState.isValid() && restored.apvtsState.hasType(apvts.state.getType()))
        apvts.replaceState(restored.apvtsState);

    stateTree = restored.stateTree; // already migrated + defaults-filled

    // Restore the SYNC source BPM into the engine (task 037). A persisted,
    // non-zero value is an intentional user value, so it is marked user-set —
    // a later filename auto-guess must never clobber it (ui-design §6.2 step 4).
    const double restoredSourceBpm =
        static_cast<double>(stateTree.getProperty(state::id::sourceBPM,
                                                  state::defaults::sourceBPM));
    if (restoredSourceBpm > 0.0)
        engine.setSourceBpm(restoredSourceBpm, /*userSet=*/true);

    // Keep the blob cache's embed policy in step with the restored state-tree
    // flag (default ON ≤ 16 MB encoded, dsp-engine.md §2) before we restore.
    blobCache_.setEmbedEnabled(
        static_cast<bool>(stateTree.getProperty(state::id::embedAudio,
                                                state::defaults::embedAudio)));

    // Restore embedded audio (decode dispatched off the message thread) or
    // resolve the sourceFile path + verify its content hash; then fire the
    // deterministic re-render with the saved snapshot (architecture.md §6).
    // The processor's re-render hook is parameterless (task-029 contract); the
    // saved ParamSnapshot is the just-restored APVTS snapshot. This supersedes
    // the pre-032 hasRenderMetadata direct trigger (task 032 carries the
    // re-render through the restore callback).
    blobCache_.restore(restored.embeddedAudioBlob, stateTree, makeParamSnapshot(),
                       [this](const mws::engine::ParamSnapshot&) {
                           if (onReRenderRequested)
                               onReRenderRequested();
                       });
}

} // namespace mws::plugin

// JUCE plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new mws::plugin::PluginProcessor();
}
