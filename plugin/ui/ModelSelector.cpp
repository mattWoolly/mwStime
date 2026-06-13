// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "ModelSelector.h"

#include <cmath>

namespace mws::ui {

namespace {

/// Group id for the latching radio behavior (any non-zero value).
constexpr int kBadgeRadioGroup = 4401;

constexpr std::size_t kReservedIndex =
    static_cast<std::size_t>(model::ModelId::S3000);

} // namespace

ModelSelector::ModelSelector(juce::RangedAudioParameter& modelParam)
    : attachment(
          modelParam,
          [this](float newValue) {
              latch(static_cast<model::ModelId>(
                  static_cast<int>(std::lround(newValue))));
          },
          nullptr)
{
    for (std::size_t i = 0; i < badges.size(); ++i)
    {
        const auto id = model::kAllModels[i];
        auto& button = badges[i];
        button = std::make_unique<juce::TextButton>();
        button->setRadioGroupId(kBadgeRadioGroup);
        button->setClickingTogglesState(true);

        // Visual latching from the model's OWN spec accent (ui-design §3) —
        // the LookAndFeel draws toggleOn caps with buttonOnColourId.
        button->setColour(juce::TextButton::buttonOnColourId,
                          faceplateSpecFor(id).accent.withAlpha(0.55f));

        if (i == kReservedIndex)
        {
            // ADR-004: reserved blank S3000 badge position — present,
            // BLANK, disabled. No behavior at v1.
            button->setButtonText({});
            button->setEnabled(false);
            button->setTitle("RESERVED");
            button->setTooltip("Reserved badge slot (v1.1)");
        }
        else
        {
            button->setButtonText(juce::String(model::toString(id).data(),
                                               model::toString(id).size()));
            button->setTitle(button->getButtonText());  // accessibility name
            button->onClick = [this, id] {
                // setClickingTogglesState keeps the radio latched; write the
                // non-automatable parameter as a complete gesture. The
                // attachment callback re-latches and fires onModelChanged.
                if (badge(static_cast<std::size_t>(id)).getToggleState())
                    attachment.setValueAsCompleteGesture(
                        static_cast<float>(static_cast<int>(id)));
            };
        }

        addAndMakeVisible(*button);
    }

    setTitle("MODEL SELECTOR");
    attachment.sendInitialUpdate();
}

void ModelSelector::latch(model::ModelId id)
{
    selected = id;
    for (std::size_t i = 0; i < badges.size(); ++i)
        badges[i]->setToggleState(i == static_cast<std::size_t>(id),
                                  juce::dontSendNotification);
    if (onModelChanged != nullptr)
        onModelChanged(id);
}

void ModelSelector::resized()
{
    // §1 mockup: two badge rows — "S900 S950 S1000" / "S1100 (·v1.1·)".
    auto bounds = getLocalBounds();
    const int rowH = bounds.getHeight() / 2;
    const int gap = 4;

    auto top = bounds.removeFromTop(rowH).reduced(0, 2);
    const int topW = (top.getWidth() - 2 * gap) / 3;
    for (std::size_t i = 0; i < 3; ++i)
    {
        badges[i]->setBounds(top.removeFromLeft(topW));
        top.removeFromLeft(gap);
    }

    auto rest = bounds.reduced(0, 2);
    const int bottomW = (rest.getWidth() - gap) / 2;
    badges[3]->setBounds(rest.removeFromLeft(bottomW));
    rest.removeFromLeft(gap);
    badges[4]->setBounds(rest.removeFromLeft(bottomW));
}

} // namespace mws::ui
