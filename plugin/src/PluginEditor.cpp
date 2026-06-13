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

    // ENT commits an in-flight text entry (the LCD field overlay or the F7
    // source-BPM overlay); otherwise it is a soft-action no-op.
    softKeyBar.onEnter = [this] {
        if (syncTextEditor != nullptr)
            closeSyncTextEditor(/*commit*/ true);
        else if (fieldEditor.isEditingText())
            closeFieldTextEditor(/*commit*/ true);
    };

    // Soft keys F1–F8 execute their TIME-page actions (ui-design §6.1–§6.4).
    // The SoftKeyBar gates disabled keys (FX-mode grey state), so handleSoftKey
    // only ever sees live keys.
    softKeyBar.onSoftKey = [this](int index) { handleSoftKey(index); };

    // F8 ABORT is a hold gesture, NOT a tap (ui-design §1 region 3 / §6.3 step
    // 2: "hold F8 to abort", hold >= 600 ms (PI)). The editor owns this wiring
    // explicitly so an accidental F8 tap can never kill a running GO render —
    // requestAbort() fires from the SoftKeyBar's onSoftKey ONLY after the hold
    // completes (the bar's own 30 Hz timer drives updateHoldProgress while held).
    softKeyBar.setKeyRequiresHold(ui::softkey::kAbort, ui::SoftKeyBar::kAbortHoldMs);

    // Jog wheel edits the focused field with the hardware step (fine on Shift).
    jogWheel.onDelta = [this](int steps, bool fine) {
        fieldEditor.applyJog(steps, fine);
        refreshLcd();
    };

    // Stretch-zone / new-name fields are not parameters, so the field editor
    // routes them back through these callbacks (ui-design §6.2 step 1: "jog
    // wheel or direct typing edits it" — for every editable field).

    // Jog: step the focused zone handle by the signed frame delta, relative to
    // the WaveformView's current zone (clamped, start<end by setZone).
    fieldEditor.onZoneJog = [this](ui::LcdFieldKind which, std::int64_t deltaFrames) {
        const auto start = waveform.zoneStart();
        const auto end = waveform.zoneEnd();
        if (which == ui::LcdFieldKind::ZoneStart)
            waveform.setZone(start + deltaFrames, end);
        else
            waveform.setZone(start, end + deltaFrames);
        refreshLcd();
    };

    // Text commit: set the focused zone frame to the absolute typed value.
    fieldEditor.onZoneCommit = [this](ui::LcdFieldKind which, std::int64_t frame) {
        if (which == ui::LcdFieldKind::ZoneStart)
            waveform.setZone(frame, waveform.zoneEnd());
        else
            waveform.setZone(waveform.zoneStart(), frame);
        refreshLcd();
    };

    // currentFieldText() seeds the overlay/LCD with the live zone value rather
    // than a hardcoded 0.
    fieldEditor.zoneValueProvider = [this](ui::LcdFieldKind which) -> std::int64_t {
        return which == ui::LcdFieldKind::ZoneStart ? waveform.zoneStart()
                                                    : waveform.zoneEnd();
    };

    // Destination-name commit: persist the typed name in a member that survives
    // the 30 Hz poll (refreshLcd feeds it back into the LcdSampleInfo).
    fieldEditor.onNameCommit = [this](const juce::String& name) {
        newSampleName_ = name.toStdString();
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

    // --- waveform interactions (ui-design §6.1 drop / §6.3 audition + drag-out)

    // Drop-in: forward the dropped file(s) to the off-thread FileLoader; the
    // 30 Hz poll bridges the decoded sample into the engine + view + export
    // name + filename-BPM auto-guess (pollFileLoader).
    waveform.onFilesDropped = [this](const juce::StringArray& files) {
        if (! files.isEmpty())
            pendingLoadId_ = fileLoader.load(juce::File(files[0]));
    };

    // Click-to-audition (architecture.md §7: audition is always available from
    // the UI / waveform click). A body click auditions the loaded ORIGINAL (A)
    // from the head; PLAY (F5) auditions the render (B).
    waveform.onAudition = [this](std::int64_t /*frame*/) {
        auto& host = processor.engineHost();
        host.setAuditionSource(mws::plugin::AuditionSource::Original);
        host.startSamplePlayback();
        waveform.setRenderProminent(false);
    };

    // Drag-out: initiate the external WAV drag of the published render through
    // the ExportService (ui-design §6.3 step 4 / §1 floppy slot).
    waveform.onDragExport = [this] { exportService.startDrag(waveform); };

    // --- file loader / export service wiring ---------------------------------
    exportService.attach(&processor.engineHost());
    fileLoader.startThread();

    // Keyboard-only operability (ui-design §7): the editor is a focus
    // container; arrows/Enter mirror cursor/ENT through the SoftKeyBar, and
    // Tab traverses every control. Screen-reader names = LCD field labels are
    // set per-poll in refreshLcd (the field map drives them).
    setWantsKeyboardFocus(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle("mwStime editor");

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
    fileLoader.stop();  // joins the decode thread deterministically
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
    pollFileLoader();
    refreshLcd();
}

void PluginEditor::pollFileLoader()
{
    auto& host = processor.engineHost();

    // Drain load outcomes (ui-design §6.1 step 3: errors surface on the LCD).
    LoadEvent ev;
    while (fileLoader.popEvent(ev))
    {
        if (ev.kind != LoadEvent::Kind::Finished)
            continue;

        // Only the load we are awaiting; an older decode posts Superseded.
        if (pendingLoadId_ != 0 && ev.requestId != pendingLoadId_
            && ev.outcome != LoadOutcome::Loaded)
            continue;

        if (ev.outcome == LoadOutcome::Loaded)
        {
            renderInfo_.loadError = ui::LcdLoadError::None;
            if (auto source = fileLoader.currentSource())
            {
                // Bridge the decoded source into the engine (audition A + the
                // worker's render source) and the waveform view (peaks + zone).
                auto buffer =
                    std::make_shared<const mws::core::AudioBuffer>(source->audio);
                host.setAuditionSource(buffer);
                waveform.setSourceSample(buffer);

                loadedSampleName_ = source->name.toStdString();
                loadedSampleFrames_ = source->numFrames;
                newSampleName_.clear();  // new render name falls back to "*ST"

                // Export name stem + filename `_174bpm` BPM auto-guess (never
                // clobbers a user value — ui-design §6.2 step 4).
                exportService.setSampleName(source->name);
                host.guessSourceBpmFromFilename(source->name.toStdString());
            }
        }
        else if (ev.outcome == LoadOutcome::UnsupportedFormat)
        {
            renderInfo_.loadError = ui::LcdLoadError::UnsupportedFormat;
        }
        else if (ev.outcome == LoadOutcome::ReadFailure)
        {
            renderInfo_.loadError = ui::LcdLoadError::ReadFailure;
        }
    }

    fileLoader.collectGarbage();
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

    // The loaded sample slot (FileLoader flow, ui-design §6.1 step 2). Mono-sum
    // is engine feedback for the S900/S950 page notice.
    ui::LcdSampleInfo sample;
    sample.monoSummed = processor.engineHost().fxMonoSummed();
    sample.name = loadedSampleName_;
    sample.lengthFrames = loadedSampleFrames_;

    // Live stretch zone from the WaveformView (the field editor's zone callbacks
    // write it there), and the persisted render-destination name, so the LCD
    // shows the edited values instead of a constant 0 / default.
    sample.zoneStart = waveform.zoneStart();
    sample.zoneEnd = waveform.zoneEnd();
    sample.newName = newSampleName_;

    const ui::LcdPage page =
        ui::LcdPageModel::build(snapshot, spec, sample, renderInfo_);

    // Keep the field cursor on the live page's map (skips greyed fields).
    fieldEditor.setPage(page);

    // FX mode turns the waveform region into the live input scope (ui-design
    // §6.4). Attach the processor's scope FIFO once it exists (after prepareFx).
    const bool fx = snapshot.pluginMode == mws::engine::PluginMode::Fx;
    waveform.setFxScopeMode(fx);
    if (! scopeFifoAttached_)
        if (auto* fifo = processor.engineHost().scopeFifo())
        {
            waveform.attachScopeFifo(fifo);
            scopeFifoAttached_ = true;
        }

    // Soft-key captions + per-mode/per-model enable matrix (ui-design §6.4 /
    // §6.2.3): FX greys GO/PLAY/A-B, the S950 reads AUTO-D, the S900 greys the
    // stretch-only autC/ZONE.
    refreshSoftKeys(snapshot);

    // Accessibility: the LCD's screen-reader name is the focused field's label
    // (ui-design §7: "screen-reader names = LCD field labels"). The field map
    // drives it, so it tracks the cursor across pages/models every poll.
    if (const ui::LcdField* focused = fieldEditor.focusedField())
        lcd.setTitle("LCD: " + juce::String(ui::fieldLabel(*focused)));
    else
        lcd.setTitle("LCD");

    ui::renderPage(page, lcd, fieldEditor.focusedIndex());
}

// ---------------------------------------------------------------------------
// Soft-key captions + enable matrix (ui-design §6.1–§6.4; ui::EditorActions)
// ---------------------------------------------------------------------------

void PluginEditor::refreshSoftKeys(const mws::engine::ParamSnapshot& snapshot)
{
    const auto& spec = specForSnapshot(snapshot);
    const auto labels = ui::softKeyLabels(snapshot, spec);
    for (int i = 0; i < ui::softkey::kCount; ++i)
    {
        softKeyBar.setKeyLabel(i, juce::String(labels[(std::size_t) i]));
        softKeyBar.setKeyEnabled(i, ui::softKeyEnabled(i, snapshot, spec));
    }
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

    // Double-clicking the LCD away from an editable field opens the typed
    // source-BPM entry (the "typed" half of the SYNC flow, ui-design §6.2.4 —
    // the F7 soft key is the tap half).
    openSyncTextEditor();
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
// Soft-key actions (ui-design §6.1–§6.4)
// ---------------------------------------------------------------------------

EngineHost::Zone PluginEditor::currentZone() const noexcept
{
    const auto total = waveform.sourceFrames();
    if (total <= 0)
        return {};  // {0, 1}: full source (no selection yet)

    EngineHost::Zone z;
    z.start = juce::jlimit(0.0, 1.0,
                           (double) waveform.zoneStart() / (double) total);
    z.end = juce::jlimit(0.0, 1.0, (double) waveform.zoneEnd() / (double) total);
    return z;
}

void PluginEditor::handleSoftKey(int index)
{
    switch (index)
    {
        case ui::softkey::kTime:  grabKeyboardFocus(); break;  // F1: page focus
        case ui::softkey::kAutC:  doAutoCycle(); break;        // F2: auto cycle
        case ui::softkey::kZone:                               // F3: ZONE preview
        {
            const auto snapshot = processor.makeParamSnapshot();
            const bool on = ! processor.engineHost().zonePreviewActive();
            processor.engineHost().setZonePreview(on, currentZone(), snapshot);
            break;
        }
        case ui::softkey::kGo:    doGoRender(); break;         // F4: GO render
        case ui::softkey::kPlay:  doPlay(); break;             // F5: PLAY
        case ui::softkey::kAb:    doAbToggle(); break;         // F6: A/B
        case ui::softkey::kSync:  doSyncEntry(); break;        // F7: SYNC
        case ui::softkey::kAbort:                              // F8: ABORT (hold)
            processor.engineHost().requestAbort();
            break;
        default: break;
    }
    refreshLcd();
}

void PluginEditor::doAutoCycle()
{
    // F2 autC / AUTO-D: run the task-014 detector over the selected zone (on
    // this message thread — AutoCycle allocates, not RT-safe) and land the
    // result in the cycleLen parameter (ui-design §6.2 step 3). The autoCycle
    // trigger parameter is the §2 momentary signal: set it, consume, reset.
    auto* trigger = processor.parameterState().getParameter(paramid::autoCycle);
    if (trigger != nullptr)
        trigger->setValueNotifyingHost(1.0f);

    // Detect over the loaded source's selected zone (nullptr → documented
    // fallback). The loader holds the published source; we read it directly.
    std::shared_ptr<const mws::core::AudioBuffer> source;
    if (auto loaded = fileLoader.currentSource())
        source = std::make_shared<const mws::core::AudioBuffer>(loaded->audio);

    const int detected =
        ui::autoCycleForZone(source, waveform.zoneStart(), waveform.zoneEnd());

    if (auto* cycleParam = processor.parameterState().getParameter(paramid::cycleLen))
        cycleParam->setValueNotifyingHost(
            cycleParam->convertTo0to1(static_cast<float>(detected)));

    // Consume + self-reset the momentary trigger.
    if (trigger != nullptr)
        trigger->setValueNotifyingHost(0.0f);
}

void PluginEditor::doGoRender()
{
    // F4 GO: enqueue an offline render of the selected zone (ui-design §6.3
    // step 2). Progress / refusal / done arrive on the worker FIFO (pollEngine).
    const auto snapshot = processor.makeParamSnapshot();
    exportService.setRenderParams(snapshot);
    processor.engineHost().requestRender(snapshot, currentZone());
}

void PluginEditor::doPlay()
{
    // F5 PLAY: audition the render (B) from its head (ui-design §6.3 step 3).
    auto& host = processor.engineHost();
    host.setAuditionSource(mws::plugin::AuditionSource::Render);
    host.startSamplePlayback();
    waveform.setRenderProminent(true);
}

void PluginEditor::doAbToggle()
{
    // F6 A/B: flip the audition source and the waveform's prominent layer
    // (ui-design §6.3 step 3).
    auto& host = processor.engineHost();
    const bool toRender = host.auditionSource() == mws::plugin::AuditionSource::Original;
    host.setAuditionSource(toRender ? mws::plugin::AuditionSource::Render
                                    : mws::plugin::AuditionSource::Original);
    waveform.setRenderProminent(toRender);
}

void PluginEditor::doSyncEntry()
{
    // F7 SYNC: source-BPM entry (ui-design §6.2 step 4 — "typed or tap"). Each
    // F7 press is a tap; the running tap average sets the source BPM. A
    // double-click on the sync LCD line opens the typed overlay (openSync...).
    const double bpm = tapTempo.tap((double) juce::Time::getMillisecondCounter());
    if (bpm > 0.0)
        processor.engineHost().setSourceBpm(bpm, /*userSet=*/true);
}

void PluginEditor::openSyncTextEditor()
{
    // Typed source-BPM entry overlay (the "typed" half of ui-design §6.2.4).
    syncTextEditor = std::make_unique<juce::TextEditor>("syncBpmEntry");
    syncTextEditor->setText(juce::String(processor.engineHost().sourceBpm(), 1),
                            juce::dontSendNotification);
    syncTextEditor->setJustification(juce::Justification::centredLeft);
    syncTextEditor->setSelectAllWhenFocused(true);
    syncTextEditor->setTitle("Source BPM");
    syncTextEditor->onReturnKey = [this] { closeSyncTextEditor(/*commit*/ true); };
    syncTextEditor->onEscapeKey = [this] { closeSyncTextEditor(/*commit*/ false); };
    syncTextEditor->onFocusLost = [this] { closeSyncTextEditor(/*commit*/ true); };

    // Anchor the overlay over the LCD glass (the SYNC readout line region).
    addAndMakeVisible(*syncTextEditor);
    auto cells = lcd.getBounds().reduced(lcd.getWidth() / 4, lcd.getHeight() / 3);
    syncTextEditor->setBounds(cells);
    syncTextEditor->grabKeyboardFocus();
}

void PluginEditor::closeSyncTextEditor(bool commit)
{
    if (syncTextEditor == nullptr)
        return;

    const juce::String typed = syncTextEditor->getText().trim();

    auto editor = std::move(syncTextEditor);
    syncTextEditor.reset();
    editor.reset();

    if (commit)
    {
        const double bpm = typed.getDoubleValue();
        if (bpm > 0.0)
            processor.engineHost().setSourceBpm(bpm, /*userSet=*/true);
    }
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
