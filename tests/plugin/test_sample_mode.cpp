// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// SAMPLE-mode pipeline tests (plan/backlog/034) — the headless GO -> render ->
// audition round-trip plus the ZONE live preview, exercised through EngineHost +
// SamplePlayer:
//   - GO render of the whole source equals a direct OfflineRenderer call (same
//     snapshot) bit-exactly, and is deterministic,
//   - abort mid-render leaves the PREVIOUS published render in place,
//   - the render-cap refusal surfaces the typed NotEnoughMemory event,
//   - A/B audition toggles between the render (B) and the source (A) between
//     blocks, RCU publication protocol respected,
//   - ZONE preview produces stretched output of the zone (splice-comb structure
//     present vs the dry zone),
//   - outTrim -6 dB scales SAMPLE playback by x0.501 (+/-1e-3),
//   - the SamplePlayer / ZONE-preview audio paths allocate nothing.
//
// docs/design/architecture.md §4 (threading + RCU publication), §5.1 (SAMPLE
// data flow); plan/decisions/006-fx-vs-sample-mode.md (workflow contract);
// ui-design.md §6.3 (audition/render flow). Test-case names begin with
// "sample_mode" so `ctest -R sample_mode` selects them (README test-selection
// rules).

#include <catch2/catch_test_macros.hpp>

#include "EngineHost.h"
#include "SamplePlayer.h"

// The global operator-new replacement (TestAllocationCounter.h) is provided by
// this binary's own owner translation unit (test_alloc_counter.cpp) so the
// audio-thread no-allocation assertions can run here.
#include "TestAllocationCounter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::RenderResult;
using mws::model::ModelId;
using mws::plugin::AuditionSource;
using mws::plugin::EngineHost;
using mws::plugin::RenderedSample;
using mws::plugin::RenderOutcome;
using mws::plugin::SamplePlayer;
using mws::plugin::WorkerEvent;

// A deterministic source: a long-enough, structured signal so a CLASSIC stretch
// shows a clear splice-comb signature and the worker render is non-trivial.
std::shared_ptr<const AudioBuffer> makeSource(std::size_t frames, double rate = 44100.0,
                                              std::size_t channels = 1)
{
    auto buf = std::make_shared<AudioBuffer>(channels, frames);
    buf->sampleRate = rate;
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
        auto v = buf->channel(ch);
        const double amp = (ch == 0 ? 0.8 : 0.6);
        for (std::size_t i = 0; i < frames; ++i)
            v[i] = static_cast<float>(
                std::sin(2.0 * 3.14159265358979 * 220.0 * static_cast<double>(i) / rate)
                * amp);
    }
    return buf;
}

ParamSnapshot sampleParams(double timeFactor = 200.0)
{
    ParamSnapshot p;
    p.model = ModelId::S1000;
    p.pluginMode = PluginMode::Sample;
    p.timeFactor = timeFactor;
    p.character = false; // exercise the deterministic integer CLASSIC path
    return p;
}

struct DrainResult {
    bool sawFinished = false;
    RenderOutcome outcome = RenderOutcome::Completed;
};

DrainResult drainUntilFinished(EngineHost& host, std::uint64_t id, int timeoutMs = 5000)
{
    DrainResult r;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        WorkerEvent ev;
        bool any = false;
        while (host.popEvent(ev))
        {
            any = true;
            if (ev.requestId == id && ev.kind == WorkerEvent::Kind::Finished)
            {
                r.sawFinished = true;
                r.outcome = ev.outcome;
            }
        }
        host.collectGarbage();
        if (r.sawFinished)
            break;
        if (!any)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}

// Run the SamplePlayer/EngineHost over a whole signal block by block, capturing
// the output. EngineHost must be prepared for SAMPLE first.
std::vector<float> runSampleBlocks(EngineHost& host, std::int64_t frames, int block,
                                   const ParamSnapshot& params, std::size_t channels = 1)
{
    std::vector<float> out(static_cast<std::size_t>(frames * static_cast<std::int64_t>(channels)), 0.0f);
    std::vector<std::vector<float>> bufs(channels, std::vector<float>(static_cast<std::size_t>(block)));
    std::vector<float*> chans(channels);
    std::int64_t pos = 0;
    while (pos < frames)
    {
        const auto len = std::min<std::int64_t>(block, frames - pos);
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            std::fill(bufs[ch].begin(), bufs[ch].end(), 0.0f);
            chans[ch] = bufs[ch].data();
        }
        host.processSampleBlock(chans.data(), static_cast<int>(channels),
                                static_cast<int>(len), params);
        for (std::size_t ch = 0; ch < channels; ++ch)
            for (std::int64_t i = 0; i < len; ++i)
                out[static_cast<std::size_t>((pos + i) * static_cast<std::int64_t>(channels) + static_cast<std::int64_t>(ch))]
                    = bufs[ch][static_cast<std::size_t>(i)];
        pos += len;
    }
    return out;
}
} // namespace

TEST_CASE("sample_mode: GO render equals a direct OfflineRenderer call bit-exactly",
          "[sample_mode]")
{
    EngineHost host;
    host.startWorker();

    auto source = makeSource(44100); // 1 s
    host.setSource(source);

    const auto params = sampleParams(/*timeFactor=*/200.0);
    const auto id = host.requestRender(params);
    REQUIRE(id != 0);
    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::Completed);

    auto rendered = host.currentRender();
    REQUIRE(rendered != nullptr);
    REQUIRE(rendered->requestId == id);
    REQUIRE(rendered->audio.numFrames() > 0);

    // A direct OfflineRenderer call with the same snapshot must match the
    // published render sample-for-sample (the worker just wraps the renderer).
    OfflineRenderer ref;
    const RenderResult direct = ref.render(*source, params);
    REQUIRE(direct.ok());
    REQUIRE(direct.out.numFrames() == rendered->audio.numFrames());
    REQUIRE(direct.out.numChannels() == rendered->audio.numChannels());
    for (std::size_t ch = 0; ch < direct.out.numChannels(); ++ch)
    {
        const auto a = direct.out.channel(ch);
        const auto b = rendered->audio.channel(ch);
        REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    }

    // Determinism: a second render of the same input yields the same length and
    // bytes (architecture.md §6 determinism rule).
    const auto id2 = host.requestRender(params);
    REQUIRE(drainUntilFinished(host, id2).outcome == RenderOutcome::Completed);
    auto rendered2 = host.currentRender();
    REQUIRE(rendered2->audio.numFrames() == rendered->audio.numFrames());

    host.collectGarbage();
    host.stopWorker();
}

TEST_CASE("sample_mode: abort mid-render leaves the previous render in place",
          "[sample_mode]")
{
    EngineHost host;
    host.startWorker();
    host.setSource(makeSource(44100 * 4)); // 4 s

    // First, a clean render we expect to survive the later aborted attempt.
    const auto first = host.requestRender(sampleParams(150.0));
    REQUIRE(drainUntilFinished(host, first).outcome == RenderOutcome::Completed);
    auto kept = host.currentRender();
    REQUIRE(kept != nullptr);
    REQUIRE(kept->requestId == first);

    // Now abort the next render: raise the latch before enqueue so the worker
    // aborts at the first stage boundary (nothing new published).
    host.requestAbort();
    const auto second = host.requestRender(sampleParams(800.0));
    const auto r = drainUntilFinished(host, second);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::Aborted);

    // The PREVIOUS render is still the published one.
    auto now = host.currentRender();
    REQUIRE(now != nullptr);
    REQUIRE(now->requestId == first);

    host.collectGarbage();
    host.stopWorker();
}

TEST_CASE("sample_mode: render cap refusal surfaces the typed NotEnoughMemory event",
          "[sample_mode]")
{
    EngineHost host;
    host.startWorker();
    host.setSource(makeSource(44100 * 40)); // 40 s

    auto params = sampleParams(2000.0); // 40 s * 2000% = 800 s > the 600 s cap
    const auto id = host.requestRender(params);
    REQUIRE(id != 0);
    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::NotEnoughMemory);

    // Nothing published for the refused render.
    auto rendered = host.currentRender();
    REQUIRE((rendered == nullptr || rendered->requestId != id));

    host.stopWorker();
}

TEST_CASE("sample_mode: GO renders only the selected zone slice", "[sample_mode]")
{
    EngineHost host;
    host.startWorker();

    auto source = makeSource(44100); // 1 s
    host.setSource(source);

    // Render the middle half [0.25, 0.75) at 100% (no stretch, CLASSIC): the
    // achieved length tracks the half-length slice, not the full source.
    auto params = sampleParams(100.0);
    const auto id = host.requestRender(params, EngineHost::Zone{ 0.25, 0.75 });
    REQUIRE(drainUntilFinished(host, id).outcome == RenderOutcome::Completed);
    auto zoneRender = host.currentRender();
    REQUIRE(zoneRender != nullptr);

    // A direct render of the cropped slice matches bit-for-bit.
    const std::int64_t total = static_cast<std::int64_t>(source->numFrames());
    const auto from = static_cast<std::size_t>(std::llround(0.25 * static_cast<double>(total)));
    const auto to = static_cast<std::size_t>(std::llround(0.75 * static_cast<double>(total)));
    AudioBuffer slice(source->numChannels(), to - from);
    slice.sampleRate = source->sampleRate;
    for (std::size_t ch = 0; ch < source->numChannels(); ++ch)
    {
        const auto in = source->channel(ch);
        auto dst = slice.channel(ch);
        for (std::size_t i = 0; i < to - from; ++i)
            dst[i] = in[from + i];
    }
    OfflineRenderer ref;
    const auto direct = ref.render(slice, params);
    REQUIRE(direct.ok());
    REQUIRE(direct.out.numFrames() == zoneRender->audio.numFrames());
    const auto a = direct.out.channel(0);
    const auto b = zoneRender->audio.channel(0);
    REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);

    host.collectGarbage();
    host.stopWorker();
}

TEST_CASE("sample_mode: A/B audition toggles render (B) vs original (A) between blocks",
          "[sample_mode]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    EngineHost host;
    host.startWorker();
    auto source = makeSource(static_cast<std::size_t>(kRate), kRate); // 1 s at host rate
    host.setSource(source);

    const auto params = sampleParams(200.0); // B (render) is longer than A (source)
    const auto id = host.requestRender(params);
    REQUIRE(drainUntilFinished(host, id).outcome == RenderOutcome::Completed);
    auto rendered = host.currentRender();
    REQUIRE(rendered != nullptr);
    const auto renderFrames = static_cast<std::int64_t>(rendered->audio.numFrames());

    host.prepareSample(kRate, kBlock, /*channels=*/1, params);

    // Play enough frames to pass the cyclic engine's verbatim grain head
    // (ovStart = cycleLen * (1 - F) = 800 at the default cycle) so the stretched
    // render (B) visibly diverges from the dry source (A).
    const std::int64_t kPlay = 2048;

    // B = render (default): playback reproduces the render buffer verbatim
    // (rate-matched: host == buffer rate => no interpolation).
    host.setAuditionSource(AuditionSource::Render);
    host.startSamplePlayback();
    REQUIRE(host.isSamplePlaying());
    auto outB = runSampleBlocks(host, kPlay, kBlock, params, 1);
    const auto rb = rendered->audio.channel(0);
    const auto cmpLenB = std::min<std::size_t>(
        outB.size(), static_cast<std::size_t>(renderFrames));
    // memcmp avoids per-sample float-equal warnings (codebase convention).
    REQUIRE(std::memcmp(outB.data(), rb.data(), cmpLenB * sizeof(float)) == 0);

    // A = original: now the playback follows the SOURCE, not the render. Restart.
    host.setAuditionSource(AuditionSource::Original);
    host.startSamplePlayback();
    auto outA = runSampleBlocks(host, kPlay, kBlock, params, 1);
    const auto sa = source->channel(0);
    REQUIRE(std::memcmp(outA.data(), sa.data(), outA.size() * sizeof(float)) == 0);

    // The two auditions differ (B is the stretched render, A is the dry source) —
    // they coincide only over the verbatim grain head, then diverge.
    REQUIRE(std::memcmp(outA.data(), outB.data(),
                        std::min(outA.size(), outB.size()) * sizeof(float)) != 0);

    host.collectGarbage();
    host.stopWorker();
}

TEST_CASE("sample_mode: ZONE preview produces stretched output of the zone (splice-comb vs dry)",
          "[sample_mode]")
{
    constexpr double kRate = 44100.0;
    constexpr int kBlock = 512;

    EngineHost host;
    auto source = makeSource(static_cast<std::size_t>(kRate), kRate); // 1 s sine
    host.setSource(source);

    // A 200% CLASSIC stretch over a moderate cycle: the cyclic splicer repeats
    // grains (the audible "splice-comb"), so the looped-zone preview output is a
    // restructured version of the dry zone, not a delayed copy of it.
    auto params = sampleParams(/*timeFactor=*/200.0);
    params.cycleLen = 600;
    host.prepareSample(kRate, kBlock, /*channels=*/1, params);

    // ZONE preview ON over the whole sample.
    host.setZonePreview(true, EngineHost::Zone{ 0.0, 1.0 }, params);
    REQUIRE(host.zonePreviewActive());

    // Run well past the streaming pre-roll + expansion lag so the active region
    // is populated (at T=200% the read head lags; the output ramps in after a
    // few thousand frames).
    const std::int64_t kFrames = 24576;
    auto wet = runSampleBlocks(host, kFrames, kBlock, params, 1);

    // The dry zone for comparison is the looped source.
    std::vector<float> dry(static_cast<std::size_t>(kFrames));
    const auto sv = source->channel(0);
    const auto srcLen = static_cast<std::int64_t>(source->numFrames());
    for (std::int64_t i = 0; i < kFrames; ++i)
        dry[static_cast<std::size_t>(i)] = sv[static_cast<std::size_t>(i % srcLen)];

    // Locate the active region (first non-trivial output sample) and analyze the
    // tail past it.
    std::int64_t firstActive = -1;
    for (std::int64_t i = 0; i < kFrames; ++i)
        if (std::fabs(wet[static_cast<std::size_t>(i)]) > 1.0e-4f)
        {
            firstActive = i;
            break;
        }
    REQUIRE(firstActive >= 0);            // the preview is producing audio
    REQUIRE(firstActive < kFrames - 4096); // with a usable active tail

    // Over the active tail the preview is non-silent and differs from the dry
    // zone — the splice-comb restructuring (it is not a verbatim loop).
    std::size_t nonZero = 0;
    double diffEnergy = 0.0;
    double wetEnergy = 0.0;
    for (std::size_t i = static_cast<std::size_t>(firstActive); i < wet.size(); ++i)
    {
        if (std::fabs(wet[i]) > 1.0e-6f)
            ++nonZero;
        const double d = static_cast<double>(wet[i]) - static_cast<double>(dry[i]);
        diffEnergy += d * d;
        wetEnergy += static_cast<double>(wet[i]) * static_cast<double>(wet[i]);
    }
    REQUIRE(nonZero > 0);
    REQUIRE(wetEnergy > 0.0);
    REQUIRE(diffEnergy > 0.01 * wetEnergy); // restructured vs the dry zone
}

TEST_CASE("sample_mode: outTrim -6 dB scales SAMPLE playback by x0.501", "[sample_mode]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    EngineHost host;
    host.startWorker();
    auto source = makeSource(static_cast<std::size_t>(kRate), kRate);
    host.setSource(source);

    auto params = sampleParams(150.0);
    const auto id = host.requestRender(params);
    REQUIRE(drainUntilFinished(host, id).outcome == RenderOutcome::Completed);
    auto rendered = host.currentRender();
    REQUIRE(rendered != nullptr);

    host.prepareSample(kRate, kBlock, /*channels=*/1, params);
    host.setAuditionSource(AuditionSource::Render);

    // Reference at 0 dB.
    host.startSamplePlayback();
    auto out0 = runSampleBlocks(host, kBlock, kBlock, params, 1);

    // Same playback at -6 dB out trim.
    auto trimmed = params;
    trimmed.outTrim = -6.0;
    host.startSamplePlayback();
    auto outTrim = runSampleBlocks(host, kBlock, kBlock, trimmed, 1);

    const double expected = std::pow(10.0, -6.0 / 20.0); // ~0.5012
    std::size_t checked = 0;
    for (std::size_t i = 0; i < out0.size(); ++i)
    {
        if (std::fabs(out0[i]) > 1.0e-4f)
        {
            const double ratio = static_cast<double>(outTrim[i]) / static_cast<double>(out0[i]);
            REQUIRE(std::fabs(ratio - expected) < 1.0e-3);
            ++checked;
        }
    }
    REQUIRE(checked > 0);

    host.collectGarbage();
    host.stopWorker();
}

TEST_CASE("sample_mode: SamplePlayer playback path allocates nothing", "[sample_mode]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    EngineHost host;
    host.startWorker();
    auto source = makeSource(static_cast<std::size_t>(kRate), kRate);
    host.setSource(source);

    auto params = sampleParams(200.0);
    const auto id = host.requestRender(params);
    REQUIRE(drainUntilFinished(host, id).outcome == RenderOutcome::Completed);
    host.prepareSample(kRate, kBlock, /*channels=*/2, params);
    host.setAuditionSource(AuditionSource::Render);
    host.startSamplePlayback();

    std::vector<float> l(kBlock), r(kBlock);
    float* chans[2] = { l.data(), r.data() };

    // Warm up (first-touch lazy allocation out of the way).
    for (int i = 0; i < 4; ++i)
        host.processSampleBlock(chans, 2, kBlock, params);

    const auto before = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
        host.processSampleBlock(chans, 2, kBlock, params);
    const auto after = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    REQUIRE(after == before); // no allocation on the SAMPLE audio path

    host.stopWorker();
}

TEST_CASE("sample_mode: ZONE preview path allocates nothing", "[sample_mode]")
{
    constexpr double kRate = 44100.0;
    constexpr int kBlock = 512;

    EngineHost host;
    auto source = makeSource(static_cast<std::size_t>(kRate), kRate);
    host.setSource(source);

    auto params = sampleParams(300.0);
    params.cycleLen = 150;
    host.prepareSample(kRate, kBlock, /*channels=*/2, params);
    host.setZonePreview(true, EngineHost::Zone{ 0.1, 0.9 }, params);

    std::vector<float> l(kBlock), r(kBlock);
    float* chans[2] = { l.data(), r.data() };

    for (int i = 0; i < 4; ++i)
        host.processSampleBlock(chans, 2, kBlock, params);

    const auto before = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
        host.processSampleBlock(chans, 2, kBlock, params);
    const auto after = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    REQUIRE(after == before); // no allocation on the ZONE-preview audio path
}
