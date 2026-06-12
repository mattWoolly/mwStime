// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

namespace mws::model {

/// S900/S950 variable-clock 12-bit character chain
/// (docs/design/dsp-engine.md §8.1; deep-research-report.md Finding 4 —
/// service-manual grade: per-voice BA9221 12-bit DAC + MF6CN-50 clock-tunable
/// switched-capacitor filter, sample rate = 2.5 x bandwidth).
///
///   ingest:   resample to f_s = 2.5 x bandwidth, 12-bit mid-tread quantize
///             (no dither (PI)). Values are quantized, storage stays float32 —
///             stretch arithmetic remains 16-bit-precision capable ("12-bit
///             sampling/16-bit processing" [MAN §2 p.74]).
///   playback: zero-order hold at the virtual voice clock
///             clock = f_s x clockRatio, NO interpolation [DRR F4], rendered
///             at an internal oversampled rate >= 2 x max(clock, hostRate)
///             (4x host default (PI)) so the ZOH images are represented
///             before filtering; 6th-order Butterworth reconstruction at
///             cutoff = clock / 2.5 TRACKING the clock; sinc-decimate to
///             host rate (dsp-engine.md §8.1 IMPLEMENTATION NOTE).
///
/// There is deliberately NO saturation/drive stage anywhere in this chain:
/// the "stacked preamp saturation" explanation of the S900 sound was refuted
/// 0-3 (deep-research-report.md refuted #4).
///
/// Offline-buffer API (mono — the S950 is a mono machine; the stereo sum
/// rule lives in CharacterChain, task 019). This is the heaviest DSP in the
/// plugin and the early-risk prototype (architecture.md §10 risk 3); the
/// CPU datum lives in tests/unit/test_varclock.cpp's benchmark case.
class VarClockChain
{
public:
    /// Model rate rule [DRR F4]: f_s = 2.5 x bandwidth.
    /// bandwidth 3..16 kHz (S900, rate <= 40 kHz) / 3..19.2 kHz (S950, 48 kHz).
    [[nodiscard]] static double modelRateFor(double bandwidthKHz) noexcept
    {
        return 2.5 * bandwidthKHz * 1000.0;
    }

    /// Ingest emulation (placed BEFORE the stretch engine — the hardware
    /// stretched already-quantized, already-band-limited sample RAM [DRR F8]):
    /// windowed-sinc resample from `inRate` to f_s = 2.5 x bandwidth, then
    /// 12-bit mid-tread quantize, no dither (PI). Returns a 1-channel buffer
    /// with `sampleRate` = f_s. `inRate` > 0, `bandwidthKHz` > 0.
    [[nodiscard]] static core::AudioBuffer
    ingest(core::ConstAudioView in, double inRate, double bandwidthKHz);

    /// Intermediate result of the oversampled ZOH render (pre-filter) —
    /// exposed so the §3.9 image-spectrum property tests can probe the
    /// images before reconstruction (testing-strategy.md §3 item 9).
    struct OversampledZoh
    {
        core::AudioBuffer audio; ///< 1 channel at `rate`, pre-Butterworth.
        double rate = 0.0;       ///< Internal oversampled rate (Hz).
        double clockHz = 0.0;    ///< Virtual voice clock f_s x clockRatio (Hz).
    };

    /// Renders `stretched` (one channel of post-stretch material living at
    /// model rate `fs`) through the virtual variable-clock ZOH at
    /// clock = fs x clockRatio, into an internal oversampled rate
    /// max(4 x hostRate (PI), 2 x clock) — always >= 2 x max(clock, hostRate)
    /// (dsp-engine.md §8.1 IMPLEMENTATION NOTE). No interpolation [DRR F4].
    /// `fs`, `clockRatio`, `hostRate` all > 0.
    [[nodiscard]] static OversampledZoh
    renderZoh(core::ConstAudioView stretched, double fs, double clockRatio,
              double hostRate);

    /// Full playback emulation (placed AFTER stretch + transpose):
    /// renderZoh, then the clock-tracking 6th-order Butterworth at
    /// cutoff = clock / 2.5 run at the oversampled rate, then windowed-sinc
    /// decimation to `hostRate`. Returns a 1-channel buffer with
    /// `sampleRate` = hostRate. Deterministic (architecture.md §7).
    [[nodiscard]] static core::AudioBuffer
    playback(core::ConstAudioView stretched, double fs, double clockRatio,
             double hostRate);
};

} // namespace mws::model
