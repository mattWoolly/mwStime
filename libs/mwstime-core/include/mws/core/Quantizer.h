// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

#include <cstdint>

namespace mws::core {

/// Bit-depth quantization for the character chains
/// (docs/design/architecture.md §2.1, docs/design/dsp-engine.md §8).
///
/// Mid-tread quantizer over full scale ±1.0: values are snapped to the
/// nearest of 2^bits levels spaced `step() = 2 / 2^bits` apart, with zero a
/// code (mid-tread) and codes clamped to [-2^(bits-1), 2^(bits-1) - 1] —
/// two's-complement sample RAM, so at most 2^bits distinct output values.
/// Values are quantized, storage stays float32 ("bit depth stages quantize
/// values, they do not change storage type", dsp-engine.md §8 intro).
///
/// - 12-bit, no dither: S900/S950 ingest (dsp-engine.md §8.1).
/// - 16-bit, no dither: S1000/S1100 ingest (dsp-engine.md §8.2).
/// - 16-bit + TPDF dither: S1100 output stage modelling the 20-bit DAC
///   (dsp-engine.md §8.2). The dither rng is an in-repo deterministic
///   generator with an explicit caller-supplied seed so dithered renders are
///   bit-reproducible (testing-strategy.md §3 item 6).
class Quantizer
{
public:
    /// `bits` in [2, 24]; 12 and 16 are the values used by the v1 chains.
    explicit Quantizer(int bits) noexcept;

    [[nodiscard]] int bits() const noexcept { return bits_; }

    /// Quantization step: 2.0 / 2^bits, an exact power of two in float32
    /// (1/2048 full scale at 12-bit, 1/32768 at 16-bit).
    [[nodiscard]] float step() const noexcept { return step_; }

    /// Mid-tread quantize one sample, clamped to the code range. No dither.
    [[nodiscard]] float quantizeSample(float sample) const noexcept;

    /// Quantize a channel in place, no dither. Idempotent.
    void process(AudioView view) const noexcept;

    /// Quantize a channel in place with TPDF dither (±1 LSB triangular,
    /// added before the quantizer). Deterministic: the same `seed` over the
    /// same input yields bit-identical output.
    void process(AudioView view, std::uint64_t seed) const noexcept;

private:
    int bits_ = 16;
    float codesPerUnit_ = 32768.0f; ///< 2^(bits-1) codes per unit amplitude.
    float step_ = 1.0f / 32768.0f;
    std::int32_t minCode_ = -32768;
    std::int32_t maxCode_ = 32767;
};

} // namespace mws::core
