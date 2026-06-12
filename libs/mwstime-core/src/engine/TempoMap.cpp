// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// TempoMap implementation. See mws/engine/TempoMap.h for the contract and the
// design-doc citations.

#include "mws/engine/TempoMap.h"

#include <cmath>

namespace mws::engine {

double TempoMap::windowBars(FxWindow window)
{
    switch (window) {
    case FxWindow::QuarterBar: return 0.25;
    case FxWindow::HalfBar:    return 0.5;
    case FxWindow::OneBar:     return 1.0;
    case FxWindow::TwoBars:    return 2.0;
    case FxWindow::FourBars:   return 4.0;
    case FxWindow::EightBars:  return 8.0;
    case FxWindow::Free:       return 0.0; // no transport-aligned length
    }
    return 0.0;
}

double TempoMap::syncedTimeFactor(double sourceBpm, double hostBpm)
{
    // timeFactor := 100 × sourceBPM / hostBPM (dsp-engine.md §2, direction
    // panel-corrected). Guard against a non-positive host tempo.
    if (hostBpm <= 0.0)
        return 100.0;
    return 100.0 * sourceBpm / hostBpm;
}

int TempoMap::quantizeClassic(double pct)
{
    // Round-half-up to an integer percent (AKZ §2.1).
    return static_cast<int>(std::llround(pct));
}

std::int64_t TempoMap::achievedLengthSamples(int quantizedPct,
                                             std::int64_t sourceLengthSamples)
{
    // LCD hint: source length scaled by the quantized integer factor. Use
    // double then round — the true schedule-derived length (§3.4) is separate.
    const double scaled =
        static_cast<double>(sourceLengthSamples) * static_cast<double>(quantizedPct)
        / 100.0;
    return static_cast<std::int64_t>(std::llround(scaled));
}

TempoMap::WindowBoundary TempoMap::windowBoundary(double ppqPosition, double bpm,
                                                  int tsNumerator,
                                                  int tsDenominator,
                                                  FxWindow window,
                                                  double hostRate)
{
    WindowBoundary result;

    const double bars = windowBars(window);
    if (bars <= 0.0 || bpm <= 0.0 || tsNumerator <= 0 || tsDenominator <= 0) {
        // Free (no transport sync) or degenerate input: a zero-length window
        // anchored at the current position.
        result.windowStartPpq = ppqPosition;
        result.samplesToNextBoundary = 0.0;
        return result;
    }

    // PPQ is measured in quarter notes. A bar spans `numerator` beats, each
    // beat being `4/denominator` quarter notes — 3/4 and 4/4 differ here.
    const double barLengthQuarters =
        static_cast<double>(tsNumerator) * 4.0 / static_cast<double>(tsDenominator);
    const double windowLengthQuarters = bars * barLengthQuarters;

    // Snap to the sub-bar grid: the window containing ppqPosition starts at the
    // largest multiple of windowLengthQuarters at or below it.
    const double windowsElapsed = std::floor(ppqPosition / windowLengthQuarters);
    result.windowStartPpq = windowsElapsed * windowLengthQuarters;

    const double nextBoundaryPpq = result.windowStartPpq + windowLengthQuarters;
    const double quartersToBoundary = nextBoundaryPpq - ppqPosition;

    // Quarter-note duration in seconds = 60 / bpm.
    const double secondsToBoundary = quartersToBoundary * 60.0 / bpm;
    result.samplesToNextBoundary = secondsToBoundary * hostRate;
    return result;
}

double TempoMap::windowFromWallClock(double lastKnownBpm, double hostRate,
                                     FxWindow window)
{
    // No transport / Standalone: assume 4/4 and use the last-known tempo, or
    // 120 BPM if none was ever observed (architecture.md §5.2 / ADR-006).
    const double bpm = (lastKnownBpm > 0.0) ? lastKnownBpm : kFallbackBpm;
    const double bars = windowBars(window);
    if (bars <= 0.0)
        return 0.0;

    // 4/4 ⇒ 4 quarter notes per bar.
    const double windowLengthQuarters = bars * 4.0;
    const double seconds = windowLengthQuarters * 60.0 / bpm;
    return seconds * hostRate;
}

} // namespace mws::engine
