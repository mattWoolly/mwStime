// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"

namespace mws::model {

/// Unified per-model character facade (docs/design/architecture.md §2.1,
/// §5.1 stages [1] and [4]; docs/design/dsp-engine.md §8; task 019).
///
/// THE one switch point for per-model character dispatch: routes S900/S950
/// through the VarClockChain (§8.1) and S1000/S1100 through the
/// FixedRateChain (§8.2, with the S1100 dither delta), selected via the
/// ModelSpec chain table (models are data, not code). No other code may
/// branch on the chain kind.
///
/// Three global rules live here and nowhere else:
///   - CHARACTER bypass (§8.4): `character == OFF` makes ingest() and
///     playback() exact identities (no resample, no quantize, no mono sum)
///     and modelRateFor() return the host rate — the engine runs at host
///     rate on unquantized audio (basis for the engine-only null tests).
///   - S900/S950 mono sum (dsp-engine.md §5): multi-channel input is summed
///     to mono FIRST (the S950 is a mono machine [MAN §2]; authentic), and
///     the IngestResult::monoSummed flag tells the LCD to state it.
///   - S3000 is reserved (ADR-004): every entry point fails loudly on it
///     (debug assert; empty buffer / 0.0 rate in release) — it must never
///     silently alias another model.
class CharacterChain
{
public:
    /// Ingest output: the audio the stretch engine sees, plus the LCD flag.
    struct IngestResult
    {
        /// At model rate, on the model bit lattice (or the untouched input
        /// when character is OFF). `sampleRate` is set accordingly.
        core::AudioBuffer audio;

        /// True iff a multi-channel input was summed to mono by the
        /// S900/S950 rule — surfaced on the LCD (dsp-engine.md §5).
        bool monoSummed = false;
    };

    /// Stage [1] (architecture.md §5.1): per-model ingest emulation, BEFORE
    /// the stretch engine (the hardware stretched already-quantized,
    /// already-band-limited sample RAM [DRR F8]). The input rate is
    /// `in.sampleRate` (must be > 0 unless the buffer is empty).
    ///   - S900/S950: mono-sum first, then VarClockChain::ingest at
    ///     f_s = 2.5 x bandwidth (bandwidth clamped per model spec).
    ///   - S1000/S1100: FixedRateChain::ingest per channel at the
    ///     sampleRateSel rate; channel count preserved.
    ///   - character OFF: identity (bit-exact copy, monoSummed = false).
    [[nodiscard]] static IngestResult
    ingest(const core::AudioBuffer& in, ModelId model,
           const engine::ParamSnapshot& params);

    /// Stage [4] (architecture.md §5.1): per-model playback emulation, AFTER
    /// stretch + transpose. `stretched` lives at the model rate;
    /// `clockRatio` is the virtual voice-clock ratio (2^(transpose/12); only
    /// the varclock models consume it — fixed-rate transpose is the separate
    /// task-018 stage). Returns audio at `hostRate`.
    ///   - S900/S950: VarClockChain::playback (ZOH at clock = f_s x
    ///     clockRatio, tracking Butterworth, decimate to host rate).
    ///   - S1000/S1100: FixedRateChain::playback per channel; the S1100 gets
    ///     the seeded-TPDF 16-bit output quantize (deterministic — the seed
    ///     is a fixed internal constant per channel, architecture.md §7).
    ///   - character OFF: identity (bit-exact copy).
    [[nodiscard]] static core::AudioBuffer
    playback(const core::AudioBuffer& stretched, ModelId model,
             const engine::ParamSnapshot& params, double clockRatio,
             double hostRate);

    /// THE single model-rate authority for the renderer and the FX latency
    /// formula (dsp-engine.md §7.4): 2.5 x bandwidth for S900/S950 (clamped
    /// to the model's bandwidth range), the sampleRateSel rate for
    /// S1000/S1100 — and `hostRate` when character is OFF (§8.4: the engine
    /// runs at host rate on unquantized audio). 0.0 for the reserved S3000.
    [[nodiscard]] static double
    modelRateFor(ModelId model, const engine::ParamSnapshot& params,
                 double hostRate) noexcept;
};

} // namespace mws::model
