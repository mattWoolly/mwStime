// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// EngineHost threading backbone implementation (task 030).
// See EngineHost.h and docs/design/architecture.md §4 for the protocol.

#include "EngineHost.h"

#include <algorithm>
#include <utility>

// ScopeFifo lives in the UI tree but its push/pushDecimated are the only
// audio-thread code it carries (plugin/ui/WaveformView.cpp); EngineHost owns one
// as the FX-scope producer. Included here (not in EngineHost.h) so the header
// stays GUI-free and FxEngine.h compiles into the JUCE-free core test binary.
#include "ui/WaveformView.h"

namespace mws::plugin {

EngineHost::EngineHost() = default;

EngineHost::~EngineHost()
{
    stopWorker();
}

// --- Message-thread API ------------------------------------------------------

void EngineHost::setSource(std::shared_ptr<const mws::core::AudioBuffer> source)
{
    source_.publish(std::move(source));
}

std::uint64_t EngineHost::requestRender(mws::engine::ParamSnapshot params)
{
    const auto id = nextRequestId_.fetch_add(1, std::memory_order_relaxed);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    requestFifo_.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return 0; // FIFO momentarily full — caller may retry

    requestSlots_[start1] = RenderRequest{ id, params };
    requestFifo_.finishedWrite(1);

    // Note: the abort flag is NOT cleared here. It is a latch the UI raises
    // while a render is in flight (hold-F8); the worker clears it when it
    // FINISHES a render (serviceRequest), so a fresh render always starts
    // un-aborted while an abort raised before/during a render is honoured
    // deterministically.
    wake_.signal();
    return id;
}

bool EngineHost::popEvent(WorkerEvent& out) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    eventFifo_.prepareToRead(1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return false;

    out = eventSlots_[size1 > 0 ? start1 : start2];
    eventFifo_.finishedRead(1);
    return true;
}

void EngineHost::startWorker()
{
    if (!worker_.isThreadRunning())
        worker_.startThread();
}

void EngineHost::stopWorker()
{
    if (worker_.isThreadRunning())
    {
        worker_.signalThreadShouldExit();
        wake_.signal();
        worker_.stopThread(2000);
    }
}

// --- Worker side -------------------------------------------------------------

bool EngineHost::pushEvent(const WorkerEvent& ev) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    eventFifo_.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return false; // FIFO full — advisory events may be dropped

    eventSlots_[size1 > 0 ? start1 : start2] = ev;
    eventFifo_.finishedWrite(1);
    return true;
}

void EngineHost::Worker::run()
{
    while (!threadShouldExit())
    {
        // Drain the request FIFO; the LAST request wins (latest edit), older
        // queued requests are discarded — the hardware re-renders from the
        // current settings, not a backlog.
        EngineHost::RenderRequest req{};
        bool haveReq = false;

        for (;;)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            host_.requestFifo_.prepareToRead(1, start1, size1, start2, size2);
            if (size1 + size2 < 1)
                break;
            req = host_.requestSlots_[size1 > 0 ? start1 : start2];
            host_.requestFifo_.finishedRead(1);
            haveReq = true;
        }

        if (haveReq)
        {
            host_.serviceRequest(req);
            // The abort latch is per-render: clear it once this render is done
            // (completed/refused/aborted) so the next render starts un-aborted.
            host_.abortFlag_.store(false, std::memory_order_release);
        }

        if (threadShouldExit())
            break;

        // Sleep until a new request (or shutdown) signals us.
        host_.wake_.wait(-1);
    }
}

namespace
{
RenderOutcome toOutcome(mws::engine::RenderError err) noexcept
{
    switch (err)
    {
        case mws::engine::RenderError::None: return RenderOutcome::Completed;
        case mws::engine::RenderError::NotEnoughMemory: return RenderOutcome::NotEnoughMemory;
        case mws::engine::RenderError::Aborted: return RenderOutcome::Aborted;
        case mws::engine::RenderError::UnsupportedModel: return RenderOutcome::UnsupportedModel;
    }
    return RenderOutcome::Completed;
}
} // namespace

void EngineHost::serviceRequest(const RenderRequest& req)
{
    pushEvent(WorkerEvent{ WorkerEvent::Kind::Started, req.id, 0.0f,
                           RenderOutcome::Completed });

    auto src = source_.current();
    if (!src || src->numFrames() == 0)
    {
        // Nothing to render from — publish an empty result, THEN post Finished,
        // so a message thread that wakes on the event already sees the result.
        published_.publish(std::make_shared<const RenderedSample>());
        pushFinished(req.id, RenderOutcome::Completed);
        return;
    }

    mws::engine::OfflineRenderer::Callbacks cb;
    cb.progress = [this, id = req.id](float p) {
        pushEvent(WorkerEvent{ WorkerEvent::Kind::Progress, id, p,
                               RenderOutcome::Completed });
    };
    cb.shouldAbort = [this]() {
        return abortFlag_.load(std::memory_order_acquire);
    };

    mws::engine::RenderResult result = renderer_.render(*src, req.params, cb);

    if (result.ok())
    {
        auto sample = std::make_shared<RenderedSample>();
        sample->audio = std::move(result.out);
        sample->info = result.info;
        sample->requestId = req.id;
        // Publish as immutable (shared_ptr<const>) — the producer-side store.
        published_.publish(std::shared_ptr<const RenderedSample>(std::move(sample)));
    }

    pushFinished(req.id, toOutcome(result.error));
}

void EngineHost::pushFinished(std::uint64_t requestId, RenderOutcome outcome) noexcept
{
    // The Finished event must never be lost (the UI relies on it to leave the
    // "rendering" state). Retry until the FIFO accepts it.
    const WorkerEvent ev{ WorkerEvent::Kind::Finished, requestId, 0.0f, outcome };
    while (!pushEvent(ev))
    {
        if (workerShouldExit())
            return;
        juce::Thread::sleep(1);
    }
}

bool EngineHost::workerShouldExit() const noexcept
{
    return worker_.shouldExit();
}

// --- FX path (task 033) ------------------------------------------------------

int EngineHost::prepareFx(double hostRate, int maxBlockFrames,
                          std::size_t numChannels,
                          const mws::engine::ParamSnapshot& params)
{
    fx_.prepare(hostRate, maxBlockFrames, numChannels, params);

    // Per-block channel-view scratch, sized once here so processFxBlock never
    // allocates (architecture.md §4).
    fxIns_.assign(numChannels, mws::core::ConstAudioView{});
    fxOuts_.assign(numChannels, mws::core::AudioView{});

    // The FX input scope FIFO (one rolling window of decimated samples). Sized
    // generously: a worst-case block at the smallest decimation still fits with
    // wide margin against the 30 Hz UI drain.
    if (!scopeFifo_)
        scopeFifo_ =
            std::make_unique<mws::ui::waveform::ScopeFifo>(/*capacity=*/8192);

    return fx_.latencySamples();
}

bool EngineHost::reconfigureFxIfNeeded(const mws::engine::ParamSnapshot& params)
{
    if (!fx_.requestReconfigure(params))
        return false;
    return fx_.consumeLatencyDirty();
}

void EngineHost::processFxBlock(
    float* const* channelData, int numChannels, int numFrames,
    const mws::engine::ParamSnapshot& params,
    const mws::engine::RealtimeStretcher::TransportInfo& transport) noexcept
{
    if (numChannels <= 0 || numFrames <= 0)
        return;

    const auto chans = static_cast<std::size_t>(numChannels);
    const auto frames = static_cast<std::size_t>(numFrames);

    // The scratch arrays were sized in prepareFx to the prepared channel count;
    // never grow them on the audio thread. If the host hands us more channels
    // than prepared (should not happen with the fixed stereo bus), clamp.
    const auto n = std::min<std::size_t>(chans, fxIns_.size());
    if (n == 0)
        return;

    for (std::size_t ch = 0; ch < n; ++ch)
    {
        fxIns_[ch] = mws::core::ConstAudioView{ channelData[ch], frames };
        fxOuts_[ch] = mws::core::AudioView{ channelData[ch], frames };
    }

    // FX input scope feed: push channel-0 decimated samples (architecture.md §4
    // FIFO→timer poll). Producer-side decimation; drops when full (never blocks).
    if (scopeFifo_ != nullptr)
        scopeFifo_->pushDecimated(channelData[0], numFrames, kScopeDecimation);

    fx_.processBlock(fxIns_.data(), fxOuts_.data(), n, params, transport);
}

} // namespace mws::plugin
