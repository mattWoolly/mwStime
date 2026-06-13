// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ControlPanel — the right-hand plugin-level control panel (docs/design/
// ui-design.md §1 region 4, task 044): ModelSelector badges on top, then
// MODE FX/SAMPLE, TIMING CLASSIC/REVISED, CHARACTER on/off, FX WINDOW
// selector (enabled only in FX mode), the context-dependent BANDWIDTH
// (S900/S950) / FS 44.1/22.05 (S1000/S1100) row, and the OUTPUT trim rotary
// (-24..+12 dB).
//
// Attachment policy (task 044 / dsp-engine.md §2 non-automatable set):
//   - automatable params (character, fxWindow, outTrim) use the stock APVTS
//     Button/Combo/SliderAttachment classes;
//   - hopMode (TIMING) is automatable but rendered as a two-button radio,
//     which has no stock APVTS attachment class — it uses the underlying
//     juce::ParameterAttachment primitive with complete-gesture writes;
//   - NON-automatable params (model, pluginMode, sampleRateSel, bandwidth)
//     use manual juce::Parameter/SliderParameterAttachment listeners, never
//     an APVTS attachment class.
//
// Tooltips show hardware unit + host-normalized value (ui-design §7);
// accessibility names are set on every control. Model-switch cross-fade and
// clamp-restore are task 046; LCD feedback of these values is 041/045.

#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ModelSelector.h"

#include "mws/model/ModelId.h"

namespace mws::ui {

class ControlPanel final : public juce::Component
{
public:
    /// `apvts` must own the task-028 layout (mws::plugin::createParameterLayout).
    explicit ControlPanel(juce::AudioProcessorValueTreeState& apvts);

    /// Forwarded from the embedded ModelSelector after the `model` parameter
    /// changes (message thread) — the editor restyles faceplate/LnF from it.
    std::function<void(model::ModelId)> onModelChanged;

    void resized() override;

    // --- component access (layout assembly in 045; tests) ------------------
    [[nodiscard]] ModelSelector& modelSelector() noexcept { return selector; }
    [[nodiscard]] juce::TextButton& modeFxButton() noexcept { return modeFx; }
    [[nodiscard]] juce::TextButton& modeSampleButton() noexcept { return modeSample; }
    [[nodiscard]] juce::TextButton& timingClassicButton() noexcept { return timingClassic; }
    [[nodiscard]] juce::TextButton& timingRevisedButton() noexcept { return timingRevised; }
    [[nodiscard]] juce::TextButton& characterButton() noexcept { return characterToggle; }
    [[nodiscard]] juce::ComboBox& windowSelector() noexcept { return windowBox; }
    [[nodiscard]] juce::Slider& bandwidthSlider() noexcept { return bandwidth; }
    [[nodiscard]] juce::TextButton& fs441Button() noexcept { return fs441; }
    [[nodiscard]] juce::TextButton& fs2205Button() noexcept { return fs2205; }
    [[nodiscard]] juce::Slider& outputKnob() noexcept { return output; }

    /// True when the BANDWIDTH row is shown (S900/S950 — dsp-engine §2).
    [[nodiscard]] bool bandwidthRowVisible() const noexcept
    {
        return bandwidth.isVisible();
    }
    /// True when the FS 44.1/22.05 row is shown (S1000/S1100 — dsp-engine §2).
    [[nodiscard]] bool fsRowVisible() const noexcept { return fs441.isVisible(); }

private:
    juce::RangedAudioParameter& param(const char* paramID) const;
    void setRowVisibilityFor(model::ModelId id);
    void refreshTooltips();

    juce::AudioProcessorValueTreeState& state;

    // --- model selector ------------------------------------------------------
    juce::Label selectorLabel;
    ModelSelector selector;

    // --- rows ----------------------------------------------------------------
    juce::Label modeLabel, timingLabel, characterLabel, windowLabel,
        bandwidthLabel, fsLabel, outputLabel;

    juce::TextButton modeFx{ "FX" }, modeSample{ "SAMPLE" };
    juce::TextButton timingClassic{ "CLASSIC" }, timingRevised{ "REVISED" };
    juce::TextButton characterToggle;
    juce::ComboBox windowBox;
    juce::Slider bandwidth;
    juce::TextButton fs441, fs2205;
    juce::Slider output;

    // Manual attachments (non-automatable params + the TIMING radio). The
    // pluginMode/sampleRateSel ones are constructed AFTER the buttons they
    // drive (declaration order matters for the initial-update callbacks).
    std::unique_ptr<juce::ParameterAttachment> modeAttachment;     // pluginMode
    std::unique_ptr<juce::ParameterAttachment> timingAttachment;   // hopMode
    std::unique_ptr<juce::ParameterAttachment> fsAttachment;       // sampleRateSel
    std::unique_ptr<juce::SliderParameterAttachment> bandwidthAttachment;

    // Stock APVTS attachments (automatable params).
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> characterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> windowAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControlPanel)
};

} // namespace mws::ui
