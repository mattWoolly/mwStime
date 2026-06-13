// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "ControlPanel.h"

#include <cmath>

#include "FaceplateSpec.h"

#include "state/Parameters.h"

namespace mws::ui {

namespace pid = mws::plugin::paramid;

namespace {

// Radio-group ids (any non-zero, distinct per row).
constexpr int kModeGroup = 4402;
constexpr int kTimingGroup = 4403;
constexpr int kFsGroup = 4404;

/// ui-design §7: tooltips show BOTH the hardware-unit value (what the LCD
/// shows) and the host-normalized 0–1 value.
juce::String hardwareTooltip(const juce::RangedAudioParameter& p)
{
    return p.getName(32) + ": " + p.getCurrentValueAsText() + "  (host "
           + juce::String(p.getValue(), 3) + ")";
}

void styleRowLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setInterceptsMouseClicks(false, false);
    label.setMinimumHorizontalScale(0.7f);
}

void setupRadioPair(juce::TextButton& a, juce::TextButton& b, int groupId)
{
    for (auto* button : { &a, &b })
    {
        button->setRadioGroupId(groupId);
        button->setClickingTogglesState(true);
        button->setTitle(button->getButtonText());  // accessibility name
    }
}

/// Latches a two-button radio row from a choice-parameter value (0 or 1).
void latchPair(juce::TextButton& zero, juce::TextButton& one, float value)
{
    const bool second = std::lround(value) == 1;
    zero.setToggleState(!second, juce::dontSendNotification);
    one.setToggleState(second, juce::dontSendNotification);
}

} // namespace

ControlPanel::ControlPanel(juce::AudioProcessorValueTreeState& apvts)
    : state(apvts), selector(param(pid::model))
{
    // --- MODEL SELECTOR badges (ModelSelector owns the latching) -----------
    styleRowLabel(selectorLabel, "MODEL SELECTOR");
    selectorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(selectorLabel);
    addAndMakeVisible(selector);
    selector.onModelChanged = [this](model::ModelId id) {
        setRowVisibilityFor(id);
        if (onModelChanged != nullptr)
            onModelChanged(id);
    };

    // --- MODE FX/SAMPLE — pluginMode, NON-automatable ⇒ manual attachment --
    styleRowLabel(modeLabel, "MODE");
    addAndMakeVisible(modeLabel);
    setupRadioPair(modeFx, modeSample, kModeGroup);
    addAndMakeVisible(modeFx);
    addAndMakeVisible(modeSample);
    {
        auto& p = param(pid::pluginMode);
        modeAttachment = std::make_unique<juce::ParameterAttachment>(
            p,
            [this](float v) {
                latchPair(modeFx, modeSample, v);
                // WINDOW applies to FX mode only (dsp-engine §2 fxWindow row;
                // ui-design §1 "WINDOW [1 BAR] (FX)").
                const bool fx = std::lround(v) == 0;
                windowBox.setEnabled(fx);
                windowLabel.setEnabled(fx);
                refreshTooltips();
            },
            nullptr);
        modeFx.onClick = [this] {
            if (modeFx.getToggleState())
                modeAttachment->setValueAsCompleteGesture(0.0f);
        };
        modeSample.onClick = [this] {
            if (modeSample.getToggleState())
                modeAttachment->setValueAsCompleteGesture(1.0f);
        };
    }

    // --- TIMING CLASSIC/REVISED — hopMode, automatable; two-button radios
    // have no stock APVTS attachment class, so the juce::ParameterAttachment
    // primitive provides the gesture-correct bridge.
    styleRowLabel(timingLabel, "TIMING");
    addAndMakeVisible(timingLabel);
    setupRadioPair(timingClassic, timingRevised, kTimingGroup);
    addAndMakeVisible(timingClassic);
    addAndMakeVisible(timingRevised);
    {
        auto& p = param(pid::hopMode);
        timingAttachment = std::make_unique<juce::ParameterAttachment>(
            p,
            [this](float v) {
                latchPair(timingClassic, timingRevised, v);
                refreshTooltips();
            },
            nullptr);
        timingClassic.onClick = [this] {
            if (timingClassic.getToggleState())
                timingAttachment->setValueAsCompleteGesture(0.0f);
        };
        timingRevised.onClick = [this] {
            if (timingRevised.getToggleState())
                timingAttachment->setValueAsCompleteGesture(1.0f);
        };
    }

    // --- CHARACTER on/off — automatable bool ⇒ stock ButtonAttachment ------
    styleRowLabel(characterLabel, "CHARACTER");
    addAndMakeVisible(characterLabel);
    characterToggle.setClickingTogglesState(true);
    characterToggle.setTitle("CHARACTER");
    addAndMakeVisible(characterToggle);
    characterAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            state, pid::character, characterToggle);
    // The attachment latches with sendNotificationSync, so one click handler
    // keeps the ON/OFF cap text mirrored for both directions of change.
    characterToggle.onClick = [this] {
        characterToggle.setButtonText(characterToggle.getToggleState() ? "ON" : "OFF");
        refreshTooltips();
    };
    characterToggle.setButtonText(characterToggle.getToggleState() ? "ON" : "OFF");

    // --- WINDOW — fxWindow, automatable choice ⇒ stock ComboBoxAttachment --
    styleRowLabel(windowLabel, "WINDOW");
    addAndMakeVisible(windowLabel);
    {
        auto& p = param(pid::fxWindow);
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&p))
            windowBox.addItemList(choice->choices, 1);
        windowBox.setTitle("WINDOW");
        addAndMakeVisible(windowBox);
        windowAttachment =
            std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
                state, pid::fxWindow, windowBox);
        windowBox.onChange = [this] { refreshTooltips(); };
    }

    // --- BANDWIDTH — S900/S950 row; NON-automatable (latency-changing,
    // dsp-engine §2) ⇒ manual SliderParameterAttachment, never APVTS.
    styleRowLabel(bandwidthLabel, "BANDWIDTH");
    addAndMakeVisible(bandwidthLabel);
    bandwidth.setSliderStyle(juce::Slider::LinearHorizontal);
    bandwidth.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    bandwidth.setTitle("BANDWIDTH");
    addAndMakeVisible(bandwidth);
    bandwidthAttachment = std::make_unique<juce::SliderParameterAttachment>(
        param(pid::bandwidth), bandwidth, nullptr);
    bandwidth.onValueChange = [this] { refreshTooltips(); };

    // --- FS 44.1/22.05 — S1000/S1100 row; NON-automatable ⇒ manual ---------
    styleRowLabel(fsLabel, "FS");
    addAndMakeVisible(fsLabel);
    {
        auto& p = param(pid::sampleRateSel);
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&p))
        {
            fs441.setButtonText(choice->choices[0]);
            fs2205.setButtonText(choice->choices[1]);
        }
        setupRadioPair(fs441, fs2205, kFsGroup);
        addAndMakeVisible(fs441);
        addAndMakeVisible(fs2205);
        fsAttachment = std::make_unique<juce::ParameterAttachment>(
            p,
            [this](float v) {
                latchPair(fs441, fs2205, v);
                refreshTooltips();
            },
            nullptr);
        fs441.onClick = [this] {
            if (fs441.getToggleState())
                fsAttachment->setValueAsCompleteGesture(0.0f);
        };
        fs2205.onClick = [this] {
            if (fs2205.getToggleState())
                fsAttachment->setValueAsCompleteGesture(1.0f);
        };
    }

    // --- OUTPUT trim rotary — automatable float ⇒ stock SliderAttachment ---
    styleRowLabel(outputLabel, "OUTPUT");
    addAndMakeVisible(outputLabel);
    output.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    output.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    output.setTitle("OUTPUT");
    addAndMakeVisible(output);
    outputAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, pid::outTrim, output);
    output.onValueChange = [this] { refreshTooltips(); };

    // Initial state: latch radios / enable WINDOW / show the right context
    // row for the current parameter values.
    modeAttachment->sendInitialUpdate();
    timingAttachment->sendInitialUpdate();
    fsAttachment->sendInitialUpdate();
    setRowVisibilityFor(selector.selectedModel());
    refreshTooltips();
}

juce::RangedAudioParameter& ControlPanel::param(const char* paramID) const
{
    auto* p = state.getParameter(paramID);
    jassert(p != nullptr);  // layout/id mismatch is a programming error
    return *p;
}

void ControlPanel::setRowVisibilityFor(model::ModelId id)
{
    // Context-dependent rows follow the dsp-engine §2 applies-to column via
    // the FaceplateSpec visibility table (single source of truth, task 039):
    // BANDWIDTH on the varclock models (S900/S950), FS on S1000/S1100.
    const auto& vis = faceplateSpecFor(id).visibility;

    bandwidthLabel.setVisible(vis.bandwidth);
    bandwidth.setVisible(vis.bandwidth);

    fsLabel.setVisible(vis.sampleRateSel);
    fs441.setVisible(vis.sampleRateSel);
    fs2205.setVisible(vis.sampleRateSel);

    resized();
}

void ControlPanel::refreshTooltips()
{
    modeFx.setTooltip(hardwareTooltip(param(pid::pluginMode)));
    modeSample.setTooltip(modeFx.getTooltip());
    timingClassic.setTooltip(hardwareTooltip(param(pid::hopMode)));
    timingRevised.setTooltip(timingClassic.getTooltip());
    characterToggle.setTooltip(hardwareTooltip(param(pid::character)));
    windowBox.setTooltip(hardwareTooltip(param(pid::fxWindow)));
    bandwidth.setTooltip(hardwareTooltip(param(pid::bandwidth)));
    fs441.setTooltip(hardwareTooltip(param(pid::sampleRateSel)));
    fs2205.setTooltip(fs441.getTooltip());
    output.setTooltip(hardwareTooltip(param(pid::outTrim)));
}

void ControlPanel::resized()
{
    // Proportional row layout inside the §1 region-4 frame (290×230 at the
    // 1000×380 base canvas; everything scales with the component).
    auto bounds = getLocalBounds().reduced(juce::roundToInt((float) getWidth() * 0.03f),
                                           juce::roundToInt((float) getHeight() * 0.02f));
    const int rowGap = juce::jmax(1, bounds.getHeight() / 110);
    const int labelW = bounds.getWidth() * 32 / 100;
    const float h = (float) bounds.getHeight();

    auto takeRow = [&](float proportion) {
        auto row = bounds.removeFromTop(juce::roundToInt(h * proportion));
        bounds.removeFromTop(rowGap);
        return row;
    };

    selectorLabel.setBounds(takeRow(0.07f));
    selector.setBounds(takeRow(0.24f));

    auto layoutPair = [&](juce::Label& label, juce::Component& a, juce::Component& b,
                          float proportion) {
        auto row = takeRow(proportion);
        label.setBounds(row.removeFromLeft(labelW));
        const int half = (row.getWidth() - rowGap) / 2;
        a.setBounds(row.removeFromLeft(half));
        row.removeFromLeft(rowGap);
        b.setBounds(row);
    };

    layoutPair(modeLabel, modeFx, modeSample, 0.10f);
    layoutPair(timingLabel, timingClassic, timingRevised, 0.10f);

    {
        auto row = takeRow(0.10f);
        characterLabel.setBounds(row.removeFromLeft(labelW));
        characterToggle.setBounds(row.removeFromLeft(row.getWidth() / 2));
    }
    {
        auto row = takeRow(0.10f);
        windowLabel.setBounds(row.removeFromLeft(labelW));
        windowBox.setBounds(row);
    }

    // Context row: BANDWIDTH and FS share the same slot (only one is ever
    // visible — the model is either varclock or fixed-rate).
    {
        auto row = takeRow(0.10f);
        auto bwRow = row;
        bandwidthLabel.setBounds(bwRow.removeFromLeft(labelW));
        bandwidth.setBounds(bwRow);

        fsLabel.setBounds(row.removeFromLeft(labelW));
        const int half = (row.getWidth() - rowGap) / 2;
        fs441.setBounds(row.removeFromLeft(half));
        row.removeFromLeft(rowGap);
        fs2205.setBounds(row);
    }

    // OUTPUT knob takes the remaining height.
    auto row = bounds;
    outputLabel.setBounds(row.removeFromLeft(labelW));
    output.setBounds(row.withSizeKeepingCentre(
        juce::jmin(row.getWidth(), row.getHeight()),
        juce::jmin(row.getWidth(), row.getHeight())));
}

} // namespace mws::ui
