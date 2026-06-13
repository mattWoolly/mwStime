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
    currentModel_ = controlPanel.modelSelector().selectedModel();
    faceplate.setModel(currentModel_);
    lookAndFeel.setSpec(faceplate.spec());
    setLookAndFeel(&lookAndFeel);

    // Seed the per-model clamp memory from the persisted state-tree field
    // (task 029 schema, ui-design §6.5 (PI) — survives save/reload). A model
    // with no stored value stays unset (the live host value carries across).
    {
        const auto& tree = processor.nonParameterState();
        for (const auto modelId : mws::model::kAllModels)
        {
            constexpr double kNoMemory = -1.0;  // no valid timeFactor is negative
            const double stored = mws::plugin::state::getClampMemory(tree, modelId, kNoMemory);
            if (stored > 0.0)
                clampMemory_.remember(modelId, stored);
        }
    }

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

    // Model switch (ui-design §6.5, task 046): clamp-memory restore/remember +
    // 150 ms faceplate palette cross-fade + LCD layout swap + page rebuild.
    controlPanel.onModelChanged = [this](mws::model::ModelId id) {
        handleModelSwitch(id);
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

    // Fixed-aspect resizability (task 047, ui-design §5): the editor scales from
    // the 1000×380 base with the 1000/380 aspect locked between 0.6× and 2.0×
    // (600×228 / 2000×760). Vector rendering is resolution-independent (ADR-005),
    // so each cached layer (Faceplate/LCD/waveform) re-renders at the new scale
    // automatically on the resized() bounds change.
    ui::resize::configureConstrainer(constrainer);
    setConstrainer(&constrainer);
    // Host may resize the window (true); we manage our OWN corner grip (false)
    // so it is driven by the scale-persisting constrainer and positioned by
    // resized() at every scale — avoids two overlapping grips that
    // setResizable(true, true) would create alongside the explicit corner
    // (scope: setResizable + explicit ResizableCornerComponent).
    setResizable(/*allowHostToResize*/ true, /*useBottomRightCornerResizer*/ false);
    resizerCorner = std::make_unique<juce::ResizableCornerComponent>(this, &constrainer);
    addAndMakeVisible(*resizerCorner);

    // Restore the persisted scale (architecture.md §6 UI state). The size is
    // derived from the saved scale (clamped to the legal range); resized() lays
    // every child out proportionally. Suppress persistence during this initial
    // setSize so the restore can never round-trip-overwrite the saved value.
    {
        const auto& tree = processor.nonParameterState();
        const auto ui = tree.getChildWithName(state::id::uiState);
        const double savedScale = ui::resize::clampScale(static_cast<double>(
            ui.getProperty(state::id::scaleFactor, state::defaults::scaleFactor)));

        const juce::ScopedValueSetter<bool> guard(suppressScalePersist_, true);
        setSize(ui::resize::widthForScale(savedScale),
                ui::resize::heightForScale(savedScale));
    }

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
// Model switch (ui-design §6.5, task 046)
// ---------------------------------------------------------------------------

void PluginEditor::handleModelSwitch(mws::model::ModelId newModel)
{
    const auto oldModel = currentModel_;

    // (1) Clamp memory (ui-design §6.5 (PI)): remember the value effective on the
    // model we are leaving, restore the new model's pre-clamp value, and write
    // the new model's CLAMPED value to the (range-fixed) host parameter so the
    // engine/host see the value actually in effect (e.g. S1000 1500 -> S950 999).
    if (auto* tf = processor.parameterState().getParameter(paramid::timeFactor))
    {
        const double current = static_cast<double>(tf->convertFrom0to1(tf->getValue()));
        const double next = ui::applyModelSwitchTimeFactor(clampMemory_, oldModel,
                                                           newModel, current);
        if ((next < current) || (current < next))  // -Wfloat-equal-clean
            tf->setValueNotifyingHost(tf->convertTo0to1(static_cast<float>(next)));

        // Mirror the updated clamp map back into the task-029 state tree so it
        // survives save/reload (the schema is owned by 029; this only writes the
        // two models the switch touched).
        auto& tree = processor.nonParameterState();
        for (const auto m : { oldModel, newModel })
            if (clampMemory_.has(m))
                mws::plugin::state::setClampMemory(tree, m,
                                                   clampMemory_.recall(m, current));
    }

    currentModel_ = newModel;

    // (2) Faceplate palette cross-fade over 150 ms (ui-design §6.5) + the
    // dependent LCD/waveform/LookAndFeel spec swap (instant — the chassis blend
    // is purely cosmetic; the LCD re-layout (S950_2LINE <-> S1000_PAGE) and the
    // LcdPageModel rebuild happen on the next refreshLcd).
    faceplate.crossfadeToModel(newModel);
    lcd.setSpec(faceplate.spec());
    waveform.setSpec(faceplate.spec());
    lookAndFeel.setSpec(faceplate.spec());
    sendLookAndFeelChange();
    refreshLcd();
    repaint();
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

    // Bottom-right corner grip (task 047). Sized proportionally so it stays a
    // sensible target at every scale.
    if (resizerCorner != nullptr)
    {
        const int grip = juce::jmax(12, getWidth() / 50);
        resizerCorner->setBounds(getWidth() - grip, getHeight() - grip, grip, grip);
        resizerCorner->toFront(false);
    }
}

// ---------------------------------------------------------------------------
// Scale persistence + the §1 region-1 hamburger menu (task 047, ui-design §5)
// ---------------------------------------------------------------------------

void PluginEditor::persistScale()
{
    if (suppressScalePersist_)
        return;

    const double scale = ui::resize::clampScale(
        ui::resize::scaleForWidth(getWidth()));

    auto& tree = processor.nonParameterState();  // message thread only
    auto ui = tree.getChildWithName(state::id::uiState);
    if (! ui.isValid())
    {
        ui = juce::ValueTree(state::id::uiState);
        tree.appendChild(ui, nullptr);
    }
    ui.setProperty(state::id::scaleFactor, scale, nullptr);
}

void PluginEditor::applyScale(double scale)
{
    const double clamped = ui::resize::clampScale(scale);
    setSize(ui::resize::widthForScale(clamped), ui::resize::heightForScale(clamped));
    persistScale();  // setSize's resized() does not persist; do it explicitly
}

void PluginEditor::mouseDown(const juce::MouseEvent& event)
{
    // Clicking the §1 region-1 hamburger glyph opens the menu (about / manual /
    // scale). The glyph is drawn by the Faceplate at the header's right edge;
    // mirror that hit region here (geometry is single-authority — kHeader).
    namespace geo = ui::geometry;
    const auto header = ui::scaledRegion(geo::kHeader, getWidth(), getHeight());
    const float scale = (float) getWidth() / (float) geo::kBaseWidth;

    // The Faceplate insets the header by 4×scale then takes a (height*1.1) box
    // off the right for the menu; reconstruct that rectangle.
    const auto inset = header.reduced(4.0f * scale, 2.0f * scale);
    const float menuW = inset.getHeight() * 1.1f;
    const juce::Rectangle<float> menu(inset.getRight() - menuW, inset.getY(),
                                      menuW, inset.getHeight());

    if (menu.contains(event.position))
        showHamburgerMenu();
}

void PluginEditor::showHamburgerMenu()
{
    juce::PopupMenu menu;

    // "about" — version, AGPLv3 notice, credits (ui-design §1 region 1). Shown
    // as a modal info box so the entries below stay a flat action list.
    menu.addItem(1, "About mwStime…");

    // "manual" — opens the project manual / URL.
    menu.addItem(2, "Manual…");

    // "scale" submenu — 75/100/150/200% (ui-design §1 region 1). A tick marks
    // the entry nearest the current scale.
    const double current = ui::resize::scaleForWidth(getWidth());
    juce::PopupMenu scaleMenu;
    struct ScaleEntry { int id; const char* label; double scale; };
    static constexpr ScaleEntry kScales[] = {
        { 100, "75%", 0.75 }, { 101, "100%", 1.0 },
        { 102, "150%", 1.5 }, { 103, "200%", 2.0 },
    };
    for (const auto& s : kScales)
        scaleMenu.addItem(s.id, s.label, /*enabled*/ true,
                          /*ticked*/ std::abs(current - s.scale) < 0.05);
    menu.addSubMenu("Scale", scaleMenu);

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this),
        [this](int result) {
            switch (result)
            {
                case 0: break;  // dismissed
                case 1:         // About
                {
                    const juce::String about =
                        juce::String("mwStime ") + JucePlugin_VersionString + "\n\n"
                        + "Akai S-series timestretch emulation.\n"
                        + "Copyright (C) 2026 mattWoolly.\n\n"
                        + "Free software under the GNU Affero General Public "
                          "License v3 (AGPLv3) or later. This program comes with "
                          "ABSOLUTELY NO WARRANTY. Source: "
                          "https://github.com/mattWoolly/mwStime\n\n"
                        + "Credits: built on JUCE 8; clean-room vector UI "
                          "evoking the S900/S950/S1000/S1100 (no Akai assets).";
                    juce::NativeMessageBox::showMessageBoxAsync(
                        juce::MessageBoxIconType::InfoIcon, "About mwStime", about,
                        this);
                    break;
                }
                case 2:  // Manual
                    juce::URL("https://github.com/mattWoolly/mwStime/blob/main/"
                              "docs/design/ui-design.md")
                        .launchInDefaultBrowser();
                    break;
                case 100: applyScale(0.75); break;
                case 101: applyScale(1.0); break;
                case 102: applyScale(1.5); break;
                case 103: applyScale(2.0); break;
                default: break;
            }
        });
}

} // namespace mws::plugin
