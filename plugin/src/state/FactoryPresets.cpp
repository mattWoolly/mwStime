// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Factory presets (task 038, dsp-engine.md §9). The §9 table, encoded as data:
// each entry starts from the ParamSnapshot §2 defaults and overlays only the
// §9 deltas, so a preset round-trips inert fields (e.g. "Jungle Amen 300"
// carries qual 20 / width 10, inert in CYCLIC but stored). applyProgram writes
// the snapshot through the APVTS and mirrors the non-parameter mode into the
// state tree.

#include "state/FactoryPresets.h"

#include <array>
#include <cmath>

#include "state/Parameters.h"
#include "state/StateTree.h"

namespace mws::plugin::presets {

namespace {

using mws::engine::ParamSnapshot;

/// One §9 preset: its display name and full snapshot.
struct PresetDef {
    const char* name;
    ParamSnapshot snapshot;
};

/// Builds the §2-default snapshot, overlays the §9 deltas for one preset, and
/// stamps the authentic SAMPLE mode (the §9 presets emulate the hardware
/// timestretch operating on a loaded sample — architecture.md §5.1).
[[nodiscard]] ParamSnapshot withSampleMode(ParamSnapshot s)
{
    s.pluginMode = mws::engine::PluginMode::Sample;
    return s;
}

// The §9 table, in §9 order. Each snapshot starts from the §2 defaults
// (ParamSnapshot's member initializers) and overlays only the §9 deltas — the
// SAME basis the golden cases.json loader uses, so the task-038 cross-check
// holds. Verbatim from dsp-engine.md §9; cross-checked against the matching
// tests/golden/cases.json entry by tests/plugin/test_presets.cpp.
const std::array<PresetDef, 4>& table()
{
    using namespace mws::engine;
    using model::ModelId;

    static const std::array<PresetDef, 4> presets = {{
        // "Jungle Amen 300" — S1100 CYCLIC, Cycle 1000, Time 300%, qual 20 /
        // width 10 stored but inert in CYCLIC [AKZ §9; DRR F10: SPOD].
        // (cases.json: s1100_jungle_amen_300)
        PresetDef{ "Jungle Amen 300", withSampleMode([] {
            ParamSnapshot s;            // §2 defaults
            s.model       = ModelId::S1100;
            s.stretchMode = StretchMode::Cyclic; // (default, stated for clarity)
            s.cycleLen    = 1000;                // (default)
            s.timeFactor  = 300.0;
            s.qual        = 20;                  // stored but inert in CYCLIC
            s.width       = 10;                  // (default; inert in CYCLIC)
            s.hopMode     = HopMode::Classic;    // (default)
            s.character   = true;                // (default)
            return s;
        }()) },

        // "S950 vocal 200" — S950 STRETCH 200%, D-TIME 1000, POL2 [MAN §2 p.30].
        // (cases.json: s950_vocal_200)
        PresetDef{ "S950 vocal 200", withSampleMode([] {
            ParamSnapshot s;
            s.model      = ModelId::S950;
            s.timeFactor = 200.0;
            s.cycleLen   = 1000;             // D-TIME ≡ cycle length in samples (§2/§5)
            s.material   = Material::Pol2;   // (default)
            s.hopMode    = HopMode::Classic; // (default)
            s.character  = true;             // (default)
            return s;
        }()) },

        // "S900 half-speed" — S900 timeFactor 200% (varispeed, -12 st emerges
        // from the engine — NOT a transpose value), BW max (16.0 kHz, the S900
        // ceiling) [ADR-003; §2]. (cases.json: s900_half_speed)
        PresetDef{ "S900 half-speed", withSampleMode([] {
            ParamSnapshot s;
            s.model      = ModelId::S900;
            s.timeFactor = 200.0;
            s.bandwidth  = 16.0;  // BW max for S900 (40 kHz rate ceiling, §2)
            s.transpose  = 0.0;   // (default) — pitch is a varispeed side-effect
            s.character  = true;  // (default)
            return s;
        }()) },

        // "Dred vox" — S1000 CYCLIC, Cycle 200, Time 400% (PI extremes)
        // [DRR F9 idiom]. (cases.json: s1000_dred_vox)
        PresetDef{ "Dred vox", withSampleMode([] {
            ParamSnapshot s;
            s.model       = ModelId::S1000;     // (default)
            s.stretchMode = StretchMode::Cyclic; // (default)
            s.cycleLen    = 200;
            s.timeFactor  = 400.0;
            s.hopMode     = HopMode::Classic;    // (default)
            s.character   = true;                // (default)
            return s;
        }()) },
    }};
    return presets;
}

/// Writes one unnormalized (plain) value to an APVTS parameter, host-notified.
void setPlain(juce::AudioProcessorValueTreeState& apvts, const char* id, float plainValue)
{
    if (auto* p = apvts.getParameter(id))
        p->setValueNotifyingHost(p->convertTo0to1(plainValue));
}

/// Pushes a full snapshot through the APVTS (every §2 row, including the
/// non-automatable model/pluginMode/bandwidth/FS) and mirrors the
/// non-parameter mode into the state tree. Choice indices match the layout
/// orders in Parameters.cpp (index-for-index with the core enums).
void writeSnapshot(const ParamSnapshot& s,
                   juce::AudioProcessorValueTreeState& apvts,
                   juce::ValueTree& stateTree)
{
    setPlain(apvts, paramid::model,         static_cast<float>(static_cast<int>(s.model)));
    setPlain(apvts, paramid::pluginMode,    static_cast<float>(static_cast<int>(s.pluginMode)));
    setPlain(apvts, paramid::timeFactor,    static_cast<float>(s.timeFactor));
    setPlain(apvts, paramid::cycleLen,      static_cast<float>(s.cycleLen));
    setPlain(apvts, paramid::stretchMode,   static_cast<float>(static_cast<int>(s.stretchMode)));
    setPlain(apvts, paramid::hopMode,       static_cast<float>(static_cast<int>(s.hopMode)));
    setPlain(apvts, paramid::transpose,     static_cast<float>(s.transpose));
    setPlain(apvts, paramid::qual,          static_cast<float>(s.qual));
    setPlain(apvts, paramid::width,         static_cast<float>(s.width));
    setPlain(apvts, paramid::material,      static_cast<float>(static_cast<int>(s.material)));
    setPlain(apvts, paramid::bandwidth,     static_cast<float>(s.bandwidth));
    setPlain(apvts, paramid::sampleRateSel, static_cast<float>(static_cast<int>(s.sampleRateSel)));
    setPlain(apvts, paramid::character,     s.character ? 1.0f : 0.0f);
    setPlain(apvts, paramid::norm,          s.norm ? 1.0f : 0.0f);
    setPlain(apvts, paramid::tempoSync,     static_cast<float>(static_cast<int>(s.tempoSync)));
    setPlain(apvts, paramid::fxWindow,      static_cast<float>(static_cast<int>(s.fxWindow)));
    setPlain(apvts, paramid::outTrim,       static_cast<float>(s.outTrim));

    // Mirror the non-parameter mode (the state-tree `mode` property serialized
    // alongside the APVTS — StateTree.h) so APVTS and the state tree never
    // disagree (no partial states, task-038 scope).
    stateTree.setProperty(state::id::mode,
                          s.pluginMode == mws::engine::PluginMode::Sample ? "SAMPLE" : "FX",
                          nullptr);
}

} // namespace

int numPresets() noexcept
{
    return static_cast<int>(table().size());
}

juce::String presetName(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= numPresets())
        return {};
    return table()[static_cast<std::size_t>(presetIndex)].name;
}

ParamSnapshot presetSnapshot(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= numPresets())
        return ParamSnapshot{};
    return table()[static_cast<std::size_t>(presetIndex)].snapshot;
}

ParamSnapshot snapshotFor(const juce::String& name)
{
    for (int i = 0; i < numPresets(); ++i)
        if (presetName(i) == name)
            return presetSnapshot(i);
    return ParamSnapshot{};
}

int numPrograms() noexcept
{
    return 1 + numPresets(); // program 0 = "Default", 1..N = §9 presets
}

int programForPreset(int presetIndex) noexcept
{
    return presetIndex + 1; // program 0 is reserved for "Default"
}

juce::String programName(int program)
{
    if (program == 0)
        return "Default";
    return presetName(program - 1); // empty for out-of-range
}

void applyProgram(int program,
                  juce::AudioProcessorValueTreeState& apvts,
                  juce::ValueTree& stateTree)
{
    if (program <= 0 || program > numPresets())
    {
        // Program 0 ("Default") or out-of-range → the §2 default snapshot.
        writeSnapshot(ParamSnapshot{}, apvts, stateTree);
        return;
    }
    writeSnapshot(presetSnapshot(program - 1), apvts, stateTree);
}

} // namespace mws::plugin::presets
