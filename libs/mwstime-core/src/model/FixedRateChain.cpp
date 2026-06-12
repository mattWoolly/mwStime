// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/model/FixedRateChain.h"

#include "mws/core/MovingLpf.h"
#include "mws/core/Quantizer.h"
#include "mws/core/Resampler.h"

#include <cassert>

namespace mws::model {
namespace {

constexpr int kSampleBits = 16; // S1000/S1100 sample RAM and output word.

} // namespace

core::AudioBuffer FixedRateChain::ingest(core::ConstAudioView input,
                                         double inRate,
                                         double modelRate)
{
    assert(inRate > 0.0);
    assert(modelRate == 44100.0 || modelRate == 22050.0); // §2 sampleRateSel

    // Resample to the model rate (band-limits when downsampling), THEN
    // quantize: the stretch engine must only ever see 16-bit sample RAM
    // (dsp-engine.md §8 intro, DRR F8).
    core::AudioBuffer out =
        core::SincResampler::resample(input, modelRate / inRate);

    const core::Quantizer quantizer(kSampleBits);
    quantizer.process(out.channel(0)); // no dither at ingest (§8.2)

    out.sampleRate = modelRate;
    return out;
}

core::AudioBuffer FixedRateChain::playback(core::ConstAudioView input,
                                           double modelRate,
                                           double hostRate,
                                           bool s1100Dither,
                                           std::uint64_t seed)
{
    assert(modelRate > 0.0);
    assert(hostRate > 0.0);

    // Voice filter (§8.2): present in the chain but FULLY OPEN by default,
    // i.e. transparent, exactly as on hardware. No cutoff is ever set here —
    // exposing a FILTER control is additive later, not v1.
    core::AudioBuffer filtered(1, input.size());
    core::AudioView filteredView = filtered.channel(0);
    for (std::size_t n = 0; n < input.size(); ++n)
        filteredView[n] = input[n];

    core::MovingLpf3 voiceFilter; // default state: fully open
    voiceFilter.process(filteredView);

    // Fixed-rate interpolating playback to the host rate (DRR F5): the §7.2
    // windowed-sinc.
    core::AudioBuffer out =
        core::SincResampler::resample(filtered.channel(0), hostRate / modelRate);

    if (s1100Dither)
    {
        // S1100 output stage: 16-bit quantize with seeded TPDF dither — the
        // 20-bit-DAC noise-floor model (§8.2, PI). Deliberately NO level
        // offset versus the S1000 (§2 outTrim row).
        const core::Quantizer quantizer(kSampleBits);
        quantizer.process(out.channel(0), seed);
    }

    out.sampleRate = hostRate;
    return out;
}

} // namespace mws::model
