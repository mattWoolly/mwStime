// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// OfflineRenderer — the five-stage authentic render pipeline
// (docs/design/architecture.md §5.1; docs/design/dsp-engine.md §1, §5, §6,
// §7.2, §7.3, §8; ADR-006 SAMPLE-mode contract; task
// plan/backlog/020-offline-renderer.md). See OfflineRenderer.h for the
// stage-by-stage contract.

#include "mws/engine/OfflineRenderer.h"

#include "mws/core/Version.h"
#include "mws/engine/Transpose.h"
#include "mws/model/CharacterChain.h"
#include "mws/model/ModelSpec.h"
#include "mws/stretch/RepitchEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace mws::engine {
namespace {

using model::CharacterChain;
using model::ModelId;
using model::ModelSpec;

/// Falls back to 44.1 kHz when the source never had its rate set (callers are
/// expected to set it; the fallback keeps release builds well-defined).
double sourceRateOr44100(const core::AudioBuffer& source) noexcept
{
    return source.sampleRate > 0.0 ? source.sampleRate : 44100.0;
}

/// Predicted ingest length: SincResampler convention, ceil(N x ratio)
/// (core/Resampler.h). Identity when character is OFF (§8.4 bypass).
std::int64_t predictedIngestFrames(std::int64_t sourceFrames, double sourceRate,
                                   double modelRate, bool characterOn) noexcept
{
    if (sourceFrames <= 0)
        return 0;
    if (!characterOn || sourceRate <= 0.0 || modelRate <= 0.0
        || sourceRate == modelRate)
        return sourceFrames;
    return static_cast<std::int64_t>(std::ceil(
        static_cast<double>(sourceFrames) * modelRate / sourceRate));
}

/// Peak |sample| over every channel (the §7.3 normalization reference).
float peakAbs(const core::AudioBuffer& buffer) noexcept
{
    float peak = 0.0f;
    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
        for (const float v : buffer.channel(ch))
            peak = std::max(peak, std::abs(v));
    return peak;
}

/// Reassembles per-channel renders into one buffer. Channel lengths are
/// normally identical (the shared hop schedule depends only on (N, C, T,
/// hopMode), dsp-engine.md §3 stereo rule); the only divergent corner is
/// S950 MON1 with character OFF on differing stereo content, where per-grain
/// content snaps may shift lengths — shorter channels are zero-padded.
core::AudioBuffer assembleChannels(std::vector<core::AudioBuffer>&& channels)
{
    std::size_t maxFrames = 0;
    for (const core::AudioBuffer& chan : channels)
        maxFrames = std::max(maxFrames, chan.numFrames());

    core::AudioBuffer out(channels.size(), maxFrames);
    for (std::size_t ch = 0; ch < channels.size(); ++ch)
    {
        const core::ConstAudioView src = channels[ch].channel(0);
        core::AudioView dst = out.channel(ch);
        for (std::size_t n = 0; n < src.size(); ++n)
            dst[n] = src[n];
    }
    return out;
}

/// The S900 virtual-DAC-clock ratio for a snapshot, reported by the engine
/// itself (RepitchEngine stays the single formula authority,
/// dsp-engine.md §6: clock = f_s x rate x 2^(transpose/12)). The empty view
/// makes this a pure ratio query — no audio is rendered.
double s900ClockRatio(const ParamSnapshot& params)
{
    return stretch::RepitchEngine::render(core::ConstAudioView{},
                                          params.timeFactor, params.transpose)
        .clockRatio;
}

} // namespace

std::int64_t OfflineRenderer::maxOutputFrames(double modelRate) noexcept
{
    if (modelRate <= 0.0)
        return 0;
    return std::llround(kMaxRenderSeconds * modelRate);
}

std::int64_t
OfflineRenderer::predictedOutputFrames(std::int64_t sourceFrames,
                                       double sourceSampleRate,
                                       ParamSnapshot params) const
{
    // Mirror render(): offline IS the SAMPLE-mode path (ADR-006), and
    // ModelSpec::clamp is the single clamping authority.
    params.pluginMode = PluginMode::Sample;
    const ModelSpec& spec = ModelSpec::get(params.model);
    if (!spec.shipping)
        return 0;
    params = spec.clamp(params);

    const double outRate = sourceSampleRate > 0.0 ? sourceSampleRate : 44100.0;
    const double modelRate =
        CharacterChain::modelRateFor(params.model, params, outRate);
    const std::int64_t nModel = predictedIngestFrames(
        sourceFrames, outRate, modelRate, params.character);
    if (nModel <= 0)
        return 0;

    // Post-transpose duration scale at model rate: every model's transpose
    // path shortens/lengthens the model-rate output by 2^(-transpose/12)
    // (sinc resample ratio p for the fixed-rate models, dsp-engine.md §7.2;
    // playback-clock division for the varclock models, §8.1).
    const double pitchScale = std::exp2(-params.transpose / 12.0);

    switch (params.model)
    {
        case ModelId::S900:
            // Varispeed read schedule (dsp-engine.md §6): round(N / ratio);
            // the ratio already folds transpose in (ADR-003 coupling).
            return std::llround(static_cast<double>(nModel)
                                / s900ClockRatio(params));

        case ModelId::S950:
            // CLASSIC/REVISED schedule at the mapped D-TIME cycle length
            // (dsp-engine.md §5). MON1 per-grain snaps can move the achieved
            // figure slightly; the set C is the prediction (header note).
            return static_cast<std::int64_t>(std::ceil(
                static_cast<double>(cyclic_.expectedOutputLength(
                    nModel,
                    stretch::S950Engine::mapDTimeToCycleLen(params.cycleLen),
                    params.timeFactor, params.hopMode))
                * pitchScale));

        case ModelId::S1000:
        case ModelId::S1100:
        case ModelId::S3000:
        default:
        {
            const std::int64_t base = cyclic_.expectedOutputLength(
                nModel, params.cycleLen, params.timeFactor, params.hopMode);
            if (params.transpose == 0.0)
                return base; // exact: the sinc stage is skipped at 0 st
            return static_cast<std::int64_t>(
                std::ceil(static_cast<double>(base) * pitchScale));
        }
    }
}

RenderResult OfflineRenderer::render(const core::AudioBuffer& source,
                                     ParamSnapshot params,
                                     const Callbacks& callbacks) const
{
    RenderResult result;
    result.info.engineVersionHash = core::engineVersionHash();

    // Offline render IS the SAMPLE-mode path (ADR-006): forcing the mode
    // before clamping keeps the FX FREE causality clamp out of the offline
    // pipeline (T < 100% compresses offline, dsp-engine.md §3.4 edge rules).
    params.pluginMode = PluginMode::Sample;

    const ModelSpec& spec = ModelSpec::get(params.model);
    if (!spec.shipping)
    {
        // ADR-004: reserved slot fails loudly, never aliases another model.
        assert(false && "OfflineRenderer: model is reserved at v1 (ADR-004)");
        result.error = RenderError::UnsupportedModel;
        return result;
    }

    // THE single clamping authority (architecture.md §6).
    params = spec.clamp(params);

    const double outRate = sourceRateOr44100(source);
    const double modelRate =
        CharacterChain::modelRateFor(params.model, params, outRate);
    const auto sourceFrames = static_cast<std::int64_t>(source.numFrames());

    // ---- memory cap (architecture.md §5.1): refuse BEFORE allocating ----
    const std::int64_t predicted =
        predictedOutputFrames(sourceFrames, outRate, params);
    if (predicted > maxOutputFrames(modelRate))
    {
        result.error = RenderError::NotEnoughMemory;
        return result;
    }

    const auto emitProgress = [&](float p) {
        if (callbacks.progress)
            callbacks.progress(p);
    };
    const auto abortRequested = [&]() {
        return callbacks.shouldAbort && callbacks.shouldAbort();
    };
    const auto abortedResult = [&]() {
        RenderResult aborted;
        aborted.info.engineVersionHash = result.info.engineVersionHash;
        aborted.info.monoSummed = result.info.monoSummed;
        aborted.error = RenderError::Aborted;
        return aborted;
    };

    emitProgress(0.0f);
    if (abortRequested())
        return abortedResult();

    // ---- [1] ingest character (CharacterChain, dsp-engine.md §8) ----------
    CharacterChain::IngestResult ingested =
        CharacterChain::ingest(source, params.model, params);
    result.info.monoSummed = ingested.monoSummed;

    emitProgress(0.15f);
    if (abortRequested())
        return abortedResult();

    const auto nModel = static_cast<std::int64_t>(ingested.audio.numFrames());
    const std::size_t numChannels = ingested.audio.numChannels();

    if (nModel <= 0 || numChannels == 0)
    {
        // Nothing to stretch: an empty render is a success with zero length.
        result.out = core::AudioBuffer(std::max<std::size_t>(1, numChannels), 0);
        result.out.sampleRate = outRate;
        result.info.outputSampleRate = outRate;
        emitProgress(1.0f);
        return result;
    }

    // ---- [2] per-model stretch engine (dsp-engine.md §3, §5, §6) ----------
    // clockRatio is the virtual voice-clock multiplier handed to playback
    // (only the varclock chain consumes it, dsp-engine.md §8.1).
    core::AudioBuffer stretched;
    double clockRatio = 1.0;
    double achievedPct = 0.0;

    switch (params.model)
    {
        case ModelId::S900:
        {
            const double fullRatio = s900ClockRatio(params);
            if (params.character)
            {
                // The chain's variable-clock ZOH IS the varispeed read
                // (dsp-engine.md §6/§8.1): the engine stage passes the
                // sample RAM through verbatim and reports the clock ratio
                // so the reconstruction filter tracks pitch [DRR F4/F7].
                clockRatio = fullRatio;
                stretched = std::move(ingested.audio);
            }
            else
            {
                // §8.4 bypass: no chain runs, so the engine performs the
                // ZOH varispeed read itself, per channel (transpose folded
                // into the same ratio — ADR-003 coupling).
                std::vector<core::AudioBuffer> channels;
                channels.reserve(numChannels);
                for (std::size_t ch = 0; ch < numChannels; ++ch)
                {
                    channels.push_back(stretch::RepitchEngine::render(
                                           ingested.audio.channel(ch),
                                           params.timeFactor, params.transpose)
                                           .out);
                    if (abortRequested())
                        return abortedResult();
                }
                stretched = assembleChannels(std::move(channels));
            }
            // Varispeed achieved factor (time and pitch coupled, ADR-003).
            achievedPct = 100.0 / fullRatio;
            break;
        }

        case ModelId::S950:
        {
            stretch::S950Params s950Params;
            s950Params.timeFactorPct = params.timeFactor;
            s950Params.dTime = params.cycleLen;
            s950Params.material = params.material;
            s950Params.autoD = false; // AUTO-D is a UI trigger, not snapshot state
            s950Params.hopMode = params.hopMode;
            s950Params.sampleRate = modelRate;

            std::vector<core::AudioBuffer> channels;
            channels.reserve(numChannels);
            for (std::size_t ch = 0; ch < numChannels; ++ch)
            {
                channels.push_back(
                    s950_.render(ingested.audio.channel(ch), s950Params).out);
                if (abortRequested())
                    return abortedResult();
            }
            stretched = assembleChannels(std::move(channels));

            // Transpose modulates the virtual DAC clock (dsp-engine.md
            // §8.1); with character OFF there is no clock — the sinc stage
            // below keeps the parameter functional (documented fallback).
            clockRatio = params.character
                             ? Transpose::clockRatioFor(params.transpose)
                             : 1.0;
            achievedPct = 100.0 * static_cast<double>(stretched.numFrames())
                          / static_cast<double>(nModel);
            break;
        }

        case ModelId::S1000:
        case ModelId::S1100:
        default:
        {
            // Stereo rule (dsp-engine.md §3): the CLASSIC/REVISED schedule
            // depends only on (N, C, T, hopMode) — never on content — so
            // per-channel renders share the identical hop schedule by
            // construction (property-tested in tests/unit/
            // test_cyclic_properties.cpp).
            std::vector<core::AudioBuffer> channels;
            channels.reserve(numChannels);
            for (std::size_t ch = 0; ch < numChannels; ++ch)
            {
                channels.push_back(cyclic_.render(ingested.audio.channel(ch),
                                                  params.cycleLen,
                                                  params.timeFactor,
                                                  params.hopMode));
                if (abortRequested())
                    return abortedResult();
            }
            stretched = assembleChannels(std::move(channels));
            achievedPct = 100.0 * static_cast<double>(stretched.numFrames())
                          / static_cast<double>(nModel);
            break;
        }
    }

    emitProgress(0.6f);
    if (abortRequested())
        return abortedResult();

    // ---- [3] transpose (dsp-engine.md §7.2; routing per task-018 rule) ----
    // Fixed-rate models: anti-aliased windowed sinc. Varclock models:
    // clockRatio (already set above) — except the character-OFF S950
    // fallback, where no clock stage exists. The S900 never reaches the
    // sinc: its transpose is folded into the varispeed ratio either way.
    const bool useSinc =
        (Transpose::usesSincTranspose(params.model)
         || (!params.character && params.model == ModelId::S950));
    if (useSinc && params.transpose != 0.0)
    {
        std::vector<core::AudioBuffer> channels;
        channels.reserve(stretched.numChannels());
        for (std::size_t ch = 0; ch < stretched.numChannels(); ++ch)
        {
            channels.push_back(
                Transpose::transposeSinc(stretched.channel(ch),
                                         params.transpose));
            if (abortRequested())
                return abortedResult();
        }
        stretched = assembleChannels(std::move(channels));
    }

    emitProgress(0.7f);
    if (abortRequested())
        return abortedResult();

    // ---- [4] playback character (CharacterChain, dsp-engine.md §8) --------
    result.out =
        CharacterChain::playback(stretched, params.model, params, clockRatio,
                                 outRate);
    result.out.sampleRate = outRate;

    emitProgress(0.9f);
    if (abortRequested())
        return abortedResult();

    // ---- [5] optional normalize (dsp-engine.md §7.3, default OFF) ---------
    if (params.norm)
    {
        const float sourcePeak = peakAbs(source);
        const float outPeak = peakAbs(result.out);
        if (sourcePeak > 0.0f && outPeak > 0.0f)
        {
            const float gain = sourcePeak / outPeak;
            for (std::size_t ch = 0; ch < result.out.numChannels(); ++ch)
                for (float& v : result.out.channel(ch))
                    v *= gain;
        }
    }

    result.info.outputFrames =
        static_cast<std::int64_t>(result.out.numFrames());
    result.info.outputSampleRate = outRate;
    result.info.achievedTimeFactorPct = achievedPct;

    emitProgress(1.0f);
    return result;
}

} // namespace mws::engine
