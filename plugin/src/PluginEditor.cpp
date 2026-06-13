// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "PluginEditor.h"

#include "../ui/LcdPageBinding.h"

namespace mws::plugin {

namespace {

/// Map the editor's APVTS `model` parameter index → ModelSpec.
const model::ModelSpec& specForSnapshot(const mws::engine::ParamSnapshot& s) noexcept
{
    return model::ModelSpec::get(s.model);
}

} // namespace

PluginEditor::PluginEditor(PluginProcessor& owner)
    : juce::AudioProcessorEditor(owner),
      processor(owner),
      controlPanel(owner.parameterState()),
      fieldEditor(owner.parameterState())
{
    // Sync the faceplate to the model parameter the panel restored/holds.
    faceplate.setModel(controlPanel.modelSelector().selectedModel());
    lookAndFeel.setSpec(faceplate.spec());
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(faceplate);
    addAndMakeVisible(waveform);
    addAndMakeVisible(softKeyBar);
    addAndMakeVisible(jogWheel);

    lcd.setSpec(faceplate.spec());
    addAndMakeVisible(lcd);

    addAndMakeVisible(controlPanel);

    waveform.setSpec(faceplate.spec());

    // --- input-cluster → field-editing wiring (ui-design §6.2 steps 1–2) -----

    // Cursor keys move the LCD field cursor across the active page's field map.
    softKeyBar.onCursor = [this](ui::CursorDir dir) {
        fieldEditor.moveCursor(dir);
        refreshLcd();
    };

    // ENT commits an in-flight text entry (else it is a soft-action no-op at
    // 045 — wired in 045b).
    softKeyBar.onEnter = [this] {
        if (fieldEditor.isEditingText())
            closeFieldTextEditor(/*commit*/ true);
    };

    // Soft keys F1–F8: captions render from the active page; the actions are
    // stubbed no-ops here (task 045b wires TIME/autC/ZONE/GO/PLAY/A-B/SYNC/
    // ABORT). Cursor/ENT field editing above is the only live behavior.
    softKeyBar.onSoftKey = [](int /*index*/) { /* actions: task 045b */ };

    // Jog wheel edits the focused field with the hardware step (fine on Shift).
    jogWheel.onDelta = [this](int steps, bool fine) {
        fieldEditor.applyJog(steps, fine);
        refreshLcd();
    };

    // The field editor surfaces the live value text after any edit so the LCD
    // (and the overlay text editor, while open) re-derive.
    fieldEditor.onChanged = [this] { refreshLcd(); };

    // Instant restyle on model switch (cross-fade + clamp-restore is task 046).
    controlPanel.onModelChanged = [this](mws::model::ModelId id) {
        faceplate.setModel(id);
        lcd.setSpec(faceplate.spec());
        waveform.setSpec(faceplate.spec());
        lookAndFeel.setSpec(faceplate.spec());
        sendLookAndFeelChange();
        refreshLcd();
        repaint();
    };

    // Adopt the active page's field map and draw the first frame.
    {
        const auto snapshot = processor.makeParamSnapshot();
        fieldEditor.setPage(ui::LcdPageModel::build(snapshot, specForSnapshot(snapshot),
                                                    ui::LcdSampleInfo{}, renderInfo_));
    }
    refreshLcd();

    // 1000×380 base canvas (ui-design §1, §5); resizability is task 047.
    setSize(ui::geometry::kBaseWidth, ui::geometry::kBaseHeight);

    // 30 Hz UI poll (architecture.md §4 render-done / progress / LCD updates).
    startTimerHz(kPollHz);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

const model::ModelSpec& PluginEditor::activeSpec() const noexcept
{
    return model::ModelSpec::get(processor.makeParamSnapshot().model);
}

// ---------------------------------------------------------------------------
// 30 Hz UI poll → LcdPageModel → LcdDisplay (architecture.md §4)
// ---------------------------------------------------------------------------

void PluginEditor::timerCallback()
{
    pollEngine();
    refreshLcd();
}

void PluginEditor::pollEngine()
{
    auto& host = processor.engineHost();

    // Drain every queued worker event into the render-info the page reads.
    WorkerEvent ev;
    while (host.popEvent(ev))
        ui::applyWorkerEvent(ev, renderInfo_);

    // FX-mode engine feedback (ADR-006 clamp + SYNC readout) for the page model.
    renderInfo_.fxClampActive = host.fxClampActive();
    const auto sync = host.fxSyncReadout();
    if (sync.active)
    {
        renderInfo_.sourceBpm = sync.sourceBpm;
        renderInfo_.hostBpm = sync.hostBpm;
    }
    else
    {
        renderInfo_.sourceBpm = 0.0;
        renderInfo_.hostBpm = 0.0;
    }

    // Free retired render / FX buffers here on the message thread (never on the
    // audio thread — architecture.md §4 graveyard drain).
    host.collectGarbage();
    host.collectFxGarbage();
}

void PluginEditor::refreshLcd()
{
    const auto snapshot = processor.makeParamSnapshot();
    const auto& spec = specForSnapshot(snapshot);

    // The sample/name slot fills in with the FileLoader flow (task 045b); at
    // 045 the page model renders the "no sample" state. Mono-sum is engine
    // feedback already available for the S900/S950 page notice.
    ui::LcdSampleInfo sample;
    sample.monoSummed = processor.engineHost().fxMonoSummed();

    const ui::LcdPage page =
        ui::LcdPageModel::build(snapshot, spec, sample, renderInfo_);

    // Keep the field cursor on the live page's map (skips greyed fields).
    fieldEditor.setPage(page);

    // FX mode turns the waveform region into the live input scope (ui-design
    // §6.4); the grey-out of GO/PLAY/A-B soft keys is task 045b.
    waveform.setFxScopeMode(snapshot.pluginMode == mws::engine::PluginMode::Fx);

    ui::renderPage(page, lcd, fieldEditor.focusedIndex());
}

// ---------------------------------------------------------------------------
// Direct text entry (double-click a field; ADR-005 — no numeric keypad)
// ---------------------------------------------------------------------------

void PluginEditor::mouseDoubleClick(const juce::MouseEvent& event)
{
    // Map the click into LCD-local coordinates and hit-test an editable field.
    const auto lcdLocal = lcd.getLocalPoint(this, event.position);
    if (! lcd.getLocalBounds().toFloat().contains(lcdLocal))
        return;

    int row = 0, col = 0;
    if (! lcd.cellAt(lcdLocal, row, col))
        return;

    const auto snapshot = processor.makeParamSnapshot();
    const ui::LcdPage page =
        ui::LcdPageModel::build(snapshot, specForSnapshot(snapshot),
                                ui::LcdSampleInfo{}, renderInfo_);

    // Find the editable field whose value cells contain (row, col).
    for (std::size_t i = 0; i < page.fields.size(); ++i)
    {
        const ui::LcdField& f = page.fields[i];
        if (! f.editable)
            continue;
        if (row == f.row && col >= f.col && col < f.col + f.width)
        {
            fieldEditor.focusField(static_cast<int>(i));
            openFieldTextEditor();
            return;
        }
    }
}

void PluginEditor::openFieldTextEditor()
{
    const ui::LcdField* f = fieldEditor.focusedField();
    if (f == nullptr)
        return;

    fieldEditor.beginTextEntry();

    fieldTextEditor = std::make_unique<juce::TextEditor>("lcdFieldEntry");
    fieldTextEditor->setText(fieldEditor.currentFieldText(), juce::dontSendNotification);
    fieldTextEditor->setJustification(juce::Justification::centredLeft);
    fieldTextEditor->setSelectAllWhenFocused(true);
    fieldTextEditor->onReturnKey = [this] { closeFieldTextEditor(/*commit*/ true); };
    fieldTextEditor->onEscapeKey = [this] { closeFieldTextEditor(/*commit*/ false); };
    fieldTextEditor->onFocusLost = [this] { closeFieldTextEditor(/*commit*/ true); };

    // Position the overlay over the field's value cells on the LCD glass.
    const auto first = lcd.cellBounds(f->row, f->col);
    const auto last = lcd.cellBounds(f->row, f->col + f->width - 1);
    auto cells = first.getUnion(last);
    // Translate from LCD-local to editor-local coordinates.
    cells.translate((float) lcd.getX(), (float) lcd.getY());

    addAndMakeVisible(*fieldTextEditor);
    fieldTextEditor->setBounds(cells.toNearestInt());
    fieldTextEditor->grabKeyboardFocus();
}

void PluginEditor::closeFieldTextEditor(bool commit)
{
    if (fieldTextEditor == nullptr)
    {
        fieldEditor.cancelText();
        return;
    }

    const juce::String typed = fieldTextEditor->getText();

    // Tear down the overlay before writing so onFocusLost cannot re-enter.
    auto editor = std::move(fieldTextEditor);
    fieldTextEditor.reset();
    editor.reset();

    if (commit)
        fieldEditor.commitText(typed);
    else
        fieldEditor.cancelText();

    refreshLcd();
}

// ---------------------------------------------------------------------------
// Layout (§1 mockup positions via the shared geometry constants)
// ---------------------------------------------------------------------------

void PluginEditor::resized()
{
    faceplate.setBounds(getLocalBounds());

    // The LCD sits on the bezel's glass: the kLcd region inset by the same
    // 7 px (base-canvas) lip the Faceplate uses for the glass.
    const float scale = (float) getWidth() / (float) ui::geometry::kBaseWidth;
    lcd.setBounds(ui::scaledRegion(ui::geometry::kLcd, getWidth(), getHeight())
                      .reduced(7.0f * scale)
                      .toNearestInt());

    namespace geo = ui::geometry;

    waveform.setBounds(
        ui::scaledRegion(geo::kWaveform, getWidth(), getHeight()).toNearestInt());

    // The SoftKeyBar owns BOTH the soft-key row and the cursor/ENT cluster
    // (ui-design §2), so its bounds are the union of the two frames.
    const auto keysTop = ui::scaledRegion(geo::kSoftKeys, getWidth(), getHeight());
    const auto cursorBottom =
        ui::scaledRegion(geo::kCursorKeys, getWidth(), getHeight());
    softKeyBar.setBounds(keysTop.getUnion(cursorBottom).toNearestInt());

    jogWheel.setBounds(
        ui::scaledRegion(geo::kJogWheel, getWidth(), getHeight()).toNearestInt());

    controlPanel.setBounds(
        ui::scaledRegion(geo::kModelSelector, getWidth(), getHeight())
            .getSmallestIntegerContainer());
}

} // namespace mws::plugin
