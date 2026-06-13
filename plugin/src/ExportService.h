// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ExportService (task 036) — the modern half of the render-to-new-sample
// workflow: the published RenderedSample exports as a WAV by being dragged out
// of the plugin (to the DAW timeline or the desktop), plus a "save as…"
// file-chooser fallback (architecture.md §5.1: RenderedSample -> drag-out /
// save-as WAV; ui-design.md §6.3 step 4; §1 floppy-slot easter egg).
//
// Threading: ALL of ExportService runs on the MESSAGE THREAD (drag-and-drop,
// file chooser, file IO — architecture.md §4). It reads the published render via
// EngineHost::currentRender() (an atomic load of the shared_ptr, the producer/
// message-thread side of the §4 publication protocol); the buffer is immutable
// once published so writing it to disk needs no lock. Nothing here touches the
// audio thread.
//
// What it exposes for the UI (the WaveformView drag-out hook + floppy slot, wired
// in 043/045b — out of scope here):
//   - startDrag(Component&): renders the current published sample to a temp WAV
//     and initiates a JUCE external file drag (performExternalDragDropOfFiles)
//     so the host/desktop receives the file,
//   - saveAs(...): an async file chooser that writes the render to a chosen path,
//   - exportToTempFile(): the testable file-write half of startDrag.
//
// Guard (acceptance criterion): with no render published, every export entry
// point is a no-op that posts the typed ExportOutcome::NoRender event for the LCD
// (architecture.md §9 — this layer emits a typed enum, never an LCD string).
//
// WAV writing: deterministic and bit-faithful through the project's own
// dependency-free WAV layer (mws::core::WavIo, task 003) at 16-bit by default
// (PI — no source bit-depth is carried on RenderedSample) or 24-bit. See the
// DEVIATION note on the .cpp: the task scope names juce::AudioFormatWriter, but
// the acceptance gate is "exported WAV round-trips via WavIo and is bit-faithful
// at the chosen depth" — only WavIo's own writer makes that round trip exact, so
// the temp/save-as write goes through WavIo (JUCE-free, deterministic) rather
// than JUCE's writer, whose int rounding differs.
//
// Temp-file lifetime (acceptance criterion): every temp WAV created for a
// drag-out is tracked and deleted in the destructor, so plugin teardown leaves
// no stray render files behind.

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

#include "mws/engine/Params.h"

namespace juce {
class Component;
class FileChooser; // held by unique_ptr; defined in juce_gui_basics (ExportService.cpp)
} // namespace juce

namespace mws::plugin {

class EngineHost;
struct RenderedSample;

/// Output bit depth for the exported WAV. 16-bit is the default policy (PI —
/// RenderedSample carries no source depth; 16-bit is the S-series sampler era
/// norm and the smallest faithful choice). 24-bit is offered for transparency.
enum class ExportDepth : std::uint8_t { Bits16, Bits24 };

/// The typed result of an export attempt — the LCD-feedback contract for this
/// layer (architecture.md §9: typed enum here, LCD string in the UI layer).
enum class ExportOutcome : std::uint8_t {
    Ok,        ///< the WAV was written (or the external drag was initiated)
    NoRender,  ///< nothing has been rendered/published — export was a no-op
    WriteError ///< the WAV write failed (disk full / permissions / bad path)
};

/// The message-thread export service. Construct one per plugin instance, attach
/// it to the EngineHost, and feed it the loaded sample name + current render
/// params so the deterministic file name can be built.
class ExportService
{
public:
    /// The result of a file-producing export: the outcome plus the file written
    /// (empty / non-existent unless outcome == Ok).
    struct ExportResult {
        ExportOutcome outcome = ExportOutcome::NoRender;
        juce::File file{};
    };

    // Constructor and destructor are out-of-line so the unique_ptr<FileChooser>
    // member only needs FileChooser complete in ExportService.cpp.
    ExportService();
    ~ExportService();

    ExportService(const ExportService&) = delete;
    ExportService& operator=(const ExportService&) = delete;

    // --- Wiring (message thread) ---------------------------------------------

    /// Attach the engine host whose published render is exported. Non-owning;
    /// the host must outlive this service (both are owned by the processor).
    void attach(EngineHost* host) noexcept { host_ = host; }

    /// The loaded sample's display/base name (the FileLoader name, e.g.
    /// "AMEN_165" or "break.wav") used as the file-name stem. Any extension and
    /// path-illegal characters are stripped/sanitized when the name is built.
    void setSampleName(juce::String name) { sampleName_ = std::move(name); }

    /// The render params used for the most recent GO (model + time factor feed
    /// the deterministic file name). Message thread.
    void setRenderParams(const mws::engine::ParamSnapshot& params) noexcept
    {
        params_ = params;
    }

    /// The output bit-depth policy (default Bits16, PI). Message thread.
    void setDepth(ExportDepth depth) noexcept { depth_ = depth; }
    [[nodiscard]] ExportDepth depth() const noexcept { return depth_; }

    /// Optional LCD-feedback sink: invoked on the message thread with the typed
    /// outcome of every export attempt (including the NoRender guard).
    std::function<void(ExportOutcome)> onExportEvent;

    // --- File-name policy -----------------------------------------------------

    /// The deterministic export file name: `<sample>_<model>_<timeFactor>.wav`
    /// (architecture.md §5.1; ui-design.md §6.3 step 4). `sampleName` is
    /// sanitized to a single legal path segment (extension dropped, illegal
    /// characters replaced); a blank result falls back to "render". The time
    /// factor is the integer percent when whole (e.g. `300`), else one decimal
    /// (e.g. `174.5`). The hardware-flavored `*ST` suffix is a DISPLAY-only
    /// flourish (ui-design mockup) and never appears in the file name.
    [[nodiscard]] static juce::String deterministicFileName(
        const juce::String& sampleName, const mws::engine::ParamSnapshot& params);

    // --- Export entry points (message thread) ---------------------------------

    /// Write a given RenderedSample to `file` at `depth`. Pure file IO (the
    /// testable core of every export path). Returns Ok on success, WriteError on
    /// failure. Does NOT consult the published render or post events — callers
    /// that should honor the no-render guard use exportToTempFile / saveAs.
    [[nodiscard]] ExportOutcome writeRender(const RenderedSample& render,
                                            const juce::File& file,
                                            ExportDepth depth) const;

    /// Render the currently published sample to a fresh temp WAV (the file the
    /// drag-out hands to the host/desktop). The temp file is TRACKED for cleanup
    /// in the destructor. Honors the no-render guard (posts NoRender, writes
    /// nothing). Message thread.
    [[nodiscard]] ExportResult exportToTempFile();

    /// As exportToTempFile, but for an explicitly supplied render (used by the
    /// drag-out path which has already acquired the published pointer, and by
    /// tests). The temp file is tracked for cleanup. Message thread.
    [[nodiscard]] ExportResult exportRenderToTempFile(const RenderedSample& render);

    /// Initiate an external file drag of the current render (the WaveformView /
    /// floppy-slot drag-out, ui-design.md §6.3 step 4 / §1). Renders to a temp
    /// WAV (tracked for cleanup) and calls performExternalDragDropOfFiles via the
    /// nearest DragAndDropContainer of `sourceComponent`. Honors the no-render
    /// guard. Returns the outcome (NoRender / WriteError short-circuit before the
    /// drag; Ok once the drag is initiated). Message thread.
    ExportOutcome startDrag(juce::Component& sourceComponent);

    /// "Save as…" fallback (ui-design.md §6.3 step 4 — the explicit save path).
    /// Opens an ASYNC file chooser anchored at `parent` (may be nullptr),
    /// defaulting to the deterministic file name, and writes the render on the
    /// message thread when the user confirms. Honors the no-render guard
    /// up-front (posts NoRender, opens no chooser). `onDone` (optional) receives
    /// the final outcome. Message thread; never blocks (no modal loop).
    void saveAs(juce::Component* parent,
                std::function<void(ExportOutcome)> onDone = {});

    /// Test/diagnostic: number of temp files currently tracked for cleanup.
    [[nodiscard]] std::size_t trackedTempFileCount() const noexcept
    {
        return tempFiles_.size();
    }

private:
    /// Read the currently published render (message-thread side of the §4
    /// publication protocol). Returns nullptr when nothing is published.
    [[nodiscard]] std::shared_ptr<const RenderedSample> currentRender() const;

    /// Post the typed outcome to onExportEvent (if set) and return it.
    ExportOutcome report(ExportOutcome outcome) const;

    /// Build the absolute temp-file path for the current name and remember it.
    juce::File makeTrackedTempFile();

    EngineHost* host_ = nullptr;
    juce::String sampleName_;
    mws::engine::ParamSnapshot params_{};
    ExportDepth depth_ = ExportDepth::Bits16;

    // Temp WAVs created for drag-outs; deleted in the destructor (acceptance
    // criterion: temp files cleaned up on plugin destruction).
    std::vector<juce::File> tempFiles_;

    // The async save-as chooser is held alive across its callback.
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace mws::plugin
