// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdPageBinding (task 045) — the glue between the headless LcdPageModel (041)
// and the on-screen LcdDisplay (040), plus the UI-FIFO fold the editor's 30 Hz
// poll uses (docs/design/architecture.md §4 "render-done / progress / LCD
// updates via lock-free FIFO → timer poll").
//
//   · applyWorkerEvent — folds one EngineHost::WorkerEvent (Started / Progress
//     / Finished) into the LcdRenderInfo the page model reads, so progress and
//     the terminal outcome (NotEnoughMemory) become the §6.2/§6.3 LCD message
//     lines. Pure: no JUCE, no LCD, headlessly testable;
//   · renderPage — writes a built LcdPage's rows + per-cell styles into an
//     LcdDisplay cell-by-cell and parks the block cursor on the focused field
//     (LcdField row/col). The LCD content comes ONLY from the page model
//     (acceptance criterion: "no ad-hoc strings in the editor").

#pragma once

#include "EngineHost.h"  // mws::plugin::WorkerEvent / RenderOutcome
#include "LcdDisplay.h"
#include "LcdPageModel.h"

namespace mws::ui {

/// Map the page model's per-cell style to the LcdDisplay style enum (the two
/// enums are intentionally separate — the page model is JUCE-free).
[[nodiscard]] LcdDisplay::Style toDisplayStyle(LcdCellStyle style) noexcept;

/// Fold one worker FIFO event into `info` (architecture.md §4 timer poll):
///   · Started  — clears any stale not-enough-memory flag, opens progress at 0;
///   · Progress — sets progressPercent from ev.progress (0..1 → 0..100);
///   · Finished — clears progress; NotEnoughMemory raises the refusal flag,
///     Completed/Aborted just close the progress line.
void applyWorkerEvent(const mws::plugin::WorkerEvent& ev, LcdRenderInfo& info) noexcept;

/// True when this drained worker event means a render just FINISHED and PUBLISHED
/// a new RenderedSample (Kind::Finished with RenderOutcome::Completed) — the seam
/// the editor's poll uses to decide it should bridge `EngineHost::currentRender()`
/// into the WaveformView (task 056). Pure/headless so the render→view bridge is
/// testable without the editor: a NotEnoughMemory/Aborted Finished, or any
/// Started/Progress event, returns false (nothing new was published). The editor's
/// pollEngine() copies the published render into `WaveformView::setRenderedSample`
/// only when this returns true, re-enabling hasRender() ⇒ drag-out + A/B overlay.
[[nodiscard]] bool renderFinishedSuccessfully(const mws::plugin::WorkerEvent& ev) noexcept;

/// Push a built page into the display: layout (rows count → S950_2Line / page),
/// every cell's character + style, and the block cursor on the field at
/// `cursorFieldIndex` in the page's field map (negative hides the cursor).
void renderPage(const LcdPage& page, LcdDisplay& lcd, int cursorFieldIndex);

} // namespace mws::ui
