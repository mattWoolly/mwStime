// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// RealtimeStretcher — FREE-mode streaming front-end over the shared two-grain
// scheduler (ADR-006 causality contract; see the header for the contract and
// domain notes). process() is allocation-free after prepare() by hard
// requirement (architecture.md §4 audio-thread rules).

#include "mws/engine/RealtimeStretcher.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "mws/core/Resampler.h"
#include "mws/engine/TempoMap.h"
#include "mws/model/CharacterChain.h"
#include "mws/model/ModelSpec.h"

namespace mws::engine {

namespace detail = stretch::detail;

void RealtimeStretcher::prepare(double hostRate, int maxBlockFrames,
                                std::size_t numChannels,
                                const ParamSnapshot& params)
{
    assert(hostRate > 0.0 && maxBlockFrames > 0 && numChannels > 0);

    hostRate_ = hostRate;
    maxBlock_ = maxBlockFrames;
    channels_ = numChannels;

    const model::ModelSpec& spec = model::ModelSpec::get(params.model);
    assert(spec.shipping && "reserved model slot must never reach the engine");
    const ParamSnapshot clamped = spec.clamp(params);

    // THE single model-rate authority (dsp-engine.md §7.4): host rate when
    // character is OFF (§8.4 — the engine runs at host rate on unquantized
    // audio, the basis of the null tests).
    modelRate_ = model::CharacterChain::modelRateFor(params.model, clamped, hostRate);
    assert(modelRate_ > 0.0);

    // 30 s (PI) history ring at model rate, preallocated — the ONLY
    // allocation in this class.
    historyLen_ = static_cast<std::int64_t>(std::ceil(kHistorySeconds * modelRate_));
    history_.resize(numChannels, static_cast<std::size_t>(historyLen_));
    history_.sampleRate = modelRate_;

    // Internal model-domain read delay D = 2000 + fadeMax: the worst-case
    // cycle plus its crossfade (geometry-derived, NOT hard-coded — SpliceCal
    // recalibration must flow into the latency, dsp-engine.md §3.1).
    const detail::GrainGeometry worst =
        detail::GrainGeometry::fromCycle(kMaxCycleLen, cal_.overlapF);
    delayModel_ = worst.c + worst.fadeLen;

    // Exhaustion-check margin: the boundary check must keep the surviving
    // grain's remaining reads (<= D) valid while the write head advances by
    // up to one block before the next boundary check.
    resyncMargin_ = delayModel_ + maxBlock_;

    // Reported latency, HOST samples (dsp-engine.md §7.4; header formula).
    const double r = hostRate_ / modelRate_;
    double latency = std::ceil(static_cast<double>(worst.c) * r)
                     + std::ceil(static_cast<double>(worst.fadeLen) * r);
    if (modelRate_ != hostRate_)
    {
        // Host <-> model round trip through the §7.2 SincResampler (task
        // 033's FX chain): ingest group delay is in MODEL (output) samples —
        // express it in host samples — plus the playback (model -> host)
        // group delay, already in host samples.
        const double ingest =
            core::SincResampler::groupDelaySamples(modelRate_ / hostRate_) * r;
        const double playback =
            core::SincResampler::groupDelaySamples(hostRate_ / modelRate_);
        latency += std::ceil(ingest + playback);
    }
    latencyHost_ = static_cast<int>(latency);

    varispeed_ = (params.model == model::ModelId::S900);

    written_ = 0;
    produced_ = 0;
    readCeil_ = 0;
    hasPending_ = false;

    // SYNC state reset (task 023). The transport is delivered per block via
    // setTransport(); until then SYNC uses the wall-clock fallback.
    transport_ = TransportInfo{};
    lastKnownBpm_ = 0.0;
    windowLenModel_ = 0;
    syncFramesToBoundary_ = 0;
    syncWindowStartPpq_ = 0.0;
    nextBoundaryPpq_ = 0.0;
    ppqPerFrameModel_ = 0.0;
    syncFallbackArmed_ = false;
    syncTransportStarted_ = false;
    syncFirstResyncDone_ = false;

    applyParams(params);

    // Read heads start at -D: the stream's sample 0 leaves the engine exactly
    // D samples later (pre-roll reads return 0), making T=100% a pure delay
    // of exactly the reported latency (model domain == host domain when
    // character is OFF).
    sched_.reset(geom_, -static_cast<double>(delayModel_));
    readPos_ = -static_cast<double>(delayModel_);
    initialLaunchPending_ = true;
}

void RealtimeStretcher::setParams(const ParamSnapshot& params) noexcept
{
    // Non-automatable inputs are prepare()-time only (they change the latency
    // and the ring): a model/bandwidth/FS/character change must re-prepare.
    assert(params.model == active_.model);
    assert(params.character == active_.character);
    assert(params.bandwidth == active_.bandwidth);
    assert(params.sampleRateSel == active_.sampleRateSel);

    pending_ = params;
    hasPending_ = true;
}

void RealtimeStretcher::applyParams(const ParamSnapshot& raw) noexcept
{
    const model::ModelSpec& spec = model::ModelSpec::get(raw.model);
    const ParamSnapshot p = spec.clamp(raw);

    // SYNC mode (task 023, ADR-006 SYNC column): tempoSync == Host engages the
    // transport-aligned window resync and ALLOWS T < 100% compression (the
    // captured window plays compressed, then silence to the boundary). FREE
    // (tempoSync == Off) keeps the causality clamp below.
    syncActive_ = (raw.tempoSync == TempoSync::Host);

    // ADR-006 FREE causality clamp, applied AT THE ENGINE regardless of the
    // ModelSpec fxWindow rule: effective T = max(T, 100%). The raw automation
    // value is preserved upstream; clampActive() feeds the LCD `FX MIN 100%`
    // message. The same rule clamps the S900 varispeed rate (= 100/T) to <= 1
    // (ADR-003). SYNC supersedes the clamp — compression is a product feature.
    const double effectiveT = syncActive_ ? p.timeFactor : std::max(p.timeFactor, 100.0);
    clampActive_ = !syncActive_ && raw.timeFactor < 100.0;

    // cycleLen in model-rate samples (dsp-engine.md §3.5); ModelSpec keeps it
    // within [20, 2000] — the extra floor is engine arithmetic safety only.
    const auto cycleLen =
        std::clamp<std::int64_t>(p.cycleLen, 1, kMaxCycleLen);
    geom_ = detail::GrainGeometry::fromCycle(cycleLen, cal_.overlapF);

    if (p.hopMode == HopMode::Classic)
    {
        // CLASSIC: integer-% T, integer hop (same rounding arithmetic as the
        // offline schedule — dsp-engine.md §3.2). The hop is carried as an
        // integer-VALUED double: every read keeps a zero fraction and
        // short-circuits to the verbatim sample (header note).
        const auto tPct = std::max<std::int64_t>(1, std::llround(effectiveT));
        std::int64_t hopIn = 0;
        switch (cal_.rounding)
        {
            case stretch::HopRounding::RoundNearest:
                hopIn = (2 * geom_.hopOut * 100 + tPct) / (2 * tPct);
                break;
        }
        hopIn_ = static_cast<double>(std::max<std::int64_t>(1, hopIn));
    }
    else
    {
        // REVISED: fractional hop, 0.01% step preserved (dsp-engine.md §3.2).
        hopIn_ = static_cast<double>(geom_.hopOut) / (effectiveT / 100.0);
    }

    // S900 varispeed (ADR-003, dsp-engine.md §6): rate = 100/T. FREE clamps it
    // to <= 1 (T < 100% would consume input faster than it arrives); SYNC
    // window mode PERMITS rate > 1 (dsp-engine.md §6 last paragraph), so the
    // captured window compresses and then runs out into silence.
    const double rawRate = 100.0 / std::max(p.timeFactor, 1.0);
    readRate_ = syncActive_ ? rawRate : std::min(1.0, rawRate);

    active_ = raw;
}

float RealtimeStretcher::ringAt(std::size_t channel, std::int64_t pos) const noexcept
{
    if (pos < 0)
        return 0.0f; // pre-roll: the stream has not started yet
    // Readable ceiling: FREE pins it to the write head (so positions past the
    // write head are edge silence — the original FREE invariant); SYNC pins it
    // to the captured-window end so T<100% compression decays to silence to the
    // boundary instead of bleeding the next window (task 023). readCeil_ is
    // never above the write head, so reads never run past live input.
    if (pos >= readCeil_)
        return 0.0f;
    assert(pos < written_ && "scheduled read must stay behind the write head");
    assert(pos >= written_ - historyLen_ && "scheduled read fell out of history");
    return history_.channel(channel)[static_cast<std::size_t>(pos % historyLen_)];
}

float RealtimeStretcher::ringRead(std::size_t channel, double pos) const noexcept
{
    const double floorPos = std::floor(pos);
    const auto i0 = static_cast<std::int64_t>(floorPos);
    const double frac = pos - floorPos;
    const float s0 = ringAt(channel, i0);
    if (frac == 0.0)
        return s0;
    return detail::lerpSamples(s0, ringAt(channel, i0 + 1), frac);
}

void RealtimeStretcher::process(const core::ConstAudioView* inputs,
                                core::AudioView* outputs, std::size_t numChannels)
{
    assert(historyLen_ > 0 && "prepare() must run before process()");
    assert(numChannels == channels_);
    const std::size_t numFrames = inputs[0].size();
    assert(numFrames <= static_cast<std::size_t>(maxBlock_));

    // 1. Absorb the whole input block into the history ring first — output
    //    rendering then reads history only, which makes in-place processing
    //    safe and keeps the schedule partition-invariant (reads never exceed
    //    the write head because every scheduled position lags it by >= D).
    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        assert(inputs[ch].size() == numFrames && outputs[ch].size() == numFrames);
        core::AudioView ring = history_.channel(ch);
        const core::ConstAudioView in = inputs[ch];
        for (std::size_t i = 0; i < numFrames; ++i)
            ring[static_cast<std::size_t>((written_ + static_cast<std::int64_t>(i))
                                          % historyLen_)] = in[i];
    }
    written_ += static_cast<std::int64_t>(numFrames);

    // FREE: the read ceiling is the write head (reads past it are edge silence,
    // the original invariant). SYNC pins it per window in syncResyncReadHead()
    // so the captured window's tail decays to silence — but BEFORE the first
    // window boundary there is nothing captured yet, so SYNC tracks the write
    // head too (causal pre-roll, never a long dead patch of silence).
    if (!syncActive_ || !syncFirstResyncDone_)
        readCeil_ = written_;

    // SYNC: recompute the window length + boundary countdown for this block
    // from the transport (or the wall-clock fallback) — task 023.
    if (syncActive_)
        beginSyncBlock();

    // 2. Render exactly numFrames output samples (insert FX: 1:1 I/O).
    if (varispeed_)
        processVarispeed(outputs, numChannels, numFrames);
    else
        processCyclic(outputs, numChannels, numFrames);
}

void RealtimeStretcher::processCyclic(core::AudioView* outputs,
                                      std::size_t numChannels,
                                      std::size_t numFrames)
{
    // The initial grain (launched silently by prepare()) is reported on the
    // first rendered sample so observers attached after prepare() still see
    // the complete schedule (mirrors the offline render's {0, 0} report).
    if (initialLaunchPending_)
    {
        if (observer_ != nullptr && *observer_)
            (*observer_)(stretch::GrainLaunch{ produced_, sched_.grainA().off });
        initialLaunchPending_ = false;
    }

    for (std::size_t i = 0; i < numFrames; ++i)
    {
        // SYNC: at the transport-aligned window boundary the read head
        // hard-resyncs to the start of the just-captured window — "stretch the
        // last bar" (task 023). The boundary lands here regardless of grain
        // phase; this is the ONLY SYNC resync point (mid-window resyncs never
        // occur — the FREE exhaustion rule below is suppressed in SYNC). A
        // zero-length window (FxWindow::Free under SYNC) yields no boundaries.
        if (syncActive_ && windowLenModel_ > 0 && syncFramesToBoundary_ == 0)
            syncResyncReadHead();

        // ONE shared schedule across channels (architecture.md §5.2 stereo
        // rule): taps computed once, applied per channel.
        const auto taps = sched_.taps();
        for (std::size_t ch = 0; ch < numChannels; ++ch)
        {
            const float sA = ringRead(ch, taps.posA);
            float outSample = sA;
            if (taps.hasB)
                outSample = detail::crossfadeOutput(sA, ringRead(ch, taps.posB),
                                                    taps.fade15);
            outputs[ch][i] = outSample;
        }
        ++produced_;
        if (syncActive_)
            --syncFramesToBoundary_;

        const bool boundary = sched_.advance(
            [&](double aOff) { return aOff + hopIn_; },
            [&](double off)
            {
                if (observer_ != nullptr && *observer_)
                    (*observer_)(stretch::GrainLaunch{ produced_, off });
            });

        if (!boundary)
            continue;

        // --- grain boundary (ADR-006) ---------------------------------
        // (a) staged parameter changes take effect here, never mid-grain;
        if (hasPending_)
        {
            applyParams(pending_);
            sched_.setGeometry(geom_);
            hasPending_ = false;
        }

        // (b) FREE history-exhaustion resync: when the grain taking over has
        //     lagged to the history bound (margin D keeps its remaining reads
        //     valid while the write head advances), the read head jumps to
        //     writePos - latency (model domain). Audible, documented,
        //     observable through the launch hook. SUPERSEDED in SYNC — the
        //     window boundary is the only resync there (ADR-006 SYNC column).
        if (syncActive_)
            continue;
        const auto& a = sched_.grainA();
        if (!a.active
            || a.off < static_cast<double>(written_ - historyLen_ + resyncMargin_))
        {
            sched_.reset(geom_, static_cast<double>(written_ - delayModel_));
            if (observer_ != nullptr && *observer_)
                (*observer_)(
                    stretch::GrainLaunch{ produced_, sched_.grainA().off });
        }
    }
}

void RealtimeStretcher::processVarispeed(core::AudioView* outputs,
                                         std::size_t numChannels,
                                         std::size_t numFrames)
{
    // The S900 read head is continuous (no grains), so staged parameters
    // apply at the block boundary — the closest analogue of the
    // grain-boundary rule (documented deviation; dsp-engine.md §6).
    if (hasPending_)
    {
        applyParams(pending_);
        hasPending_ = false;
    }

    for (std::size_t i = 0; i < numFrames; ++i)
    {
        if (syncActive_)
        {
            // SYNC: hard-resync the read head to the captured-window start at
            // the transport-aligned boundary — the only resync in SYNC (task
            // 023). rate = 100/T is NOT clamped here (applyParams), so T<100%
            // runs the read head past the window end into silence. A
            // zero-length window (FxWindow::Free under SYNC) yields no resyncs.
            if (windowLenModel_ > 0 && syncFramesToBoundary_ == 0)
                syncResyncReadHead();
        }
        else
        {
            // FREE history exhaustion: same landing point as the cyclic resync
            // (writePos - latency), applied immediately — a continuous read has
            // no grain boundary to wait for.
            if (readPos_
                < static_cast<double>(written_ - historyLen_ + resyncMargin_))
                readPos_ = static_cast<double>(written_ - delayModel_);
        }

        // ZOH read, NO interpolation [DRR F4]: the variable-rate read IS the
        // virtual DAC clock the §8.1 character chain consumes (task 033).
        const auto ip = static_cast<std::int64_t>(std::floor(readPos_));
        for (std::size_t ch = 0; ch < numChannels; ++ch)
            outputs[ch][i] = ringAt(ch, ip);

        readPos_ += readRate_;
        ++produced_;
        if (syncActive_)
            --syncFramesToBoundary_;
    }
}

// ---------------------------------------------------------------------------
// SYNC mode (task 023)
// ---------------------------------------------------------------------------

void RealtimeStretcher::setTransport(const TransportInfo& transport) noexcept
{
    transport_ = transport;
    // Remember the most recent positive tempo for the no-transport wall-clock
    // fallback (architecture.md §5.2 / ADR-006: "last known tempo or 120 BPM").
    if (transport.bpm > 0.0)
        lastKnownBpm_ = transport.bpm;
}

void RealtimeStretcher::beginSyncBlock() noexcept
{
    const bool transportPresent = transport_.playing && transport_.bpm > 0.0;
    if (transportPresent)
    {
        // Transport-aligned window: re-anchor the boundary grid from the host
        // ppq every block. The bar grid is the source of truth, so boundaries
        // stay locked to it and a mid-stream tempo change is followed
        // automatically (the spacing is recomputed from the new tempo — the
        // testing-strategy §6 REAPER expectation). The window length in
        // quarter notes (TempoMap convention: bar = numerator * 4/denominator
        // quarters; 3/4 vs 4/4 differ).
        const double barQuarters =
            static_cast<double>(transport_.timeSigNumerator) * 4.0
            / static_cast<double>(transport_.timeSigDenominator);
        const double windowQuarters = TempoMap::windowBars(active_.fxWindow) * barQuarters;

        // ppq advance per rendered model frame: quarter notes per model-rate
        // sample = bpm / 60 / modelRate (model rate == host rate when character
        // is OFF, so the boundaries land on the host sample grid 1:1).
        ppqPerFrameModel_ = transport_.bpm / 60.0 / modelRate_;

        // The window length in model frames is windowQuarters / ppqPerFrame.
        windowLenModel_ = (windowQuarters > 0.0 && ppqPerFrameModel_ > 0.0)
                              ? std::llround(windowQuarters / ppqPerFrameModel_)
                              : 0;

        // The next boundary to resync at is the smallest grid line at or after
        // the block-start ppq (a tiny epsilon keeps an on-grid block start —
        // ppq exactly on a boundary — INCLUSIVE so a grid line reached mid
        // stream resyncs at sample 0 rather than skipping a full window ahead).
        if (windowQuarters > 0.0)
        {
            constexpr double kPpqEps = 1e-9;
            double k = std::ceil(transport_.ppqPosition / windowQuarters - kPpqEps);
            // First transport block: if playback STARTS exactly on a grid line
            // that sample is a window START, not a window end — capture that
            // window and resync at its END (the next grid line), so the first
            // resync is the first real boundary crossing, never sample 0.
            if (!syncTransportStarted_
                && (k * windowQuarters) <= transport_.ppqPosition + kPpqEps)
                k += 1.0;
            nextBoundaryPpq_ = k * windowQuarters;
            syncWindowStartPpq_ = nextBoundaryPpq_ - windowQuarters;
            const double framesToBoundary =
                (nextBoundaryPpq_ - transport_.ppqPosition) / ppqPerFrameModel_;
            syncFramesToBoundary_ =
                std::max<std::int64_t>(0, std::llround(framesToBoundary));
        }
        syncTransportStarted_ = true;
        syncFallbackArmed_ = false;
        return;
    }

    // No transport / Standalone: a free-running wall-clock window from the
    // last-known tempo (else 120 BPM (PI)). There is no bar grid to anchor to,
    // so the countdown is initialized ONCE and then free-runs (reset to the
    // window length at each boundary in syncResyncReadHead()).
    const double hostToModel = modelRate_ / hostRate_;
    const double windowLenHost =
        TempoMap::windowFromWallClock(lastKnownBpm_, hostRate_, active_.fxWindow);
    windowLenModel_ = std::llround(windowLenHost * hostToModel);
    syncWindowStartPpq_ = 0.0;
    ppqPerFrameModel_ = 0.0; // grid-free: syncResyncReadHead re-arms by length
    if (!syncFallbackArmed_)
    {
        syncFramesToBoundary_ = windowLenModel_;
        syncFallbackArmed_ = true;
    }
}

void RealtimeStretcher::syncResyncReadHead() noexcept
{
    // (a) staged parameter changes take effect at the window boundary (the
    //     SYNC analogue of the grain-boundary param-change point, ADR-006).
    if (hasPending_)
    {
        applyParams(pending_);
        hasPending_ = false;
    }

    // (b) hard-resync to the start of the just-captured window: read head jumps
    //     to writePos - windowLen ("stretch the last bar"). The readable
    //     ceiling is pinned to the window end (the write head at capture), so
    //     T<100% compression runs out into SILENCE to the boundary instead of
    //     bleeding the next window (ADR-006 SYNC column; dsp-engine.md §3.5).
    const std::int64_t windowStart =
        std::max<std::int64_t>(0, written_ - windowLenModel_);
    readCeil_ = written_;
    syncFirstResyncDone_ = true;

    if (varispeed_)
        readPos_ = static_cast<double>(windowStart);
    else
        sched_.reset(geom_, static_cast<double>(windowStart));

    if (resyncObserver_ != nullptr && *resyncObserver_)
        (*resyncObserver_)(SyncResync{ produced_, windowLenModel_, syncWindowStartPpq_ });

    // (c) re-arm the boundary countdown for the NEXT window. With a transport
    //     grid, advance to the next grid line (handles a second boundary
    //     within the same block before beginSyncBlock re-anchors); without one
    //     (wall-clock fallback) the next boundary is exactly one window away.
    if (ppqPerFrameModel_ > 0.0)
    {
        const double windowQuarters = windowLenModel_ * ppqPerFrameModel_;
        nextBoundaryPpq_ += windowQuarters;
        syncWindowStartPpq_ += windowQuarters;
    }
    syncFramesToBoundary_ = windowLenModel_;
}

} // namespace mws::engine
