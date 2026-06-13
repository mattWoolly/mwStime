// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/engine/Transpose.h"

#include "mws/core/Resampler.h"
#include "mws/model/ModelSpec.h"

#include <cmath>

namespace mws::engine {

core::AudioBuffer Transpose::transposeSinc(core::ConstAudioView in, double semitones)
{
    // Ratio p = 2^(−semitones/12) applied as an output-length resample
    // [AKZ §10]. Pitching up gives p < 1 (downsampling): SincResampler
    // time-stretches its kernel by 1/p so the cutoff lands on the new
    // Nyquist — the anti-alias band-limit (dsp-engine.md §7.2). std::exp2 is
    // exact at integer arguments, so 0 semitones hits the resampler's exact
    // ratio-1.0 identity path.
    const double ratio = std::exp2(-semitones / 12.0);
    return core::SincResampler::resample(in, ratio);
}

double Transpose::clockRatioFor(double semitones) noexcept
{
    // dsp-engine.md §8.1: clock = f_s × 2^(transpose/12). Exact powers of
    // two at octaves (std::exp2 of an integer).
    return std::exp2(semitones / 12.0);
}

bool Transpose::usesSincTranspose(model::ModelId id) noexcept
{
    // Routed off the character-chain table (ModelSpec — models are data, not
    // code): only the varclock chain transposes by clock modulation; the
    // fixed-rate chain (and the reserved S3000 slot, ADR-004) uses the sinc.
    return model::ModelSpec::get(id).chain
           != model::CharacterChainKind::VarClock12Bit;
}

} // namespace mws::engine
