// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "../ui/ControlPanel.h"
#include "../ui/Faceplate.h"
#include "../ui/JogWheel.h"
#include "../ui/LcdDisplay.h"
#include "../ui/LcdFieldEditor.h"
#include "../ui/LcdPageModel.h"
#include "../ui/SoftKeyBar.h"
#include "../ui/WaveformView.h"
#include "../ui/lookandfeel/SeriesLookAndFeel.h"
#include "PluginProcessor.h"

namespace mws::plugin {

/// 1000×380 editor (task 045): the assembled S-series faceplate (039), the LCD
/// hero (040) driven entirely by the LcdPageModel (041), the input cluster
/// (042), the waveform/scope region (043), and the right-hand control panel
/// (044), at the §1 mockup positions (geometry constants, 039).
///
/// A 30 Hz (PI) timer polls the engine UI FIFO (architecture.md §4): render
/// progress / done / refusal, the FX FREE clamp and SYNC readout fold into an
/// LcdRenderInfo → LcdPageModel::build → the LcdDisplay cells (LcdPageBinding).
/// The LCD field-editing system (ui-design §6.2 steps 1–2) runs through
/// LcdFieldEditor: cursor keys move the field cursor, the jog wheel edits the
/// focused field with hardware steps (fine on Shift), and double-clicking a
/// field opens direct text entry (no numeric keypad — ADR-005); ENT commits.
///
/// Soft-key *actions*, drop-in/drag-out, click-to-audition, FX-mode greying
/// and keyboard-only accessibility are task 045b; this assembly stubs the
/// soft-key actions as no-ops except cursor/ENT field editing.
class PluginEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PluginEditor(PluginProcessor& owner);
    ~PluginEditor() override;

    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

private:
    /// UI poll rate (architecture.md §4 FIFO → timer poll). 30 Hz (PI).
    static constexpr int kPollHz = 30;

    void timerCallback() override;

    /// Drain the engine UI FIFO into renderInfo_ and collect the publication
    /// graveyard (message thread, per the timer poll).
    void pollEngine();

    /// Rebuild the active page from the current parameter snapshot + engine
    /// feedback and push it to the LCD with the field cursor parked.
    void refreshLcd();

    /// The model spec the LCD/field editor use for the current model parameter.
    [[nodiscard]] const model::ModelSpec& activeSpec() const noexcept;

    /// Begin direct text entry on the focused field: place the overlay editor
    /// over the field's cells seeded with its current value (ui-design §6.2).
    void openFieldTextEditor();
    void closeFieldTextEditor(bool commit);

    PluginProcessor& processor;

    ui::SeriesLookAndFeel lookAndFeel;
    ui::Faceplate faceplate;    // defaults to S1000 (the canonical look)
    ui::LcdDisplay lcd;         // dynamic layer on the faceplate's LCD glass
    ui::SoftKeyBar softKeyBar;  // F1–F8 + cursor/ENT cluster (task 042)
    ui::JogWheel jogWheel;      // endless data wheel (task 042)
    ui::WaveformView waveform;  // waveform / FX scope region (task 043)
    ui::ControlPanel controlPanel;         // model selector + right-panel controls (task 044)
    juce::TooltipWindow tooltips{ this };  // ui-design §7 tooltips

    ui::LcdFieldEditor fieldEditor;  // cursor/jog/text editing (ui-design §6.2)

    // Direct-text-entry overlay (double-click a field; ADR-005 no keypad). Lazy
    // — present only while editing; positioned over the focused field's cells.
    std::unique_ptr<juce::TextEditor> fieldTextEditor;

    // Engine feedback accumulated across timer polls (architecture.md §4).
    ui::LcdRenderInfo renderInfo_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

} // namespace mws::plugin
