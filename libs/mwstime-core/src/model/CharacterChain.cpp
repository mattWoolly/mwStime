// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CharacterChain — the one per-model character dispatch point
// (docs/design/dsp-engine.md §8; docs/design/architecture.md §5.1 stages
// [1] and [4]; task plan/backlog/019-character-chain-api.md).

#include "mws/model/CharacterChain.h"

#include "mws/model/FixedRateChain.h"
#include "mws/model/ModelSpec.h"
#include "mws/model/VarClockChain.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace mws::model {
namespace {

/// Fixed internal seed for the S1100 output dither (PI): renders must be
/// bit-reproducible with no caller-supplied entropy (architecture.md §7).
/// Per-channel offset decorrelates the L/R dither patterns.
constexpr std::uint64_t kS1100DitherSeed = 0x5113000D17BE5ULL;

/// Bit-exact copy — the §8.4 CHARACTER bypass identity.
core::AudioBuffer copyOf(const core::AudioBuffer& in)
{
    return in; // AudioBuffer copies are bit-exact (plain float32 storage)
}

/// dsp-engine.md §5: S900/S950 are mono machines — sum every channel to one
/// (equal-weight average (PI): preserves level for correlated material and
/// cannot clip full-scale stereo).
core::AudioBuffer monoSum(const core::AudioBuffer& in)
{
    core::AudioBuffer out(1, in.numFrames());
    out.sampleRate = in.sampleRate;
    const auto gain = 1.0f / static_cast<float>(in.numChannels());
    core::AudioView mixed = out.channel(0);
    for (std::size_t ch = 0; ch < in.numChannels(); ++ch)
    {
        const core::ConstAudioView source = in.channel(ch);
        for (std::size_t n = 0; n < in.numFrames(); ++n)
            mixed[n] += gain * source[n];
    }
    return out;
}

} // namespace

CharacterChain::IngestResult
CharacterChain::ingest(const core::AudioBuffer& in, ModelId model,
                       const engine::ParamSnapshot& params)
{
    // §8.4 bypass: identity — no resample, no quantize, no mono sum. The
    // engine runs at host rate on unquantized audio.
    if (!params.character)
        return { copyOf(in), /*monoSummed=*/false };

    const ModelSpec& spec = ModelSpec::get(model);
    const double modelRate = modelRateFor(model, params, /*hostRate=*/0.0);
    assert(in.sampleRate > 0.0 || in.numFrames() == 0);

    switch (spec.chain)
    {
        case CharacterChainKind::VarClock12Bit:
        {
            // Mono sum FIRST (authentic, dsp-engine.md §5), then the §8.1
            // ingest at f_s = 2.5 x bandwidth. modelRate / 2500 is the
            // model-clamped bandwidth in kHz — modelRateFor stays the single
            // rate authority.
            const bool summed = spec.monoSum && in.numChannels() > 1;
            const core::AudioBuffer mono = summed ? monoSum(in) : copyOf(in);
            assert(mono.numChannels() <= 1);
            core::AudioBuffer audio = VarClockChain::ingest(
                mono.numChannels() == 1 ? mono.channel(0)
                                        : core::ConstAudioView(),
                mono.sampleRate > 0.0 ? mono.sampleRate : modelRate,
                modelRate / 2500.0);
            return { std::move(audio), summed };
        }

        case CharacterChainKind::FixedRate16Bit:
        {
            // §8.2 ingest per channel; channel count preserved (the
            // S1000/S1100 are stereo machines — no mono sum).
            const std::size_t numChannels = in.numChannels();
            if (numChannels == 0)
            {
                core::AudioBuffer empty(1, 0);
                empty.sampleRate = modelRate;
                return { std::move(empty), false };
            }
            core::AudioBuffer audio;
            for (std::size_t ch = 0; ch < numChannels; ++ch)
            {
                const core::AudioBuffer one = FixedRateChain::ingest(
                    in.channel(ch),
                    in.sampleRate > 0.0 ? in.sampleRate : modelRate,
                    modelRate);
                if (ch == 0)
                    audio.resize(numChannels, one.numFrames());
                assert(one.numFrames() == audio.numFrames());
                core::AudioView dst = audio.channel(ch);
                const core::ConstAudioView src = one.channel(0);
                for (std::size_t n = 0; n < src.size(); ++n)
                    dst[n] = src[n];
            }
            audio.sampleRate = modelRate;
            return { std::move(audio), false };
        }

        case CharacterChainKind::S3000Reserved:
            break; // fail loudly below
    }

    // S3000 (and any out-of-range id): reserved at v1 (ADR-004) — loud
    // failure, never a silent alias of another model.
    assert(false && "CharacterChain: S3000 chain is reserved at v1 (ADR-004)");
    return { core::AudioBuffer(in.numChannels() == 0 ? 1 : in.numChannels(), 0),
             false };
}

core::AudioBuffer
CharacterChain::playback(const core::AudioBuffer& stretched, ModelId model,
                         const engine::ParamSnapshot& params,
                         double clockRatio, double hostRate)
{
    assert(clockRatio > 0.0);
    assert(hostRate > 0.0);

    // §8.4 bypass: identity — the chain contributes nothing when OFF.
    if (!params.character)
        return copyOf(stretched);

    const ModelSpec& spec = ModelSpec::get(model);
    const double modelRate = modelRateFor(model, params, hostRate);

    switch (spec.chain)
    {
        case CharacterChainKind::VarClock12Bit:
        {
            // §8.1: mono by construction (ingest summed); ZOH at
            // clock = f_s x clockRatio + tracking Butterworth + decimate.
            assert(stretched.numChannels() <= 1);
            return VarClockChain::playback(
                stretched.numChannels() == 1 ? stretched.channel(0)
                                             : core::ConstAudioView(),
                modelRate, clockRatio, hostRate);
        }

        case CharacterChainKind::FixedRate16Bit:
        {
            // §8.2 per channel. clockRatio is NOT consumed here: fixed-rate
            // transpose is the separate windowed-sinc stage (task 018,
            // dsp-engine.md §7.2). S1100 delta: seeded TPDF output dither.
            const bool s1100Dither = (model == ModelId::S1100);
            const std::size_t numChannels = stretched.numChannels();
            if (numChannels == 0)
            {
                core::AudioBuffer empty(1, 0);
                empty.sampleRate = hostRate;
                return empty;
            }
            core::AudioBuffer out;
            for (std::size_t ch = 0; ch < numChannels; ++ch)
            {
                const core::AudioBuffer one = FixedRateChain::playback(
                    stretched.channel(ch), modelRate, hostRate, s1100Dither,
                    kS1100DitherSeed + static_cast<std::uint64_t>(ch));
                if (ch == 0)
                    out.resize(numChannels, one.numFrames());
                assert(one.numFrames() == out.numFrames());
                core::AudioView dst = out.channel(ch);
                const core::ConstAudioView src = one.channel(0);
                for (std::size_t n = 0; n < src.size(); ++n)
                    dst[n] = src[n];
            }
            out.sampleRate = hostRate;
            return out;
        }

        case CharacterChainKind::S3000Reserved:
            break; // fail loudly below
    }

    assert(false && "CharacterChain: S3000 chain is reserved at v1 (ADR-004)");
    core::AudioBuffer empty(
        stretched.numChannels() == 0 ? 1 : stretched.numChannels(), 0);
    empty.sampleRate = hostRate;
    return empty;
}

double CharacterChain::modelRateFor(ModelId model,
                                    const engine::ParamSnapshot& params,
                                    double hostRate) noexcept
{
    // §8.4: with CHARACTER off the engine runs at the host rate.
    if (!params.character)
        return hostRate;

    const ModelSpec& spec = ModelSpec::get(model);
    if (spec.chain == CharacterChainKind::S3000Reserved)
    {
        assert(false
               && "CharacterChain: S3000 has no model rate at v1 (ADR-004)");
        return 0.0;
    }

    // ModelSpec::modelRateHz is the per-model table rule (2.5 x clamped
    // bandwidth for varclock; sampleRateSel for fixed-rate).
    return spec.modelRateHz(params);
}

} // namespace mws::model
