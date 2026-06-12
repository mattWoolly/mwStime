// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// S900/S950 variable-clock 12-bit character chain (docs/design/dsp-engine.md
// §8.1; deep-research-report.md Finding 4). See VarClockChain.h for the
// stage-by-stage contract. NO saturation stages (DRR refuted #4).

#include "mws/model/VarClockChain.h"

#include "mws/core/Butterworth.h"
#include "mws/core/Quantizer.h"
#include "mws/core/Resampler.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace mws::model {

core::AudioBuffer VarClockChain::ingest(core::ConstAudioView in, double inRate,
                                        double bandwidthKHz)
{
    assert(inRate > 0.0);
    assert(bandwidthKHz > 0.0);

    const double fs = modelRateFor(bandwidthKHz);

    // Sinc resample to the model rate (anti-aliased when downsampling —
    // the hardware's input filter tracked the chosen bandwidth [MAN §2]).
    core::AudioBuffer ingested = core::SincResampler::resample(in, fs / inRate);

    // 12-bit mid-tread quantize, no dither (PI). Values only — storage stays
    // float32 so stretch arithmetic keeps 16-bit-capable precision
    // ("12-bit sampling/16-bit processing" [MAN §2 p.74]).
    const core::Quantizer quantizer(12);
    quantizer.process(ingested.channel(0));

    ingested.sampleRate = fs;
    return ingested;
}

VarClockChain::OversampledZoh VarClockChain::renderZoh(core::ConstAudioView stretched,
                                                       double fs, double clockRatio,
                                                       double hostRate)
{
    assert(fs > 0.0);
    assert(clockRatio > 0.0);
    assert(hostRate > 0.0);

    OversampledZoh result;
    result.clockHz = fs * clockRatio;

    // Internal oversampled rate: 4x host default (PI), raised to 2x clock
    // when the virtual clock is the larger figure — always
    // >= 2 x max(clock, hostRate) so the first ZOH image bands are
    // represented before filtering (dsp-engine.md §8.1 IMPLEMENTATION NOTE).
    result.rate = std::max(4.0 * hostRate, 2.0 * result.clockHz);

    if (stretched.empty())
    {
        result.audio.resize(1, 0);
        result.audio.sampleRate = result.rate;
        return result;
    }

    // Output length: the stretched material lasts size()/clock seconds at
    // the virtual clock; render that span at the oversampled rate.
    const double frames = std::ceil(static_cast<double>(stretched.size())
                                    * result.rate / result.clockHz);
    const auto numFrames = static_cast<std::size_t>(frames);

    result.audio.resize(1, numFrames);
    result.audio.sampleRate = result.rate;

    // Zero-order hold at the virtual clock, NO interpolation [DRR F4]: each
    // oversampled output sample holds the most recent virtual-DAC value.
    // The source index is derived from the output index by one multiply
    // (double precision) — no drifting accumulator (architecture.md §7).
    const double step = result.clockHz / result.rate;
    const std::size_t lastIndex = stretched.size() - 1;
    core::AudioView out = result.audio.channel(0);
    for (std::size_t n = 0; n < numFrames; ++n)
    {
        const auto sourceIndex =
            static_cast<std::size_t>(static_cast<double>(n) * step);
        out[n] = stretched[std::min(sourceIndex, lastIndex)];
    }

    return result;
}

core::AudioBuffer VarClockChain::playback(core::ConstAudioView stretched, double fs,
                                          double clockRatio, double hostRate)
{
    // [1] ZOH at the virtual clock, oversampled (images represented).
    OversampledZoh zoh = renderZoh(stretched, fs, clockRatio, hostRate);

    // [2] Reconstruction: 6th-order Butterworth at cutoff = clock / 2.5,
    // TRACKING the clock [DRR F4: BA9221 + MF6CN-50, clock-controlled].
    // Run at the oversampled rate, before decimation, so the image bands it
    // must remove actually exist in the representation.
    core::Butterworth6LP reconstruction;
    reconstruction.setCutoff(zoh.clockHz / 2.5, zoh.rate);
    reconstruction.process(zoh.audio.channel(0));

    // [3] Sinc-decimate to host rate (anti-aliased; dsp-engine.md §8.1).
    core::AudioBuffer out =
        core::SincResampler::resample(zoh.audio.channel(0), hostRate / zoh.rate);
    out.sampleRate = hostRate;
    return out;
}

} // namespace mws::model
