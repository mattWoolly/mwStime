// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

namespace mws::core {

/// S1000/S1100 "digital moving low-pass" voice filter, -18 dB/oct
/// (docs/design/dsp-engine.md §8.2; docs/design/architecture.md §2.1).
///
/// A TRUE 3rd-order Butterworth low-pass: one bilinear first-order section
/// (the real pole) cascaded with one biquad whose complex pair has Q = 1.0 —
/// NOT three identical one-poles, which is not Butterworth (panel critique
/// P15). On hardware this per-voice keygroup filter is FULLY OPEN by default,
/// so the default-constructed state is a transparent passthrough; it colours
/// audio only after setCutoff().
///
/// All methods are allocation-free; coefficient computation is closed-form
/// (bilinear transform with cutoff prewarp), safe to call per-note on the
/// audio thread (architecture.md §4.2).
///
/// The S3000 -12 dB/oct resonant SVF is a v1.1 slot (ADR-004) — not here.
class MovingLpf3
{
public:
    /// Default state: fully open (transparent), matching the hardware.
    MovingLpf3() noexcept = default;

    /// Engages the filter at `cutoffHz` (prewarped bilinear 3rd-order
    /// Butterworth). `cutoffHz` is clamped to [1, 0.49 * sampleRate].
    /// Closed-form, allocation-free; does not clear state (call reset() for
    /// that).
    void setCutoff(double cutoffHz, double sampleRate) noexcept;

    /// Returns to the default transparent passthrough state.
    void setFullyOpen() noexcept;

    /// Clears the filter state (delay lines); keeps the current coefficients
    /// and open/engaged mode.
    void reset() noexcept;

    /// In-place processing of one channel. Transparent when fully open.
    void process(AudioView view) noexcept;

private:
    // First-order section (the real pole):  y = b0*x + b1*x1 - a1*y1.
    double foB0_ = 0.0, foB1_ = 0.0, foA1_ = 0.0;
    double foX1_ = 0.0, foY1_ = 0.0;

    // Biquad section (complex pair, Q = 1.0), transposed direct form II.
    double bqB0_ = 0.0, bqB1_ = 0.0, bqB2_ = 0.0, bqA1_ = 0.0, bqA2_ = 0.0;
    double bqS1_ = 0.0, bqS2_ = 0.0;

    bool fullyOpen_ = true;
};

} // namespace mws::core
