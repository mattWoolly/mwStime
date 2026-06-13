// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Factory presets (task 038) — the four documented dsp-engine.md §9 v1
// validation presets, as data: "Jungle Amen 300", "S950 vocal 200",
// "S900 half-speed", "Dred vox". Each is a name → mws::engine::ParamSnapshot
// (the §2 defaults overlaid with the §9 deltas) plus the authentic plugin mode.
//
// These ship as JUCE programs (the host program API) AND double as the
// tests/golden/cases.json validation cases — task 038's test cross-checks the
// two so they can never silently drift. The S3000 "factory default" is v1.1
// (ADR-004) and is deliberately NOT here.
//
// Program layout: program 0 is "Default" (the §2 default snapshot — hosts
// expect ≥ 1 program and a sane 0); programs 1..N are the §9 presets in §9
// order. Preset indices are 0-based into the four §9 presets.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "mws/engine/Params.h"

namespace mws::plugin::presets {

/// Number of documented §9 factory presets (NOT counting the "Default"
/// program). This is exactly four at v1 (the S3000 default is v1.1, ADR-004).
[[nodiscard]] int numPresets() noexcept;

/// Display name of the §9 preset at `presetIndex` (0-based, §9 order), verbatim
/// from the dsp-engine.md §9 table. Empty for an out-of-range index.
[[nodiscard]] juce::String presetName(int presetIndex);

/// The full ParamSnapshot for the §9 preset at `presetIndex` (§2 defaults
/// overlaid with the §9 deltas + the authentic SAMPLE mode). For an
/// out-of-range index returns the §2 default snapshot.
[[nodiscard]] mws::engine::ParamSnapshot presetSnapshot(int presetIndex);

/// Convenience: the snapshot for a preset looked up by its §9 name. Returns the
/// §2 default snapshot if the name is unknown.
[[nodiscard]] mws::engine::ParamSnapshot snapshotFor(const juce::String& name);

// --- JUCE program API surface (program 0 = "Default", 1..N = §9 presets) ----

/// Total host-visible programs: 1 ("Default") + numPresets().
[[nodiscard]] int numPrograms() noexcept;

/// 1-based program index for the §9 preset at `presetIndex` (program 0 is
/// "Default"). i.e. programForPreset(0) == 1.
[[nodiscard]] int programForPreset(int presetIndex) noexcept;

/// Program name for the host (program 0 → "Default"; 1..N → the §9 preset
/// names). Empty for an out-of-range index.
[[nodiscard]] juce::String programName(int program);

/// Applies `program` to the plugin state ATOMICALLY: writes every snapshot
/// field through the APVTS (including the non-automatable model/pluginMode/
/// bandwidth/FS) and mirrors the non-parameter `mode` into the state tree, so a
/// program selection never leaves a partial state where APVTS and the state
/// tree disagree (architecture.md §6). Program 0 restores the §2 defaults.
/// Message thread only (mutates the APVTS and the ValueTree).
void applyProgram(int program,
                  juce::AudioProcessorValueTreeState& apvts,
                  juce::ValueTree& stateTree);

} // namespace mws::plugin::presets
