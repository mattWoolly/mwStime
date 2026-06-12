// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// TempoMap — pure, host-agnostic math for FX-mode tempo sync. No JUCE, no
// transport reading (that is task 037 / the JUCE AudioPlayHead) and no
// RealtimeStretcher resync logic (task 023 consumes this math). This is just
// the arithmetic:
//   * the tempo-synced time factor (dsp-engine.md §2 `tempoSync` row, the
//     direction-corrected `timeFactor = 100 × sourceBPM / hostBPM`),
//   * CLASSIC integer quantization of that factor + the LCD achieved-length
//     hint (ui-design §6.2 — the LCD shows the achieved value, never the
//     requested one),
//   * transport-aligned `fxWindow` boundary computation for 1/4…8-bar windows
//     (architecture.md §5.2), and
//   * the no-transport / Standalone wall-clock fallback (last-known tempo or
//     120 BPM (PI), architecture.md §5.2 / ADR-006).

#pragma once

#include <cstdint>

#include "mws/engine/Params.h"

namespace mws::engine {

/// Pure tempo-sync arithmetic. Stateless: every method is `static`.
struct TempoMap {
    /// Default tempo used when no transport tempo has ever been observed
    /// (architecture.md §5.2 / ADR-006: "last known tempo or 120 BPM (PI)").
    static constexpr double kFallbackBpm = 120.0;

    /// Result of a transport-aligned window boundary computation.
    struct WindowBoundary {
        /// PPQ (quarter-note) position of the start of the window that
        /// contains `ppqPosition`. Window starts land on the sub-bar grid.
        double windowStartPpq = 0.0;
        /// Host-rate samples from `ppqPosition` to the next window boundary
        /// (the resync point in SYNC mode).
        double samplesToNextBoundary = 0.0;
    };

    /// timeFactor (percent) := 100 × sourceBPM / hostBPM. Direction-corrected
    /// per panel critique (dsp-engine.md §2): a 174 BPM loop into an 87 BPM
    /// host ⇒ 200% (longer). FX clamping of T<100% is handled elsewhere.
    static double syncedTimeFactor(double sourceBpm, double hostBpm);

    /// CLASSIC coerces the time factor to an integer percent ("Akai samplers
    /// don't use decimal values for Time Factor", AKZ §2.1). Round-half-up.
    static int quantizeClassic(double pct);

    /// LCD achieved-length hint: the source length scaled by the (already
    /// CLASSIC-quantized) integer factor. This is the display hint only — the
    /// engine's true schedule-derived output length (dsp-engine.md §3.4) is a
    /// separate computation that accounts for hop rounding.
    static std::int64_t achievedLengthSamples(int quantizedPct,
                                              std::int64_t sourceLengthSamples);

    /// Transport-aligned window boundary for a 1/4…8-bar window. `tsNumerator`
    /// / `tsDenominator` are the host time signature (3/4 vs 4/4 give a
    /// different bar length). `FxWindow::Free` has no transport sync and
    /// returns a zero-length boundary at the current position.
    static WindowBoundary windowBoundary(double ppqPosition, double bpm,
                                        int tsNumerator, int tsDenominator,
                                        FxWindow window, double hostRate);

    /// No-transport / Standalone fallback: a wall-clock window length in
    /// host-rate samples, assuming 4/4. `lastKnownBpm <= 0` (no tempo ever
    /// observed) falls back to `kFallbackBpm` (120 BPM, PI).
    static double windowFromWallClock(double lastKnownBpm, double hostRate,
                                      FxWindow window);

    /// The window length expressed in bars (QuarterBar ⇒ 0.25 … EightBars ⇒
    /// 8.0). `Free` ⇒ 0.0 (no transport-aligned length).
    static double windowBars(FxWindow window);
};

} // namespace mws::engine
