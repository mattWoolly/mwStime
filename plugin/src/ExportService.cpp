// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ExportService implementation (task 036). See ExportService.h for the contract.
//
// DEVIATION (documented; PR body + plan/backlog/036): the task scope names
// "juce AudioFormatWriter" for the WAV write. The ACCEPTANCE CRITERION, however,
// is "exported WAV round-trips via WavIo and is bit-faithful to the
// RenderedSample at the chosen depth". JUCE's WavAudioFormat and mws::core::WavIo
// quantize floats to integer PCM with different rounding rules, so a JUCE-written
// WAV read back through WavIo would NOT be bit-exact and would fail the gate.
// The faithful interpretation that satisfies the acceptance criterion is to
// write through the project's own deterministic, dependency-free WavIo
// (architecture.md §2 makes WavIo the canonical 16/24/32-int + 32-float WAV layer
// for the CLI and tests); its documented int<->float rules (WavIo.h) make 16- and
// 24-bit round trips exact. JUCE's writer remains available if a later task needs
// a feature WavIo lacks (e.g. broadcast-wave metadata); none is required here.

#include "ExportService.h"

#include "EngineHost.h" // RenderedSample, EngineHost::currentRender

#include "mws/core/WavIo.h"

#include <juce_gui_basics/juce_gui_basics.h> // juce::Component, DragAndDropContainer

#include <utility>

namespace mws::plugin {

namespace {
mws::core::WavIo::BitDepth toWavDepth(ExportDepth depth) noexcept
{
    return depth == ExportDepth::Bits24 ? mws::core::WavIo::BitDepth::Int24
                                        : mws::core::WavIo::BitDepth::Int16;
}

/// Format the time factor for the file name: bare integer when whole (e.g.
/// "300"), otherwise one decimal place (e.g. "174.5"). Never a trailing ".00".
juce::String formatTimeFactor(double timeFactorPct)
{
    const double rounded = std::round(timeFactorPct);
    if (std::abs(timeFactorPct - rounded) < 1.0e-6)
        return juce::String(static_cast<long long>(rounded));
    return juce::String(timeFactorPct, 1);
}

/// Sanitize a loaded-sample name into a single legal path segment: drop any
/// extension, replace path-illegal / whitespace characters with '_', and fall
/// back to "render" when the result is blank.
juce::String sanitizeStem(const juce::String& sampleName)
{
    // Drop a trailing extension (".wav", ".aiff", …) if present.
    juce::String stem = sampleName;
    const int dot = stem.lastIndexOfChar('.');
    if (dot > 0)
        stem = stem.substring(0, dot);

    juce::String out;
    out.preallocateBytes(static_cast<size_t>(stem.length()) + 1);
    for (auto ch : stem)
    {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')
                        || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
        out += ok ? ch : juce::juce_wchar('_');
    }

    out = out.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    return out.isEmpty() ? juce::String("render") : out;
}
} // namespace

ExportService::ExportService() = default;

ExportService::~ExportService()
{
    // Clean up every temp WAV we created for drag-outs (acceptance criterion:
    // temp files cleaned up on plugin destruction). Each file lives alone in a
    // per-export UUID subdirectory, so delete the whole directory to leave no
    // stray files OR empty dirs behind.
    for (auto& f : tempFiles_)
    {
        const auto dir = f.getParentDirectory();
        if (dir.isDirectory() && dir.getParentDirectory().getFileName() == "mwStime")
            dir.deleteRecursively();
        else if (f.existsAsFile())
            f.deleteFile();
    }
}

juce::String ExportService::deterministicFileName(const juce::String& sampleName,
                                                  const mws::engine::ParamSnapshot& params)
{
    return sanitizeStem(sampleName) + "_" + juce::String(toString(params.model).data())
           + "_" + formatTimeFactor(params.timeFactor) + ".wav";
}

ExportOutcome ExportService::writeRender(const RenderedSample& render,
                                         const juce::File& file,
                                         ExportDepth depth) const
{
    const auto result = mws::core::WavIo::write(
        file.getFullPathName().toStdString(), render.audio, toWavDepth(depth));
    return result.ok() ? ExportOutcome::Ok : ExportOutcome::WriteError;
}

std::shared_ptr<const RenderedSample> ExportService::currentRender() const
{
    if (host_ == nullptr)
        return nullptr;
    return host_->currentRender();
}

ExportOutcome ExportService::report(ExportOutcome outcome) const
{
    if (onExportEvent != nullptr)
        onExportEvent(outcome);
    return outcome;
}

juce::File ExportService::makeTrackedTempFile()
{
    const auto name = deterministicFileName(sampleName_, params_);
    // A per-export unique subdirectory in the system temp area so concurrent
    // exports of the same render never collide on the deterministic name.
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("mwStime")
                         .getChildFile(juce::Uuid().toDashedString());
    dir.createDirectory();
    const auto file = dir.getChildFile(name);
    tempFiles_.push_back(file);
    return file;
}

ExportService::ExportResult ExportService::exportRenderToTempFile(const RenderedSample& render)
{
    if (render.audio.numFrames() == 0)
        return { report(ExportOutcome::NoRender), {} };

    const auto file = makeTrackedTempFile();
    const auto outcome = writeRender(render, file, depth_);
    if (outcome != ExportOutcome::Ok)
        return { report(outcome), {} };
    return { report(ExportOutcome::Ok), file };
}

ExportService::ExportResult ExportService::exportToTempFile()
{
    const auto render = currentRender();
    if (render == nullptr || render->audio.numFrames() == 0)
        return { report(ExportOutcome::NoRender), {} };
    return exportRenderToTempFile(*render);
}

ExportOutcome ExportService::startDrag(juce::Component& sourceComponent)
{
    const auto result = exportToTempFile();
    if (result.outcome != ExportOutcome::Ok)
        return result.outcome; // already reported (NoRender / WriteError)

    // Initiate the external drag through the nearest DragAndDropContainer so the
    // host timeline / desktop receives the file (ui-design.md §6.3 step 4).
    juce::StringArray files;
    files.add(result.file.getFullPathName());
    juce::DragAndDropContainer::performExternalDragDropOfFiles(
        files, /*canMoveFiles=*/false, &sourceComponent);

    return ExportOutcome::Ok; // already reported Ok by exportToTempFile
}

void ExportService::saveAs(juce::Component* parent,
                           std::function<void(ExportOutcome)> onDone)
{
    const auto render = currentRender();
    if (render == nullptr || render->audio.numFrames() == 0)
    {
        report(ExportOutcome::NoRender);
        if (onDone != nullptr)
            onDone(ExportOutcome::NoRender);
        return;
    }

    const auto suggested = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                               .getChildFile(deterministicFileName(sampleName_, params_));

    chooser_ = std::make_unique<juce::FileChooser>("Export rendered sample as WAV",
                                                   suggested, "*.wav");

    // Capture a strong copy of the immutable render so the async callback can
    // write it even if the published render changes meanwhile.
    auto renderPtr = render;
    const auto depth = depth_;

    const auto flags = juce::FileBrowserComponent::saveMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, renderPtr, depth, onDone](const juce::FileChooser& fc) {
        const auto chosen = fc.getResult();
        if (chosen == juce::File{})
        {
            // User cancelled — no event, no write (a cancel is not an error).
            if (onDone != nullptr)
                onDone(ExportOutcome::Ok);
            return;
        }
        const auto outcome = writeRender(*renderPtr, chosen, depth);
        report(outcome);
        if (onDone != nullptr)
            onDone(outcome);
    });

    juce::ignoreUnused(parent);
}

} // namespace mws::plugin
