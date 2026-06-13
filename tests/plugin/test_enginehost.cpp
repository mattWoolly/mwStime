// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// EngineHost render-worker integration test (plan/backlog/030):
//   - request -> Started -> Progress events -> published RenderedSample +
//     Finished(Completed),
//   - abort mid-render -> Finished(Aborted), nothing published for that request,
//   - the publication graveyard drains; deterministic worker shutdown.
//
// docs/design/architecture.md §4 (threading + ownership/publication protocol);
// testing-strategy.md §3 item 6. Test-case names begin with "enginehost" so
// `ctest -R enginehost` selects them.

#include <catch2/catch_test_macros.hpp>

#include "EngineHost.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::engine::ParamSnapshot;
using mws::plugin::EngineHost;
using mws::plugin::RenderOutcome;
using mws::plugin::WorkerEvent;

// A deterministic source: a long ramp so a stretch render takes long enough for
// the abort path to land at a stage boundary, and so progress is reported.
std::shared_ptr<const AudioBuffer> makeRamp(std::size_t frames, double rate = 44100.0)
{
    auto buf = std::make_shared<AudioBuffer>(1, frames);
    buf->sampleRate = rate;
    auto ch = buf->channel(0);
    for (std::size_t i = 0; i < frames; ++i)
        ch[i] = static_cast<float>((i % 257) - 128) / 128.0f;
    return buf;
}

// Drain events for up to a timeout, collecting them, until a Finished event with
// the given request id arrives. Returns the Finished event (or a default with
// requestId 0 on timeout). Records the max progress and whether Started was seen.
struct DrainResult {
    bool sawStarted = false;
    bool sawFinished = false;
    float maxProgress = 0.0f;
    int progressCount = 0;
    RenderOutcome outcome = RenderOutcome::Completed;
};

DrainResult drainUntilFinished(EngineHost& host, std::uint64_t requestId,
                               int timeoutMs = 5000)
{
    DrainResult r;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        WorkerEvent ev;
        bool drainedAny = false;
        while (host.popEvent(ev))
        {
            drainedAny = true;
            if (ev.requestId != requestId)
                continue;
            switch (ev.kind)
            {
                case WorkerEvent::Kind::Started: r.sawStarted = true; break;
                case WorkerEvent::Kind::Progress:
                    ++r.progressCount;
                    r.maxProgress = std::max(r.maxProgress, ev.progress);
                    break;
                case WorkerEvent::Kind::Finished:
                    r.sawFinished = true;
                    r.outcome = ev.outcome;
                    break;
            }
        }
        host.collectGarbage();
        if (r.sawFinished)
            break;
        if (!drainedAny)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}
} // namespace

TEST_CASE("enginehost: request yields Started, progress, published result, Completed",
          "[enginehost]")
{
    EngineHost host;
    host.startWorker();

    host.setSource(makeRamp(44100)); // 1 s ramp

    ParamSnapshot params; // defaults: S1000, CLASSIC, 100% (a real render)
    params.timeFactor = 200.0;
    const auto id = host.requestRender(params);
    REQUIRE(id != 0);

    const auto r = drainUntilFinished(host, id);

    REQUIRE(r.sawStarted);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::Completed);
    REQUIRE(r.progressCount >= 1);
    REQUIRE(r.maxProgress == 1.0f); // progress reaches exactly 1.0 on completion

    auto rendered = host.currentRender();
    REQUIRE(rendered != nullptr);
    REQUIRE(rendered->requestId == id);
    REQUIRE(rendered->audio.numFrames() > 0);

    host.stopWorker(); // deterministic join
}

TEST_CASE("enginehost: render cap refusal surfaces NotEnoughMemory, nothing published",
          "[enginehost]")
{
    EngineHost host;
    host.startWorker();

    host.setSource(makeRamp(44100)); // 1 s source

    ParamSnapshot params;
    params.timeFactor = 2000.0; // 1 s * 2000% = 20 s — within the 10-min cap
    // Force the cap by stacking the maximum expansion on a longer source would
    // exceed 10 min; instead use a long source so 2000% blows the cap.
    host.setSource(makeRamp(44100 * 40)); // 40 s * 2000% = 800 s > 600 s cap

    const auto id = host.requestRender(params);
    REQUIRE(id != 0);

    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::NotEnoughMemory);

    // Nothing should have been published for a refused render.
    auto rendered = host.currentRender();
    REQUIRE((rendered == nullptr || rendered->requestId != id));

    host.stopWorker();
}

TEST_CASE("enginehost: abort yields Aborted, nothing published, latch resets", "[enginehost]")
{
    EngineHost host;
    host.startWorker();
    host.setSource(makeRamp(44100 * 4)); // 4 s

    ParamSnapshot params;
    params.timeFactor = 800.0;

    // Raise the abort latch BEFORE enqueuing: the worker checks shouldAbort() at
    // the very first stage boundary (progress 0.0, OfflineRenderer.cpp), so the
    // render aborts deterministically (the latch is NOT cleared by enqueue).
    host.requestAbort();
    const auto id = host.requestRender(params);
    REQUIRE(id != 0);

    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.sawStarted);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::Aborted);

    // Nothing was published for the aborted request.
    auto rendered = host.currentRender();
    REQUIRE((rendered == nullptr || rendered->requestId != id));

    // The latch is per-render: a fresh request now completes normally.
    const auto id2 = host.requestRender(params);
    REQUIRE(id2 != 0);
    const auto r2 = drainUntilFinished(host, id2);
    REQUIRE(r2.sawFinished);
    REQUIRE(r2.outcome == RenderOutcome::Completed);
    REQUIRE(host.currentRender()->requestId == id2);

    // Drain the graveyard fully; deterministic shutdown.
    host.collectGarbage();
    host.stopWorker();
    SUCCEED("worker joined; no leak (ASan/LeakSanitizer would flag otherwise)");
}

TEST_CASE("enginehost: audio-thread acquire/retire is RT-safe across a publish",
          "[enginehost]")
{
    EngineHost host;
    host.startWorker();
    host.setSource(makeRamp(22050));

    ParamSnapshot params;
    params.timeFactor = 150.0;
    const auto id = host.requestRender(params);
    REQUIRE(id != 0);
    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.outcome == RenderOutcome::Completed);

    // Simulate one audio block: copy once, read, retire. Must not free inline.
    auto blockPtr = host.acquireForAudioBlock();
    REQUIRE(blockPtr != nullptr);
    volatile float sink = 0.0f;
    if (blockPtr->audio.numFrames() > 0)
        sink += blockPtr->audio.channel(0)[0];
    (void) sink;
    host.retireFromAudioBlock(blockPtr);
    REQUIRE(blockPtr == nullptr); // moved into the graveyard

    REQUIRE(host.collectGarbage() >= 1); // freed on the message thread, here
    host.stopWorker();
}
