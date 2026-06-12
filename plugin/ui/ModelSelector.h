// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ModelSelector — latching rack-badge model tabs (docs/design/ui-design.md §1
// region 4, §2 "Model selector" row): four shipping badges S900/S950/S1000/
// S1100 plus a blank, disabled fifth badge position reserved for the S3000
// (ADR-004 — slot present, no behavior at v1). Selection writes the
// NON-automatable `model` choice parameter via a manual juce::
// ParameterAttachment (dsp-engine.md §2 non-automatable set — no APVTS
// attachment class is used for it, task 044). Visual latching uses each
// model's own FaceplateSpec accent colour (ui-design §3 palette table).
//
// Cross-fade of the faceplate on switch is task 046 — this component only
// reads/writes the parameter and latches the badges.

#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FaceplateSpec.h"

#include "mws/model/ModelId.h"

namespace mws::ui {

class ModelSelector final : public juce::Component
{
public:
    /// `modelParam` must be the non-automatable `model` choice parameter
    /// (mws::plugin::paramid::model). Message thread only.
    explicit ModelSelector(juce::RangedAudioParameter& modelParam);

    /// Notified (message thread) after the model parameter changes, with the
    /// new id — the editor restyles faceplate/LookAndFeel from it.
    std::function<void(model::ModelId)> onModelChanged;

    /// Currently latched model (mirrors the parameter).
    [[nodiscard]] model::ModelId selectedModel() const noexcept { return selected; }

    /// Badge access by table position (kAllModels order; index 4 is the
    /// reserved blank S3000 slot — disabled, empty text). For layout/tests.
    [[nodiscard]] juce::TextButton& badge(std::size_t index) noexcept
    {
        return *badges[index];
    }
    [[nodiscard]] static constexpr std::size_t badgeCount() noexcept
    {
        return model::kModelCount;
    }

    void resized() override;

private:
    void latch(model::ModelId id);

    std::array<std::unique_ptr<juce::TextButton>, model::kModelCount> badges;
    juce::ParameterAttachment attachment;  // manual — `model` is non-automatable
    model::ModelId selected = model::ModelId::S1000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelSelector)
};

} // namespace mws::ui
