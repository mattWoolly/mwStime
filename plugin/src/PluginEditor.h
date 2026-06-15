// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "../ui/ControlPanel.h"
#include "../ui/EditorActions.h"
#include "../ui/EditorResize.h"
#include "../ui/Faceplate.h"
#include "../ui/JogWheel.h"
#include "../ui/LcdDisplay.h"
#include "../ui/LcdFieldEditor.h"
#include "../ui/LcdPageModel.h"
#include "../ui/SoftKeyBar.h"
#include "../ui/WaveformView.h"
#include "../ui/lookandfeel/SeriesLookAndFeel.h"
#include "ExportService.h"
#include "FileLoader.h"
#include "PluginProcessor.h"
#include "SamplePlayer.h"  // AuditionSource

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
/// Task 045b completes the interaction flows (ui-design §6.1–§6.4, §7): the
/// eight TIME-page soft keys execute their actions through the 042 callback
/// interface (TIME/autC/ZONE/GO/PLAY/A-B/SYNC/ABORT), drop-in routes to the
/// FileLoader and drag-out to the ExportService, the waveform click auditions
/// through the SamplePlayer (architecture.md §7), FX mode greys/re-pages the
/// right things, and the editor is keyboard-only operable with JUCE
/// accessibility handlers (screen-reader names = LCD field labels). The
/// per-mode/per-model decision logic lives headless in ui::EditorActions.
class PluginEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PluginEditor(PluginProcessor& owner);
    ~PluginEditor() override;

    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    /// Persist the current scale (width / base width, clamped) into the
    /// processor's non-parameter state tree (uiState/scaleFactor) so a host
    /// session save/reload restores the editor size (architecture.md §6;
    /// ui-design §5). Called on resize-end (corner drag) and on a menu scale
    /// pick.
    void persistScale();

    /// Resize the editor to a fixed scale multiple of the base canvas (the
    /// 75/100/150/200% hamburger entries) and persist it.
    void applyScale(double scale);

    /// Show the §1 region-1 hamburger menu (about / manual / scale).
    void showHamburgerMenu();

    /// Constrainer that persists the scale when the user finishes a corner drag
    /// (JUCE calls resizeEnd at the end of an interactive resize). Programmatic
    /// resizes (menu picks, restore-on-open) persist explicitly via applyScale.
    struct ScaleConstrainer final : juce::ComponentBoundsConstrainer
    {
        explicit ScaleConstrainer(PluginEditor& ownerEditor) : owner(ownerEditor) {}
        void resizeEnd() override { owner.persistScale(); }
        PluginEditor& owner;
    };

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

    // --- soft-key actions (ui-design §6.1–§6.4) ------------------------------
    /// Dispatch a soft-key press to its action (TIME/autC/ZONE/GO/PLAY/A-B/
    /// SYNC/ABORT). Disabled keys never reach here (SoftKeyBar gates them).
    void handleSoftKey(int index);
    void doAutoCycle();             ///< F2: run the detector → write cycleLen
    void doGoRender();              ///< F4: enqueue an offline render of the zone
    void doPlay();                  ///< F5: audition the render (B)
    void doAbToggle();              ///< F6: flip the A/B audition source
    void doSyncEntry();             ///< F7: source-BPM entry (typed / tap)
    /// Push the current page's captions + per-mode enable matrix onto the bar
    /// (FX greys GO/PLAY/A-B; S950 reads AUTO-D — ui-design §6.4 / §6.2.3).
    void refreshSoftKeys(const mws::engine::ParamSnapshot& snapshot);

    /// Drain the FileLoader UI FIFO: a finished load bridges the decoded source
    /// into the engine + waveform + export name + filename BPM auto-guess.
    void pollFileLoader();

    /// The normalized [0,1] render zone from the WaveformView selection.
    [[nodiscard]] EngineHost::Zone currentZone() const noexcept;

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

    // UI-flow services owned by the editor (message thread): off-thread decode
    // (drop-in, ui-design §6.1) and the drag-out / save-as WAV export
    // (ui-design §6.3 step 4). Both bridge to the processor's EngineHost.
    FileLoader fileLoader;
    ExportService exportService;

    // F7 SYNC tap-tempo state (ui-design §6.2 step 4: "typed or tap"). Taps
    // average into a source BPM through ui::TapTempo; the source-BPM text
    // entry overlay is the typed path.
    ui::TapTempo tapTempo;
    /// The BPM the most recent F7 tap implied (0 until >= 2 taps): drives the
    /// per-tap SYNC LCD feedback hint (task 057, softKeyPressHint).
    double lastTapBpm_ = 0.0;
    std::unique_ptr<juce::TextEditor> syncTextEditor;
    void openSyncTextEditor();
    void closeSyncTextEditor(bool commit);

    // Direct-text-entry overlay (double-click a field; ADR-005 no keypad). Lazy
    // — present only while editing; positioned over the focused field's cells.
    std::unique_ptr<juce::TextEditor> fieldTextEditor;

    // Engine feedback accumulated across timer polls (architecture.md §4).
    ui::LcdRenderInfo renderInfo_;

    // The typed render-destination name (new-sample field). refreshLcd builds a
    // fresh LcdSampleInfo every poll, so the committed name must survive here
    // and be fed back into the sample info (else the edit is silently dropped).
    // Empty -> the page model falls back to the "<name>*ST" default.
    std::string newSampleName_;

    // The loaded source's display name + length, captured when the FileLoader
    // finishes a decode (ui-design §6.1 step 2: LCD top line shows the name).
    std::string loadedSampleName_;
    std::int64_t loadedSampleFrames_ = 0;

    // The id of the most recent load() we are awaiting, so the poll only
    // bridges its OWN finished decode (latest-wins; older loads post Superseded).
    std::uint64_t pendingLoadId_ = 0;

    // The attached FX input-scope FIFO (the processor owns it; attached once
    // it exists so the waveform's FX scope draws — ui-design §6.4).
    bool scopeFifoAttached_ = false;

    // --- model-switch behavior (ui-design §6.5, task 046) --------------------
    /// Apply a model switch: clamp-memory restore/remember on the timeFactor
    /// parameter (writing the new model's clamped value to the host param),
    /// faceplate palette cross-fade, LCD layout swap + page rebuild. Mirrors the
    /// clamp map to/from the task-029 state-tree `clampMemory` field so it
    /// survives save/reload.
    void handleModelSwitch(mws::model::ModelId newModel);

    /// Per-model pre-clamp timeFactor memory (ui-design §6.5 (PI)). Seeded from
    /// the state tree on construction; updated on every switch and mirrored back.
    ui::ClampMemory clampMemory_;
    /// The model the editor last styled, so a switch knows what it is leaving.
    mws::model::ModelId currentModel_ = mws::model::ModelId::S1000;

    // Fixed-aspect 0.6×–2.0× resize (task 047, ui-design §5). The constrainer
    // locks the 1000/380 aspect + 600×228 / 2000×760 limits and persists the
    // scale on corner-drag end; the corner grip is the bottom-right handle.
    ScaleConstrainer constrainer{ *this };
    std::unique_ptr<juce::ResizableCornerComponent> resizerCorner;

    // Guards persistScale() from firing during the restore-on-open setSize and
    // during a programmatic applyScale (we persist those explicitly), so an
    // intermediate JUCE resized() can never overwrite the value we are setting.
    bool suppressScalePersist_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

} // namespace mws::plugin
