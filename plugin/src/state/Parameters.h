// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// APVTS parameter layout (task 028) — implements docs/design/dsp-engine.md §2
// row by row: fixed superset ranges (never remapped at runtime — VST3 hosts
// cache parameter info, architecture.md §6), hardware-unit string conversions
// (exactly what the LCD shows), and the non-automatable set. The active
// ModelSpec clamps at the engine; nothing here remaps a range per model.

#pragma once

#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>

#include "mws/engine/Params.h"

namespace mws::plugin {

/// Parameter IDs, verbatim from the dsp-engine.md §2 table. These are
/// host-visible and serialized — never rename.
namespace paramid {
inline constexpr const char* model         = "model";
inline constexpr const char* pluginMode    = "pluginMode";
inline constexpr const char* timeFactor    = "timeFactor";
inline constexpr const char* cycleLen      = "cycleLen";
inline constexpr const char* stretchMode   = "stretchMode";
inline constexpr const char* hopMode       = "hopMode";
inline constexpr const char* transpose     = "transpose";
inline constexpr const char* qual          = "qual";
inline constexpr const char* width         = "width";
inline constexpr const char* material      = "material";
inline constexpr const char* autoCycle     = "autoCycle";
inline constexpr const char* bandwidth     = "bandwidth";
inline constexpr const char* sampleRateSel = "sampleRateSel";
inline constexpr const char* character     = "character";
inline constexpr const char* norm          = "norm";
inline constexpr const char* tempoSync     = "tempoSync";
inline constexpr const char* fxWindow      = "fxWindow";
inline constexpr const char* outTrim       = "outTrim";
inline constexpr const char* embedAudio    = "embedAudio";
} // namespace paramid

/// Builds the full §2 layout. Choice orders match the corresponding core
/// enums (mws::engine::* / mws::model::ModelId) index-for-index — the
/// snapshot bridge casts indices directly.
[[nodiscard]] juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/// Audio-thread bridge: caches the std::atomic<float>* raw-value pointers
/// once (message thread, construction time) so makeSnapshot() is plain
/// relaxed atomic loads — no ValueTree access, no allocation, no locking
/// (architecture.md §4).
class Parameters
{
public:
    /// Must be constructed after the APVTS owns the layout from
    /// createParameterLayout(). Message thread only.
    explicit Parameters(juce::AudioProcessorValueTreeState& apvts);

    /// Real-time safe: plain atomic loads into a trivially-copyable POD.
    [[nodiscard]] mws::engine::ParamSnapshot makeSnapshot() const noexcept;

    /// Real-time safe peek at the momentary autoCycle trigger (§2 trigger
    /// row). The detector consumes it and the message thread self-resets the
    /// parameter (soft-key wiring is task 045b); it is not part of
    /// ParamSnapshot.
    [[nodiscard]] bool autoCyclePending() const noexcept;

private:
    std::atomic<float>* model_         = nullptr;
    std::atomic<float>* pluginMode_    = nullptr;
    std::atomic<float>* timeFactor_    = nullptr;
    std::atomic<float>* cycleLen_      = nullptr;
    std::atomic<float>* stretchMode_   = nullptr;
    std::atomic<float>* hopMode_       = nullptr;
    std::atomic<float>* transpose_     = nullptr;
    std::atomic<float>* qual_          = nullptr;
    std::atomic<float>* width_         = nullptr;
    std::atomic<float>* material_      = nullptr;
    std::atomic<float>* autoCycle_     = nullptr;
    std::atomic<float>* bandwidth_     = nullptr;
    std::atomic<float>* sampleRateSel_ = nullptr;
    std::atomic<float>* character_     = nullptr;
    std::atomic<float>* norm_          = nullptr;
    std::atomic<float>* tempoSync_     = nullptr;
    std::atomic<float>* fxWindow_      = nullptr;
    std::atomic<float>* outTrim_       = nullptr;
};

/// Free-function form of the snapshot bridge (task-028 spec signature).
[[nodiscard]] inline mws::engine::ParamSnapshot makeSnapshot(const Parameters& params) noexcept
{
    return params.makeSnapshot();
}

} // namespace mws::plugin
