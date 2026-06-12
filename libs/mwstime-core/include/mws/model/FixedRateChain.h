// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

#include <cstdint>

namespace mws::model {

/// S1000/S1100 fixed-rate 16-bit character chain
/// (docs/design/dsp-engine.md §8.2; deep-research-report Findings 5 & 8).
///
/// Chain order is the TAL-validated hardware order (§8 intro): ingest()
/// runs BEFORE the stretch engine — the hardware stretched already-quantized,
/// already-band-limited sample RAM — and playback() runs AFTER
/// stretch+transpose. Both stages are offline, one channel in -> one fresh
/// channel out, float32 storage throughout ("bit depth" stages quantize
/// values, they do not change storage type).
///
/// The §8.2 VOICE FILTER (MovingLpf3, -18 dB/oct) sits in playback() FULLY
/// OPEN — transparent — matching the hardware default; hard-wiring it active
/// would fabricate an artifact, so this API deliberately exposes NO filter
/// control (additive later, dsp-engine.md §8.2).
///
/// S1100 delta: identical engine + chain; the 20-bit DAC is modelled as a
/// 16-bit output quantize with seeded TPDF dither (lower noise floor, PI).
/// There is NO output-level offset between S1000 and S1100 (panel ruling,
/// dsp-engine.md §2 outTrim row).
///
/// The S3000 28-bit/resonant-filter chain is v1.1 (ADR-004) — not here.
/// The transpose stage itself is task 018 — not here.
class FixedRateChain
{
public:
    /// Ingest emulation (runs before the stretch engine): windowed-sinc
    /// resample from `inRate` to `modelRate` (the §2 `sampleRateSel` rates,
    /// 44100 or 22050), then 16-bit mid-tread quantize (no dither). The
    /// returned buffer is what the stretch engine sees: already band-limited,
    /// already on the 16-bit lattice. `result.sampleRate == modelRate`.
    [[nodiscard]] static core::AudioBuffer ingest(core::ConstAudioView input,
                                                  double inRate,
                                                  double modelRate);

    /// Playback emulation (runs after stretch+transpose): the §8.2 voice
    /// filter fully open (transparent — present in the chain, default-open as
    /// on hardware), then windowed-sinc resample from `modelRate` to
    /// `hostRate`; when `s1100Dither` is true (S1100 only), a final 16-bit
    /// quantize with TPDF dither seeded by `seed` (deterministic: same seed,
    /// same input => bit-identical output). `result.sampleRate == hostRate`.
    [[nodiscard]] static core::AudioBuffer playback(core::ConstAudioView input,
                                                    double modelRate,
                                                    double hostRate,
                                                    bool s1100Dither,
                                                    std::uint64_t seed);
};

} // namespace mws::model
