// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Non-parameter state tree (task 029) — the ValueTree under the APVTS root
// described in docs/design/architecture.md §6: mode, sourceFile (path +
// content hash), embedAudio, render metadata (engine version hash + params
// used), UI state (scale factor, LCD page), plus sourceBPM (task 037),
// zoneStart/zoneEnd (task 034) and the per-model clampMemory map (task 046 —
// a (PI) extension beyond the §6 field list, owned here so the schema has one
// authority). The root carries an explicit `stateVersion`; migrations live in
// Migrations.{h,cpp}.

#pragma once

#include <cstdint>

#include <juce_audio_processors/juce_audio_processors.h>

#include "mws/model/ModelId.h"

namespace mws::plugin::state {

/// Current schema version, written into every serialized state root.
/// Bump it together with a new explicit migration function in Migrations.cpp
/// (architecture.md §6 versioning rule).
inline constexpr int kStateVersion = 1;

/// Tree types and property names. These are serialized — never rename.
namespace id {
// Serialization root wrapping the APVTS state + the non-parameter tree.
inline const juce::Identifier pluginState { "mwStime" };

// Non-parameter state tree (child of the serialization root; lives next to
// the APVTS state inside the processor).
inline const juce::Identifier stateTree { "mwsState" };

// stateTree properties.
inline const juce::Identifier stateVersion { "stateVersion" };
inline const juce::Identifier mode         { "mode" };        // "FX" | "SAMPLE"
inline const juce::Identifier embedAudio   { "embedAudio" };  // bool, default on
inline const juce::Identifier sourceBPM    { "sourceBPM" };   // double, 0 = unknown (task 037)
inline const juce::Identifier zoneStart    { "zoneStart" };   // normalized 0..1 (task 034)
inline const juce::Identifier zoneEnd      { "zoneEnd" };     // normalized 0..1 (task 034)
// Set by Migrations when a future (unknown) stateVersion was replaced with
// safe defaults, so the UI can surface it.
inline const juce::Identifier unknownVersionFallback { "unknownVersionFallback" };

// sourceFile child: the loaded sample's identity.
inline const juce::Identifier sourceFile  { "sourceFile" };
inline const juce::Identifier path        { "path" };        // String
inline const juce::Identifier contentHash { "contentHash" }; // String (hex), "" = none

// renderMeta child: present only once a render exists. A reloaded session
// re-renders deterministically from this instead of storing rendered output
// (architecture.md §6 determinism rule).
inline const juce::Identifier renderMeta        { "renderMeta" };
inline const juce::Identifier engineVersionHash { "engineVersionHash" }; // String (hex of uint64)
inline const juce::Identifier paramsUsed        { "paramsUsed" };        // String (opaque snapshot encoding)

// uiState child.
inline const juce::Identifier uiState     { "uiState" };
inline const juce::Identifier scaleFactor { "scaleFactor" }; // double, default 1.0
inline const juce::Identifier lcdPage     { "lcdPage" };     // int, default 0

// clampMemory child (PI, task 046): per-model pre-clamp value map. Each
// property is a model name (mws::model::toString) holding the pre-clamp
// timeFactor the user had dialed before that model's hardware clamp applied.
inline const juce::Identifier clampMemory { "clampMemory" };

// Reserved child type for the cached FLAC blob (task 032) — see the
// embedded-audio extension point in writePluginState/readPluginState.
inline const juce::Identifier embeddedAudio { "embeddedAudio" };
inline const juce::Identifier blob          { "blob" }; // MemoryBlock var
} // namespace id

/// Field defaults (single authority for the schema).
namespace defaults {
inline constexpr const char* mode        = "FX";   // FX-first (locked decision)
inline constexpr bool        embedAudio  = true;   // default ON ≤ 16 MB encoded (dsp-engine.md §2)
inline constexpr double      sourceBPM   = 0.0;    // unknown
inline constexpr double      zoneStart   = 0.0;
inline constexpr double      zoneEnd     = 1.0;
inline constexpr double      scaleFactor = 1.0;
inline constexpr int         lcdPage     = 0;
} // namespace defaults

/// A fresh state tree with every field at its default and
/// stateVersion = kStateVersion.
[[nodiscard]] juce::ValueTree createDefault();

/// Fills in any missing properties/children with defaults, in place. Existing
/// values are never overwritten. Returns the same tree for chaining.
juce::ValueTree& ensureDefaults(juce::ValueTree& stateTree);

/// True when render metadata exists (an engineVersionHash was recorded) —
/// the re-render-on-load trigger.
[[nodiscard]] bool hasRenderMetadata(const juce::ValueTree& stateTree);

/// Records render metadata: the engine version hash and an opaque encoding of
/// the params used (written by the render worker, task 030).
void setRenderMetadata(juce::ValueTree& stateTree,
                       std::uint64_t engineVersionHash,
                       const juce::String& paramsUsed);

/// Per-model pre-clamp memory (task 046). value is the pre-clamp timeFactor.
void setClampMemory(juce::ValueTree& stateTree, mws::model::ModelId model, double value);

/// Returns the stored pre-clamp value for a model, or fallback if none.
[[nodiscard]] double getClampMemory(const juce::ValueTree& stateTree,
                                    mws::model::ModelId model,
                                    double fallback);

// --- Full plugin state serialization (getState/setStateInformation) --------

/// Serializes the APVTS state + the non-parameter state tree into one binary
/// blob under an id::pluginState root.
///
/// `embeddedAudioBlob` is the task-032 extension point: when the file loader
/// caches a pre-encoded FLAC blob, the processor passes it here and it is
/// stored as an id::embeddedAudio child — this function never encodes
/// anything itself (architecture.md §6: getStateInformation only memcpys).
/// Pass nullptr (the default) until task 032 lands.
void writePluginState(const juce::ValueTree& apvtsState,
                      const juce::ValueTree& stateTree,
                      juce::MemoryBlock& destData,
                      const juce::MemoryBlock* embeddedAudioBlob = nullptr);

/// Result of parsing a serialized plugin state.
struct ReadResult {
    bool valid = false;             ///< false: data was not a recognizable state blob
    juce::ValueTree apvtsState;     ///< the parameter tree, ready for APVTS::replaceState
    juce::ValueTree stateTree;      ///< migrated + defaults-filled non-parameter tree
    juce::MemoryBlock embeddedAudioBlob; ///< task-032 extension point; empty until then
};

/// Parses, migrates (Migrations.cpp) and defaults-fills a blob produced by
/// writePluginState. Never throws; result.valid is false on garbage input.
[[nodiscard]] ReadResult readPluginState(const void* data, int sizeInBytes);

} // namespace mws::plugin::state
