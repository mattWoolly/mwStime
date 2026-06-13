// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Transpose — the separate pitch-shift pass that runs AFTER the stretch
// (docs/design/dsp-engine.md §7.2; docs/research/akaizer-analysis.md §2.4,
// §10: stretch first, repitch at playback).
//
// Two mutually exclusive per-model paths:
//   * S1000/S1100 (fixed-rate chain): anti-aliased windowed-sinc resample —
//     ratio p = 2^(−transpose/12) applied as an output-length resample
//     [AKZ §10]. The 16-tap Kaiser β=8 kernel is a PI-tagged stand-in for the
//     hardware's "interpolation and decimation 24-bit algorithm, custom VLSI"
//     [MAN §3 p.81] (ADR-001).
//   * S900/S950 (varclock chain): NO resampling in this stage. Transpose
//     multiplies the virtual DAC clock instead — clock = f_s × 2^(transpose/12)
//     (dsp-engine.md §8.1) — so the reconstruction filter and imaging track
//     pitch, the property RX950 lacks [DRR F7]. The product differentiator.

#pragma once

#include "mws/core/Buffer.h"
#include "mws/model/ModelId.h"

namespace mws::engine {

/// Pure, stateless transpose stage. Every method is `static`; deterministic
/// (architecture.md §7).
struct Transpose {
    /// S1000/S1100 path: resamples one channel of post-stretch material by
    /// ratio p = 2^(−semitones/12) through the 16-tap Kaiser β=8 windowed
    /// sinc (mws::core::SincResampler). Pitching up means p < 1 — the kernel
    /// is time-stretched so its cutoff lands on the new Nyquist
    /// (anti-aliasing, [AKZ §2.4] v1.2 changelog). Returns a fresh 1-channel
    /// buffer of ceil(in.size() × p) frames; `sampleRate` is left 0.0 (the
    /// playback rate is unchanged — the caller knows it). Content is delayed
    /// by SincResampler::groupDelaySamples(p) output samples.
    [[nodiscard]] static core::AudioBuffer
    transposeSinc(core::ConstAudioView in, double semitones);

    /// S900/S950 path: the virtual-voice-clock multiplier 2^(semitones/12)
    /// handed to VarClockChain::playback as `clockRatio`
    /// (dsp-engine.md §8.1: clock = f_s × 2^(transpose/12)). No resampling
    /// happens in this stage. Exact powers of two at octaves.
    [[nodiscard]] static double clockRatioFor(double semitones) noexcept;

    /// THE routing rule, encoded once so OfflineRenderer (task 020) can't
    /// mis-route (dsp-engine.md §7.2): true for the fixed-rate models
    /// (S1000/S1100 — and the reserved S3000 slot, ADR-004) which transpose
    /// via transposeSinc; false for the varclock models (S900/S950) which
    /// transpose via clockRatioFor only.
    [[nodiscard]] static bool usesSincTranspose(model::ModelId id) noexcept;
};

} // namespace mws::engine
