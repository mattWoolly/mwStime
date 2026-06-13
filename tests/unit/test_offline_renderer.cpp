// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// OfflineRenderer tests — written FIRST per plan/backlog/020-offline-renderer.md
// (TDD). Contracts under test:
//   - end-to-end S1000 CLASSIC length is the §3.4 schedule-derived value,
//     recomputed INDEPENDENTLY here (never round(N*T) — "bad timing",
//     docs/design/dsp-engine.md §3.4, testing-strategy.md §3 item 2),
//   - the 10-minute model-rate memory cap refuses with NotEnoughMemory BEFORE
//     allocating (architecture.md §5.1; testing-strategy.md §3 item 10 — the
//     cap math is unit-tested directly via predictedOutputFrames),
//   - abort: shouldAbort() == true after the first progress call => Aborted,
//   - norm ON peak-normalizes to the source peak; OFF leaves gain untouched
//     (dsp-engine.md §7.3),
//   - determinism: same (source, ParamSnapshot) twice => bit-identical
//     (architecture.md §6; incl. the fixed internal S1100 dither seed),
//   - all four shipping models render a noise burst without error.
//
// Test-case names begin with the tag word so `ctest -R renderer` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/Version.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::engine::RenderError;
using mws::engine::RenderResult;
using mws::model::ModelId;

constexpr double kRate = 44100.0;

/// 1-channel sine at `freqHz`, full scale unless `peak` given.
AudioBuffer makeSine(double freqHz, std::size_t numFrames, float peak = 1.0f)
{
    AudioBuffer buffer(1, numFrames);
    buffer.sampleRate = kRate;
    auto view = buffer.channel(0);
    const double w = 2.0 * std::numbers::pi_v<double> * freqHz / kRate;
    for (std::size_t n = 0; n < numFrames; ++n)
        view[n] = peak
                  * static_cast<float>(std::sin(w * static_cast<double>(n)));
    return buffer;
}

/// Deterministic LCG noise, per-channel decorrelated, peak-bounded.
AudioBuffer makeNoise(std::size_t numChannels, std::size_t numFrames,
                      float peak = 0.5f)
{
    AudioBuffer buffer(numChannels, numFrames);
    buffer.sampleRate = kRate;
    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        std::uint32_t state = 0x12345678u + 0x9E3779B9u * static_cast<std::uint32_t>(ch);
        auto view = buffer.channel(ch);
        for (std::size_t n = 0; n < numFrames; ++n)
        {
            state = state * 1664525u + 1013904223u;
            const float unit =
                static_cast<float>(state) / 4294967296.0f * 2.0f - 1.0f;
            view[n] = peak * unit;
        }
    }
    return buffer;
}

float peakAbs(const AudioBuffer& buffer)
{
    float peak = 0.0f;
    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
        for (const float v : buffer.channel(ch))
            peak = std::max(peak, std::abs(v));
    return peak;
}

/// INDEPENDENT recompute of the CLASSIC schedule-derived output length
/// (dsp-engine.md §3.4): ovStart = round(C*(1-F)) with F = 0.20,
/// hop_out = ovStart, hop_in = round(hop_out / T) with integer-% T,
/// G = floor((N-C)/hop_in) + 1, length = (G-1)*hop_out + C.
/// Deliberately NOT derived from CyclicEngine (testing-strategy.md §3 item 2).
std::int64_t classicScheduleLength(std::int64_t n, int c, int tPct)
{
    const std::int64_t ovStart =
        std::llround(static_cast<double>(c) * (1.0 - 0.20));
    const std::int64_t hopOut = ovStart;
    const std::int64_t hopIn = std::llround(
        static_cast<double>(hopOut) * 100.0 / static_cast<double>(tPct));
    const std::int64_t grains = (n - c) / hopIn + 1;
    return (grains - 1) * hopOut + c;
}

} // namespace

// ---------------------------------------------------------------------------
// End-to-end: S1000 CLASSIC C=1000 T=300% on a 1 s sine — output length is
// the schedule-derived value (model rate == source rate == 44.1k, transpose 0,
// so no resampling stage changes the frame count anywhere in the pipeline).
// ---------------------------------------------------------------------------
TEST_CASE("renderer: S1000 CLASSIC end-to-end length is schedule-derived",
          "[renderer]")
{
    const AudioBuffer source = makeSine(220.0, 44100);

    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.cycleLen = 1000;
    params.timeFactor = 300.0;

    const OfflineRenderer renderer;
    const RenderResult result = renderer.render(source, params);

    REQUIRE(result.error == RenderError::None);
    REQUIRE(result.ok());

    const std::int64_t expected = classicScheduleLength(44100, 1000, 300);
    REQUIRE(expected == 129800); // sanity-pin the independent recompute

    REQUIRE(static_cast<std::int64_t>(result.out.numFrames()) == expected);

    // The defining CLASSIC property: NOT round(N*T) ("bad timing").
    REQUIRE(expected != std::llround(44100.0 * 3.0));

    // RenderInfo carries the LCD readout (task 020 acceptance criteria).
    REQUIRE(result.info.outputFrames == expected);
    REQUIRE(result.info.outputSampleRate == kRate);
    const double expectedPct = 100.0 * static_cast<double>(expected) / 44100.0;
    REQUIRE(std::abs(result.info.achievedTimeFactorPct - expectedPct) < 1e-9);
    REQUIRE(result.info.engineVersionHash == mws::core::kEngineVersionHash);
    REQUIRE_FALSE(result.info.monoSummed);
    REQUIRE(result.out.sampleRate == kRate);
}

// ---------------------------------------------------------------------------
// ADR-006 SAMPLE-mode forcing: offline IS the SAMPLE-mode path, so compression
// (T < 100%) renders even when the caller's snapshot still says FX FREE — the
// one combination whose clamp floors timeFactor at 100% (ModelSpec::clamp,
// ADR-006 causality rule). Pins dsp-engine.md §3.4 "T < 1 (compression) works
// identically offline" and the `params.pluginMode = PluginMode::Sample` lines
// in OfflineRenderer::render/predictedOutputFrames: remove them and the FX
// FREE clamp turns T=50 into T=100, inflating the length to ~N.
// ---------------------------------------------------------------------------
TEST_CASE("renderer: offline compression forces SAMPLE mode past the FX FREE "
          "clamp",
          "[renderer]")
{
    const AudioBuffer source = makeSine(220.0, 44100);

    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.cycleLen = 1000;
    params.timeFactor = 50.0;
    params.fxWindow = mws::engine::FxWindow::Free;
    // Deliberately NOT set: pluginMode stays at its FX default — the renderer
    // itself must force SAMPLE mode (ADR-006) before clamping.
    REQUIRE(params.pluginMode == mws::engine::PluginMode::Fx);

    const OfflineRenderer renderer;
    const RenderResult result = renderer.render(source, params);

    REQUIRE(result.error == RenderError::None);
    REQUIRE(result.ok());

    // Independent §3.4 recompute at T=50: hop_out=800, hop_in=1600,
    // G=27 => 26*800+1000 — genuinely compressed, well under N.
    const std::int64_t expected = classicScheduleLength(44100, 1000, 50);
    REQUIRE(expected == 21800); // sanity-pin the independent recompute
    REQUIRE(expected < 44100);

    REQUIRE(static_cast<std::int64_t>(result.out.numFrames()) == expected);
    REQUIRE(result.info.outputFrames == expected);
    REQUIRE(result.info.achievedTimeFactorPct < 100.0);

    // The cap predictor mirrors render(): same SAMPLE-mode forcing, same
    // compressed length.
    REQUIRE(renderer.predictedOutputFrames(44100, kRate, params) == expected);
}

// ---------------------------------------------------------------------------
// Memory cap (architecture.md §5.1; testing-strategy.md §3 item 10): the
// schedule-predicted output length is checked against the 10-min model-rate
// cap BEFORE any allocation; the cap math is unit-tested directly.
// ---------------------------------------------------------------------------
TEST_CASE("renderer: cap math is unit-tested directly", "[renderer]")
{
    const OfflineRenderer renderer;

    // 10 minutes at 44.1 kHz, per channel.
    REQUIRE(OfflineRenderer::maxOutputFrames(44100.0) == 26'460'000);

    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.cycleLen = 1000;
    params.timeFactor = 2000.0;

    // 35 s of input at 2000%: predicted length comes from the schedule, and
    // exceeds the cap. Pure math — no audio buffer exists here at all.
    const std::int64_t srcFrames = 44100 * 35;
    const std::int64_t predicted =
        renderer.predictedOutputFrames(srcFrames, kRate, params);
    REQUIRE(predicted == classicScheduleLength(srcFrames, 1000, 2000));
    REQUIRE(predicted == 30'850'600);
    REQUIRE(predicted > OfflineRenderer::maxOutputFrames(44100.0));

    // The e2e case from above stays under the cap.
    params.timeFactor = 300.0;
    REQUIRE(renderer.predictedOutputFrames(44100, kRate, params)
            == classicScheduleLength(44100, 1000, 300));
}

TEST_CASE("renderer: over-cap render is refused with NotEnoughMemory",
          "[renderer]")
{
    // Synthetic long input (35 s of silence is enough — the refusal happens
    // before any stage runs, so content is irrelevant).
    AudioBuffer source(1, 44100u * 35u);
    source.sampleRate = kRate;

    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.cycleLen = 1000;
    params.timeFactor = 2000.0;

    bool progressSeen = false;
    OfflineRenderer::Callbacks callbacks;
    callbacks.progress = [&](float) { progressSeen = true; };

    const OfflineRenderer renderer;
    const RenderResult result = renderer.render(source, params, callbacks);

    REQUIRE(result.error == RenderError::NotEnoughMemory);
    REQUIRE_FALSE(result.ok());
    // No output allocation happened: the refusal precedes every pipeline stage.
    REQUIRE(result.out.numFrames() == 0);
    REQUIRE_FALSE(progressSeen);
}

// ---------------------------------------------------------------------------
// Abort: shouldAbort() polled at stage boundaries; true after the first
// progress call => RenderError::Aborted, no crash, no output.
// ---------------------------------------------------------------------------
TEST_CASE("renderer: abort after the first progress call returns Aborted",
          "[renderer]")
{
    const AudioBuffer source = makeSine(220.0, 44100);

    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.timeFactor = 300.0;

    bool abortFlag = false;
    int progressCalls = 0;
    OfflineRenderer::Callbacks callbacks;
    callbacks.progress = [&](float) {
        ++progressCalls;
        abortFlag = true; // raised by the first progress callback
    };
    callbacks.shouldAbort = [&]() { return abortFlag; };

    const OfflineRenderer renderer;
    const RenderResult result = renderer.render(source, params, callbacks);

    REQUIRE(result.error == RenderError::Aborted);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.out.numFrames() == 0);
    REQUIRE(progressCalls >= 1);
}

// ---------------------------------------------------------------------------
// Normalization (dsp-engine.md §7.3): ON peak-normalizes the output to the
// SOURCE peak; OFF (the authentic default) leaves the rendered gain untouched
// — ON differs from OFF by exactly one constant gain factor.
// ---------------------------------------------------------------------------
TEST_CASE("renderer: norm ON restores the source peak, OFF is untouched gain",
          "[renderer]")
{
    AudioBuffer source = makeNoise(1, 22050, 0.2f);
    source.channel(0)[0] = 0.25f; // exact, known source peak
    REQUIRE(peakAbs(source) == 0.25f);

    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.cycleLen = 500;
    params.timeFactor = 250.0;
    REQUIRE_FALSE(params.norm); // OFF is the authentic default

    const OfflineRenderer renderer;
    const RenderResult off = renderer.render(source, params);
    REQUIRE(off.error == RenderError::None);

    params.norm = true;
    const RenderResult on = renderer.render(source, params);
    REQUIRE(on.error == RenderError::None);

    // ON: output peak == source peak (+-1e-6).
    REQUIRE(std::abs(peakAbs(on.out) - 0.25f) < 1e-6f);

    // OFF -> ON is exactly one constant gain factor over the identical
    // pre-norm render (determinism + "norm only scales").
    REQUIRE(on.out.numFrames() == off.out.numFrames());
    const float gain = 0.25f / peakAbs(off.out);
    const ConstAudioView offView = off.out.channel(0);
    const ConstAudioView onView = on.out.channel(0);
    for (std::size_t n = 0; n < offView.size(); ++n)
        REQUIRE(std::abs(onView[n] - offView[n] * gain) <= 1e-6f);
}

// ---------------------------------------------------------------------------
// Determinism (architecture.md §6): same (source, ParamSnapshot) twice =>
// bit-identical output, including the S1100 seeded-dither path (the seed is a
// fixed internal constant, never caller entropy).
// ---------------------------------------------------------------------------
TEST_CASE("renderer: deterministic — identical inputs render bit-identical",
          "[renderer]")
{
    const AudioBuffer source = makeNoise(2, 22050);

    ParamSnapshot params;
    params.model = ModelId::S1100; // exercises the seeded TPDF dither delta
    params.cycleLen = 800;
    params.timeFactor = 250.0;
    params.transpose = 3.0; // exercises the sinc transpose stage too

    const OfflineRenderer renderer;
    const RenderResult first = renderer.render(source, params);
    const RenderResult second = renderer.render(source, params);

    REQUIRE(first.error == RenderError::None);
    REQUIRE(second.error == RenderError::None);
    REQUIRE(first.out.numChannels() == second.out.numChannels());
    REQUIRE(first.out.numFrames() == second.out.numFrames());
    for (std::size_t ch = 0; ch < first.out.numChannels(); ++ch)
    {
        const ConstAudioView a = first.out.channel(ch);
        const ConstAudioView b = second.out.channel(ch);
        for (std::size_t n = 0; n < a.size(); ++n)
        {
            if (a[n] != b[n]) // keep the assertion count manageable
                REQUIRE(a[n] == b[n]);
        }
    }
    REQUIRE(first.info.outputFrames == second.info.outputFrames);
    REQUIRE(first.info.achievedTimeFactorPct
            == second.info.achievedTimeFactorPct);
}

// ---------------------------------------------------------------------------
// Every shipping model renders a 0.5 s noise burst without error; the
// S900/S950 mono-sum rule (dsp-engine.md §5) surfaces in RenderInfo.
// ---------------------------------------------------------------------------
TEST_CASE("renderer: all four shipping models render a noise burst",
          "[renderer]")
{
    const AudioBuffer source = makeNoise(2, 22050);
    const OfflineRenderer renderer;

    constexpr std::array<ModelId, 4> kShipping{
        ModelId::S900, ModelId::S950, ModelId::S1000, ModelId::S1100,
    };

    for (const ModelId model : kShipping)
    {
        ParamSnapshot params;
        params.model = model;
        params.timeFactor = 150.0;

        const RenderResult result = renderer.render(source, params);

        INFO("model " << mws::model::toString(model));
        REQUIRE(result.error == RenderError::None);
        REQUIRE(result.out.numFrames() > 0);
        REQUIRE(result.out.sampleRate == kRate);
        REQUIRE(result.info.outputFrames
                == static_cast<std::int64_t>(result.out.numFrames()));
        REQUIRE(result.info.outputSampleRate == kRate);
        REQUIRE(result.info.achievedTimeFactorPct > 0.0);
        REQUIRE(result.info.engineVersionHash == mws::core::kEngineVersionHash);

        const bool varClock = (model == ModelId::S900 || model == ModelId::S950);
        REQUIRE(result.info.monoSummed == varClock); // stereo in, mono machines
        REQUIRE(result.out.numChannels() == (varClock ? 1u : 2u));

        // Progress reaches completion on a successful render.
        // (Re-render with callbacks to keep the assertion local.)
        float lastProgress = -1.0f;
        bool monotone = true;
        OfflineRenderer::Callbacks callbacks;
        callbacks.progress = [&](float p) {
            monotone = monotone && p >= lastProgress;
            lastProgress = p;
        };
        const RenderResult again = renderer.render(source, params, callbacks);
        REQUIRE(again.error == RenderError::None);
        REQUIRE(monotone);
        REQUIRE(lastProgress == 1.0f);
    }
}
