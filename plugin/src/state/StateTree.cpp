// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Non-parameter state tree implementation (task 029, architecture.md §6).

#include "StateTree.h"

#include "Migrations.h"

namespace mws::plugin::state {

namespace {

juce::ValueTree getOrCreateChild(juce::ValueTree& parent, const juce::Identifier& type)
{
    auto child = parent.getChildWithName(type);
    if (!child.isValid())
    {
        child = juce::ValueTree(type);
        parent.appendChild(child, nullptr);
    }
    return child;
}

void setIfMissing(juce::ValueTree& tree, const juce::Identifier& prop, const juce::var& value)
{
    if (!tree.hasProperty(prop))
        tree.setProperty(prop, value, nullptr);
}

} // namespace

juce::ValueTree createDefault()
{
    juce::ValueTree tree(id::stateTree);
    ensureDefaults(tree);
    return tree;
}

juce::ValueTree& ensureDefaults(juce::ValueTree& stateTree)
{
    setIfMissing(stateTree, id::stateVersion, kStateVersion);
    setIfMissing(stateTree, id::mode, defaults::mode);
    setIfMissing(stateTree, id::embedAudio, defaults::embedAudio);
    setIfMissing(stateTree, id::sourceBPM, defaults::sourceBPM);
    setIfMissing(stateTree, id::zoneStart, defaults::zoneStart);
    setIfMissing(stateTree, id::zoneEnd, defaults::zoneEnd);

    auto sourceFile = getOrCreateChild(stateTree, id::sourceFile);
    setIfMissing(sourceFile, id::path, juce::String());
    setIfMissing(sourceFile, id::contentHash, juce::String());

    auto uiState = getOrCreateChild(stateTree, id::uiState);
    setIfMissing(uiState, id::scaleFactor, defaults::scaleFactor);
    setIfMissing(uiState, id::lcdPage, defaults::lcdPage);

    // clampMemory: present but empty by default (no pre-clamp values stored).
    getOrCreateChild(stateTree, id::clampMemory);

    // renderMeta is deliberately NOT defaulted: its presence means "a render
    // exists" and drives re-render-on-load.
    return stateTree;
}

bool hasRenderMetadata(const juce::ValueTree& stateTree)
{
    return stateTree.getChildWithName(id::renderMeta)
        .hasProperty(id::engineVersionHash);
}

void setRenderMetadata(juce::ValueTree& stateTree,
                       std::uint64_t engineVersionHash,
                       const juce::String& paramsUsed)
{
    auto meta = getOrCreateChild(stateTree, id::renderMeta);
    meta.setProperty(id::engineVersionHash,
                     juce::String::toHexString(static_cast<juce::int64>(engineVersionHash)),
                     nullptr);
    meta.setProperty(id::paramsUsed, paramsUsed, nullptr);
}

void setClampMemory(juce::ValueTree& stateTree, mws::model::ModelId model, double value)
{
    auto memory = getOrCreateChild(stateTree, id::clampMemory);
    const auto name = mws::model::toString(model);
    memory.setProperty(juce::Identifier(juce::String(name.data(), name.size())),
                       value, nullptr);
}

double getClampMemory(const juce::ValueTree& stateTree,
                      mws::model::ModelId model,
                      double fallback)
{
    const auto memory = stateTree.getChildWithName(id::clampMemory);
    const auto name = mws::model::toString(model);
    return static_cast<double>(memory.getProperty(
        juce::Identifier(juce::String(name.data(), name.size())), fallback));
}

void writePluginState(const juce::ValueTree& apvtsState,
                      const juce::ValueTree& stateTree,
                      juce::MemoryBlock& destData,
                      const juce::MemoryBlock* embeddedAudioBlob)
{
    juce::ValueTree root(id::pluginState);
    root.appendChild(apvtsState.createCopy(), nullptr);
    root.appendChild(stateTree.createCopy(), nullptr);

    // Task-032 extension point: the pre-encoded FLAC blob (cached on the file
    // loader thread) is attached as-is — this path never encodes audio
    // (architecture.md §6).
    if (embeddedAudioBlob != nullptr && embeddedAudioBlob->getSize() > 0)
    {
        juce::ValueTree audio(id::embeddedAudio);
        audio.setProperty(id::blob, juce::var(*embeddedAudioBlob), nullptr);
        root.appendChild(audio, nullptr);
    }

    juce::MemoryOutputStream stream(destData, false);
    root.writeToStream(stream);
}

ReadResult readPluginState(const void* data, int sizeInBytes)
{
    ReadResult result;
    if (data == nullptr || sizeInBytes <= 0)
        return result;

    auto root = juce::ValueTree::readFromData(data, static_cast<size_t>(sizeInBytes));
    if (!root.isValid() || !root.hasType(id::pluginState))
        return result;

    // The APVTS child is whatever type the processor constructed the APVTS
    // with ("PARAMETERS") — identified as "the child that is neither the
    // state tree nor the embedded audio".
    for (const auto& child : root)
    {
        if (child.hasType(id::stateTree) || child.hasType(id::embeddedAudio))
            continue;
        result.apvtsState = child.createCopy();
        break;
    }

    auto stateTree = root.getChildWithName(id::stateTree).createCopy();
    if (!stateTree.isValid())
        stateTree = juce::ValueTree(id::stateTree); // legacy/garbled: rebuilt from defaults

    const int fromVersion = static_cast<int>(stateTree.getProperty(id::stateVersion, 0));
    stateTree = migrate(stateTree, fromVersion);
    ensureDefaults(stateTree);
    result.stateTree = stateTree;

    // Task-032 extension point: surface the embedded blob untouched.
    const auto audio = root.getChildWithName(id::embeddedAudio);
    if (const auto* blob = audio.getProperty(id::blob).getBinaryData())
        result.embeddedAudioBlob = *blob;

    result.valid = result.apvtsState.isValid() || result.stateTree.isValid();
    return result;
}

} // namespace mws::plugin::state
