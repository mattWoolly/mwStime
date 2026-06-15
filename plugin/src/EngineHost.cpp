// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// EngineHost threading backbone implementation (task 030).
// See EngineHost.h and docs/design/architecture.md §4 for the protocol.

#include "EngineHost.h"

#include "SamplePlayer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "mws/engine/TempoMap.h"

// ScopeFifo lives in the UI tree but its push/pushDecimated are the only
// audio-thread code it carries (plugin/ui/WaveformView.cpp); EngineHost owns one
// as the FX-scope producer. Included here (not in EngineHost.h) so the header
// stays GUI-free and FxEngine.h compiles into the JUCE-free core test binary.
#include "ui/WaveformView.h"

namespace mws::plugin {

EngineHost::EngineHost() : player_(std::make_unique<SamplePlayer>()) {}

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
    return requestRender(params, Zone{ 0.0, 1.0 });
}

std::uint64_t EngineHost::requestRender(mws::engine::ParamSnapshot params, Zone zone)
{
    const auto id = nextRequestId_.fetch_add(1, std::memory_order_relaxed);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    requestFifo_.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return 0; // FIFO momentarily full — caller may retry

    requestSlots_[start1] = RenderRequest{ id, params, zone.start, zone.end };
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
/// Worker-thread: crop `src` to the normalized [start, end) slice into a fresh
/// AudioBuffer (allocating — off the audio thread). Returns nullptr/empty for an
/// empty or inverted selection. The slice keeps the source's sample rate.
std::shared_ptr<const mws::core::AudioBuffer>
sliceSource(const mws::core::AudioBuffer& src, double start, double end)
{
    const auto total = static_cast<std::int64_t>(src.numFrames());
    const double clampedStart = std::clamp(start, 0.0, 1.0);
    const double clampedEnd = std::clamp(end, 0.0, 1.0);
    auto from = static_cast<std::int64_t>(std::llround(clampedStart * static_cast<double>(total)));
    auto to = static_cast<std::int64_t>(std::llround(clampedEnd * static_cast<double>(total)));
    from = std::clamp<std::int64_t>(from, 0, total);
    to = std::clamp<std::int64_t>(to, 0, total);
    if (to <= from)
        return std::make_shared<const mws::core::AudioBuffer>(); // empty

    const auto len = static_cast<std::size_t>(to - from);
    auto out = std::make_shared<mws::core::AudioBuffer>(src.numChannels(), len);
    out->sampleRate = src.sampleRate;
    for (std::size_t ch = 0; ch < src.numChannels(); ++ch)
    {
        const auto in = src.channel(ch);
        auto dst = out->channel(ch);
        for (std::size_t i = 0; i < len; ++i)
            dst[i] = in[static_cast<std::size_t>(from) + i];
    }
    return out;
}

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

    // GO renders the ZONE slice (ui-design.md §6.3 step 2). Crop the source to
    // the normalized [zoneStart, zoneEnd) selection HERE on the worker thread
    // (allocating off the audio thread) before the OfflineRenderer runs; a full
    // request is {0, 1} and the slice is the whole source. An empty/inverted
    // zone yields an empty render (nothing to stretch).
    std::shared_ptr<const mws::core::AudioBuffer> renderSrc = src;
    if (req.zoneStart > 0.0 || req.zoneEnd < 1.0)
    {
        renderSrc = sliceSource(*src, req.zoneStart, req.zoneEnd);
        if (!renderSrc || renderSrc->numFrames() == 0)
        {
            published_.publish(std::make_shared<const RenderedSample>());
            pushFinished(req.id, RenderOutcome::Completed);
            return;
        }
    }

    mws::engine::OfflineRenderer::Callbacks cb;
    cb.progress = [this, id = req.id](float p) {
        pushEvent(WorkerEvent{ WorkerEvent::Kind::Progress, id, p,
                               RenderOutcome::Completed });
    };
    cb.shouldAbort = [this]() {
        return abortFlag_.load(std::memory_order_acquire);
    };

    mws::engine::RenderResult result = renderer_.render(*renderSrc, req.params, cb);

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
    // Size the per-block channel-view scratch BEFORE handing the engine to the
    // audio thread, and only when the channel count actually changes (task 059).
    // AU/ausdk overlap prepareToPlay with Render; processFxBlock rewrites every
    // element from channelData before use, so re-assigning the same-sized vector
    // on each re-prepare would be a benign-but-flagged write under a live read.
    // Doing it before fx_.prepare() (which publishes the engine) and only on a
    // genuine size change keeps it off the audio-thread read path. Sized once so
    // processFxBlock never allocates (architecture.md §4).
    if (fxIns_.size() != numChannels)
        fxIns_.assign(numChannels, mws::core::ConstAudioView{});
    if (fxOuts_.size() != numChannels)
        fxOuts_.assign(numChannels, mws::core::AudioView{});

    fx_.prepare(hostRate, maxBlockFrames, numChannels, params);

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

    // Belt-and-braces (task 059): if prepareToPlay has not built the FX engine
    // yet, leave the buffer as-is (dry passthrough) rather than driving an
    // unprepared engine. FxEngine::processBlock also guards each adopted engine
    // (engineRunnable + passDry), so a torn/half-prepared engine can never reach
    // RealtimeStretcher::process; this is the outer early-out for the no-engine
    // case. AU/ausdk may overlap prepareToPlay with Render, so this runs on the
    // audio thread and reads only the atomic prepared_ flag.
    if (!fx_.isPrepared())
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

    // Host tempo sync (task 037): when tempoSync == HOST, derive the effective
    // time factor from sourceBPM + hostBPM and OVERRIDE it on a snapshot copy —
    // the caller's snapshot (and the APVTS automation value behind it) is never
    // mutated (architecture.md §6 clamp/override pattern). SYNC OFF returns the
    // snapshot unchanged.
    const mws::engine::ParamSnapshot effective = applyTempoSync(params, transport);

    fx_.processBlock(fxIns_.data(), fxOuts_.data(), n, effective, transport);
}

// --- Host tempo sync (task 037) ----------------------------------------------

void EngineHost::setSourceBpm(double bpm, bool userSet) noexcept
{
    if (bpm > 0.0)
    {
        sourceBpm_.store(bpm, std::memory_order_release);
        sourceBpmUserSet_.store(userSet, std::memory_order_release);
    }
    else
    {
        // Non-positive clears the value back to "unknown" and drops the
        // user-set latch so a later filename guess can populate it.
        sourceBpm_.store(0.0, std::memory_order_release);
        sourceBpmUserSet_.store(false, std::memory_order_release);
    }
}

bool EngineHost::guessSourceBpmFromFilename(const std::string& fileName) noexcept
{
    // Never clobber an explicit user/typed/tap value (ui-design §6.2 step 4).
    if (sourceBpmUserSet_.load(std::memory_order_acquire))
        return false;

    // Scan for a `(\d+(?:\.\d+)?)bpm` tag, case-insensitive. Hand-rolled (no
    // <regex> — it is heavyweight and this runs on the message thread on load):
    // find each case-insensitive "bpm" and read the contiguous numeric run that
    // immediately precedes it. The LAST such tag in the name wins (filenames
    // put the tempo nearest the descriptive suffix, e.g. `amen_174bpm.wav`).
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    double found = 0.0;
    bool any = false;
    const std::size_t n = fileName.size();
    for (std::size_t i = 0; i + 3 <= n; ++i)
    {
        if (lower(fileName[i]) != 'b' || lower(fileName[i + 1]) != 'p'
            || lower(fileName[i + 2]) != 'm')
            continue;

        // Walk backwards over an optional fractional number directly before it.
        std::size_t end = i;          // one-past the last digit/dot
        std::size_t start = i;        // first digit/dot
        bool seenDigit = false;
        bool seenDot = false;
        while (start > 0)
        {
            const char c = fileName[start - 1];
            if (c >= '0' && c <= '9')
            {
                seenDigit = true;
                --start;
            }
            else if (c == '.' && !seenDot)
            {
                seenDot = true;
                --start;
            }
            else
            {
                break;
            }
        }
        if (!seenDigit || start >= end)
            continue;

        // Parse [start, end) as a decimal (guard a leading/lone '.').
        const std::string token = fileName.substr(start, end - start);
        char* endPtr = nullptr;
        const double value = std::strtod(token.c_str(), &endPtr);
        if (endPtr != token.c_str() && value > 0.0)
        {
            found = value;
            any = true;
        }
    }

    if (!any)
        return false;

    // Adopt as a NON-user value (still overridable by a future user entry).
    sourceBpm_.store(found, std::memory_order_release);
    sourceBpmUserSet_.store(false, std::memory_order_release);
    return true;
}

mws::engine::ParamSnapshot EngineHost::applyTempoSync(
    const mws::engine::ParamSnapshot& params,
    const mws::engine::RealtimeStretcher::TransportInfo& transport) noexcept
{
    using mws::engine::TempoMap;

    // Track the last positive host tempo for the no-transport fallback (mirrors
    // the stretcher's own lastKnownBpm retention for the window math — here it
    // keeps the effective FACTOR sensible when a block reports no tempo).
    if (transport.bpm > 0.0)
        lastKnownHostBpm_.store(transport.bpm, std::memory_order_release);

    if (params.tempoSync != mws::engine::TempoSync::Host)
    {
        syncReadoutActive_.store(false, std::memory_order_release);
        return params;
    }

    const double source = sourceBpm_.load(std::memory_order_acquire);
    double host = transport.bpm;
    if (host <= 0.0)
        host = lastKnownHostBpm_.load(std::memory_order_acquire);

    // No usable BPM pair: leave the time factor (the automation value) as-is and
    // mark the readout inactive — sync cannot compute a meaningful factor yet.
    if (source <= 0.0 || host <= 0.0)
    {
        syncReadoutActive_.store(false, std::memory_order_release);
        return params;
    }

    // Effective T = 100 * source / host (dsp-engine.md §2, direction-corrected).
    double effectiveT = TempoMap::syncedTimeFactor(source, host);
    // CLASSIC coerces to an integer percent (the readout shows the achieved
    // value; the engine applies the same integer rounding internally).
    if (params.hopMode == mws::engine::HopMode::Classic)
        effectiveT = static_cast<double>(TempoMap::quantizeClassic(effectiveT));

    // OVERRIDE the snapshot copy only — the ADR-006 FREE/SYNC clamp is applied
    // downstream in RealtimeStretcher::applyParams (FREE: max(T,100); SYNC:
    // compression allowed). The APVTS automation value is never touched.
    mws::engine::ParamSnapshot effective = params;
    effective.timeFactor = effectiveT;

    // Refresh the readout cache (LCD poll, task 041).
    syncReadoutSource_.store(source, std::memory_order_release);
    syncReadoutHost_.store(host, std::memory_order_release);
    syncReadoutResult_.store(effectiveT, std::memory_order_release);
    syncReadoutActive_.store(true, std::memory_order_release);
    return effective;
}

EngineHost::SyncReadout EngineHost::fxSyncReadout() const noexcept
{
    SyncReadout r;
    r.active = syncReadoutActive_.load(std::memory_order_acquire);
    r.sourceBpm = syncReadoutSource_.load(std::memory_order_acquire);
    r.hostBpm = syncReadoutHost_.load(std::memory_order_acquire);
    r.resultPercent = syncReadoutResult_.load(std::memory_order_acquire);
    return r;
}

// --- SAMPLE path (task 034) --------------------------------------------------

void EngineHost::prepareSample(double hostRate, int maxBlockFrames,
                               std::size_t numChannels,
                               const mws::engine::ParamSnapshot& params)
{
    sampleHostRate_ = hostRate;
    sampleMaxBlock_ = maxBlockFrames;
    sampleChannels_ = numChannels;

    player_->prepare(hostRate);

    // Per-block channel-view scratch for the SAMPLE path (sized once here so
    // processSampleBlock allocates nothing — architecture.md §4). Independent of
    // the FX scratch so SAMPLE-only preparation works.
    sampleIns_.assign(numChannels, mws::core::ConstAudioView{});
    sampleOuts_.assign(numChannels, mws::core::AudioView{});

    // The ZONE-preview stretcher reuses the FX streaming engine at host rate
    // (the same character-ON deviation as FxEngine — no host<->model resampler
    // in the path yet, plan/backlog/053). Prepared here so the 30 s history-ring
    // allocation happens off the audio thread.
    zonePreview_.prepare(hostRate, maxBlockFrames, numChannels, params);
    zoneScratch_.resize(numChannels, static_cast<std::size_t>(juce::jmax(1, maxBlockFrames)));
    zoneReadPos_ = 0.0;
    // ZONE preview is CYCLIC-models-only (ui-design.md §6.3); the S900 is a
    // varispeed repitch model with no stretch, so preview is inert there.
    zonePreviewSupported_.store(params.model != mws::model::ModelId::S900,
                                std::memory_order_release);
}

void EngineHost::startSamplePlayback() noexcept { player_->start(); }
void EngineHost::stopSamplePlayback() noexcept { player_->stop(); }

bool EngineHost::isSamplePlaying() const noexcept { return player_->isPlaying(); }

void EngineHost::setAuditionSource(AuditionSource mode) noexcept
{
    player_->setSourceMode(mode);
}

AuditionSource EngineHost::auditionSource() const noexcept
{
    return player_->sourceMode();
}

void EngineHost::setZonePreview(bool enabled, Zone zone,
                                const mws::engine::ParamSnapshot& params) noexcept
{
    zoneStartNorm_.store(zone.start, std::memory_order_release);
    zoneEndNorm_.store(zone.end, std::memory_order_release);
    zonePreviewSupported_.store(params.model != mws::model::ModelId::S900,
                                std::memory_order_release);
    // Only the S900 (no stretch) is barred; other models preview through the
    // cyclic/S950 streaming engine.
    const bool on = enabled && params.model != mws::model::ModelId::S900;
    zonePreviewOn_.store(on, std::memory_order_release);
}

void EngineHost::processSampleBlock(float* const* channelData, int numChannels,
                                    int numFrames,
                                    const mws::engine::ParamSnapshot& params) noexcept
{
    if (numChannels <= 0 || numFrames <= 0)
        return;

    const auto chans = static_cast<std::size_t>(numChannels);
    const auto frames = static_cast<std::size_t>(numFrames);
    // The scratch view arrays were sized in prepareSample; never grow them on
    // the audio thread. Clamp if the host hands us more channels than prepared.
    const auto n = std::min<std::size_t>(chans, sampleOuts_.size());
    if (n == 0)
        return;

    if (zonePreviewOn_.load(std::memory_order_acquire))
    {
        // ZONE live preview: loop the selected zone of the source through the
        // preview stretcher (CYCLIC models only). Read the source once per block
        // (RCU) and retire it.
        auto src = source_.acquire();
        if (src && src->numFrames() > 0)
        {
            const auto total = static_cast<std::int64_t>(src->numFrames());
            const double s0 = std::clamp(zoneStartNorm_.load(std::memory_order_acquire), 0.0, 1.0);
            const double s1 = std::clamp(zoneEndNorm_.load(std::memory_order_acquire), 0.0, 1.0);
            auto from = static_cast<std::int64_t>(std::llround(s0 * static_cast<double>(total)));
            auto to = static_cast<std::int64_t>(std::llround(s1 * static_cast<double>(total)));
            from = std::clamp<std::int64_t>(from, 0, total);
            to = std::clamp<std::int64_t>(to, from, total);
            const std::int64_t zoneLen = std::max<std::int64_t>(1, to - from);
            const auto srcChans = src->numChannels();

            // Fill the scratch with the looped zone, then stretch it in place.
            for (std::size_t ch = 0; ch < n; ++ch)
            {
                auto dst = zoneScratch_.channel(ch);
                const auto srcCh = srcChans == 0 ? 0 : std::min(ch, srcChans - 1);
                const auto in = src->channel(srcCh);
                double pos = zoneReadPos_;
                for (std::size_t i = 0; i < frames; ++i)
                {
                    auto idx = from + static_cast<std::int64_t>(pos);
                    // Wrap inside the zone (looping playback of the selection).
                    idx = from + ((idx - from) % zoneLen);
                    dst[i] = in[static_cast<std::size_t>(idx)];
                    pos += 1.0;
                }
            }
            // Advance the shared loop head once (the schedule is shared across
            // channels — architecture.md §5.2 dual-mono shared schedule).
            zoneReadPos_ += static_cast<double>(frames);
            if (zoneLen > 0)
                zoneReadPos_ = std::fmod(zoneReadPos_, static_cast<double>(zoneLen));

            for (std::size_t ch = 0; ch < n; ++ch)
            {
                sampleIns_[ch] = mws::core::ConstAudioView{ zoneScratch_.channel(ch).data(), frames };
                sampleOuts_[ch] = mws::core::AudioView{ channelData[ch], frames };
            }
            zonePreview_.setParams(params);
            zonePreview_.process(sampleIns_.data(), sampleOuts_.data(), n);

            // outTrim applies to all SAMPLE output (dsp-engine.md §2).
            applyOutTrimChannels(channelData, n, frames, params.outTrim);

            // RCU: drop the per-block source copy. retire() pushes it into the
            // preallocated graveyard ring — never freed on the audio thread.
            source_.retire(src);
            return;
        }
        // No source: drop the copy and fall through to silence playback.
        source_.retire(src);
    }

    // PLAY / A-B audition: render the SamplePlayer over the JUCE channels. Read
    // the published render (B) and source (A) once per block (RCU).
    auto render = published_.acquire();
    auto src = source_.acquire();

    for (std::size_t ch = 0; ch < n; ++ch)
        sampleOuts_[ch] = mws::core::AudioView{ channelData[ch], frames };
    player_->process(sampleOuts_.data(), n, render, src, params.outTrim);

    // Retire the per-block copies to the graveyards (freed on the message
    // thread). retire() takes the pointer by reference and empties it.
    published_.retire(render);
    source_.retire(src);
}

void EngineHost::applyOutTrimChannels(float* const* channelData, std::size_t numChannels,
                                      std::size_t numFrames, double trimDb) noexcept
{
    if (trimDb == 0.0)
        return; // bit-exact at 0 dB
    const auto gain = static_cast<float>(std::pow(10.0, trimDb / 20.0));
    for (std::size_t ch = 0; ch < numChannels; ++ch)
        for (std::size_t i = 0; i < numFrames; ++i)
            channelData[ch][i] *= gain;
}

} // namespace mws::plugin
