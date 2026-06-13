// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// FxEngine — the FX-mode audio-thread engine wrapper + the model/bandwidth/FS
// reconfiguration handoff (plan/backlog/033; docs/design/architecture.md §4
// ownership/publication protocol, §5.2 FX causality contract; dsp-engine.md
// §3.5, §7.4).
//
// Responsibilities (the FX half of EngineHost — kept in its own header so it is
// deliberately JUCE-free and compiles into the JUCE-free core test binary, the
// same trick Published.h uses, so the FX history reconfiguration handoff runs
// under the `tsan` preset — testing-strategy.md §3.6):
//   - prepare(): builds the active RealtimeStretcher (preallocates its 30 s
//     history ring), computes the latency, captures the model rate. Message
//     thread / prepareToPlay only.
//   - processBlock(): the audio-thread path. It (1) adopts any pending
//     reconfiguration published from the message thread (swapping in the freshly
//     prepared replacement engine and retiring the old one to a graveyard FIFO,
//     so NOTHING is allocated or freed on the audio thread), (2) stages the
//     per-block automatable parameter snapshot at the next grain boundary,
//     (3) supplies the transport (SYNC math is engine-side), (4) runs
//     RealtimeStretcher::process (FREE + SYNC, dual-mono shared schedule), and
//     (5) applies the outTrim gain. Allocation-free and lock-free.
//   - requestReconfigure(): message thread. model/bandwidth/FS are
//     non-automatable and change the reported latency (§7.4), so a change builds
//     a brand-new prepared RealtimeStretcher (the heavy 30 s-ring allocation
//     happens HERE, off the audio thread) and publishes it; the new latency is
//     computed here and surfaced via latencySamples()/latencyDirty().
//   - collectGarbage(): message thread; frees retired engines here, never on
//     the audio thread (architecture.md §4).
//
// The handoff uses the SAME audited RCU/graveyard mechanism as the render result
// (Published.h GraveyardFifo) — the FX history reconfig is one of the three
// cross-thread handoffs task 030 mandated share one mechanism.
//
// DEVIATION (documented, plan/backlog/033 PR): the FX path runs
// RealtimeStretcher directly on the host-rate block. With CHARACTER OFF the
// model rate IS the host rate (CharacterChain §8.4), so this is exactly correct
// and null-testable (RealtimeStretcher.h §56-63) — the path every task-033
// verification test exercises. With CHARACTER ON the dsp-engine.md §3.5 host-rate
// rule wants the ingest character (host -> model rate, 12/16-bit quantize) run
// BEFORE the stretcher and the playback character (model -> host) AFTER it. That
// needs an allocation-free STREAMING resampler with continuous cross-block phase
// (the per-block 12-bit/16-bit character chain). The mwstime-core only ships an
// OFFLINE allocating SincResampler (core/Resampler.h) — no RT-safe streaming
// variant exists — so a faithful streaming character chain is a separate engine-
// layer effort, NOT plugin glue. Until it lands, character-ON FX runs the
// stretcher at host rate (the reported latency still includes the §7.4 SRC
// group-delay term, so PDC is honest); the offline SAMPLE-mode render remains the
// bit-faithful character path. This keeps the task in scope and the contract
// (latency + grain-boundary param application + the null) fully met.

#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/engine/RealtimeStretcher.h"
#include "mws/model/CharacterChain.h"
#include "mws/model/ModelSpec.h"

#include "Published.h"

namespace mws::plugin {

/// The FX-mode engine wrapper. One instance per processor; owned by EngineHost.
///
/// Threading: prepare()/requestReconfigure()/collectGarbage()/latencySamples()
/// run on the message thread; processBlock() runs on the audio thread. The two
/// sides communicate only through the lock-free pending slot and graveyard.
class FxEngine
{
public:
    /// What model/bandwidth/FS configuration the latency depends on
    /// (dsp-engine.md §7.4 — these are exactly the non-automatable inputs). A
    /// processBlock-time snapshot only re-prepares when one of these changes.
    struct ConfigKey
    {
        mws::model::ModelId model = mws::model::ModelId::S1000;
        double bandwidth = 19.2;
        mws::engine::SampleRateSel sampleRateSel =
            mws::engine::SampleRateSel::Fs44100;
        bool character = true;

        [[nodiscard]] bool operator==(const ConfigKey& o) const noexcept
        {
            // bandwidth comes verbatim from the same discrete APVTS atomic, so an
            // exact compare is the intent ("did the UI value change?"); the
            // not-less-not-greater form sidesteps -Wfloat-equal while staying
            // exact (and false for any NaN, which is correct — treat as changed).
            const bool sameBw = !(bandwidth < o.bandwidth) && !(o.bandwidth < bandwidth);
            return model == o.model && sameBw && sampleRateSel == o.sampleRateSel
                   && character == o.character;
        }
        [[nodiscard]] bool operator!=(const ConfigKey& o) const noexcept
        {
            return !(*this == o);
        }
    };

    static ConfigKey keyOf(const mws::engine::ParamSnapshot& p) noexcept
    {
        return ConfigKey{ p.model, p.bandwidth, p.sampleRateSel, p.character };
    }

    /// A prepared engine plus the snapshot it was prepared with. The two travel
    /// together across the handoff so the audio thread always knows the active
    /// engine's non-automatable configuration (model/bandwidth/FS/character) even
    /// in the brief window where the per-block param snapshot — taken from the
    /// live APVTS — has already moved on to a NEW model the audio thread has not
    /// adopted yet. setParams() debug-asserts the model is unchanged, so the FX
    /// path must feed it the active engine's own non-automatable fields.
    struct PreparedFx
    {
        mws::engine::RealtimeStretcher stretcher;
        mws::engine::ParamSnapshot config{}; // non-automatable inputs are taken
                                             // from here, automatable from the block
    };

    FxEngine() = default;

    FxEngine(const FxEngine&) = delete;
    FxEngine& operator=(const FxEngine&) = delete;

    // --- Message thread ------------------------------------------------------

    /// Allocates and prepares the active engine for `params` at `hostRate`
    /// (RealtimeStretcher::prepare preallocates its 30 s history ring). Computes
    /// and stores the reported latency. Resets the pending/graveyard state.
    void prepare(double hostRate, int maxBlockFrames, std::size_t numChannels,
                 const mws::engine::ParamSnapshot& params)
    {
        hostRate_ = hostRate;
        maxBlock_ = maxBlockFrames;
        channels_ = numChannels;
        activeKey_ = keyOf(params);

        auto prepared = buildPrepared(params);
        latencyHost_.store(prepared->stretcher.latencySamples(),
                           std::memory_order_release);

        // prepare() is prepareToPlay only — the audio thread is stopped — so the
        // freshly built engine becomes the active one directly (no handoff race),
        // and modelRate()/clampActive() reflect it immediately. The pending slot
        // and graveyard stay clear; only requestReconfigure() (audio running)
        // uses the publish/adopt handoff. The dirty flag is NOT set here:
        // prepareToPlay reports the latency unconditionally.
        std::atomic_store_explicit(&pending_, std::shared_ptr<PreparedFx>{},
                                   std::memory_order_release);
        active_ = std::move(prepared);
        prepared_.store(true, std::memory_order_release);
    }

    /// Whether prepare() has run (and the FX path is usable).
    [[nodiscard]] bool isPrepared() const noexcept
    {
        return prepared_.load(std::memory_order_acquire);
    }

    /// Message thread: react to a parameter snapshot. If a non-automatable,
    /// latency-relevant input (model / bandwidth / FS / character) changed since
    /// the last configuration, build a brand-new prepared RealtimeStretcher (the
    /// heavy 30 s-ring allocation happens HERE) and publish it for the audio
    /// thread to adopt at the next block; recompute and re-report the latency.
    /// Returns true iff a reconfiguration (and latency re-report) was triggered.
    /// timeFactor / cycleLen / hopMode automation never reaches this path.
    bool requestReconfigure(const mws::engine::ParamSnapshot& params)
    {
        if (!prepared_.load(std::memory_order_acquire))
            return false;
        const auto key = keyOf(params);
        if (key == activeKey_)
            return false;

        activeKey_ = key;
        auto prepared = buildPrepared(params);
        latencyHost_.store(prepared->stretcher.latencySamples(),
                           std::memory_order_release);
        latencyDirty_.store(true, std::memory_order_release);

        std::atomic_store_explicit(&pending_, std::move(prepared),
                                   std::memory_order_release);
        return true;
    }

    /// Reported FX latency in HOST samples (dsp-engine.md §7.4 formula, computed
    /// by RealtimeStretcher::prepare). Message thread.
    [[nodiscard]] int latencySamples() const noexcept
    {
        return latencyHost_.load(std::memory_order_acquire);
    }

    /// Message thread: true once since the last clearLatencyDirty(); set by
    /// prepare()/requestReconfigure() so the processor knows to call
    /// setLatencySamples(). One-shot consume via consumeLatencyDirty().
    [[nodiscard]] bool consumeLatencyDirty() noexcept
    {
        return latencyDirty_.exchange(false, std::memory_order_acq_rel);
    }

    /// Message thread: free retired engines HERE (never on the audio thread).
    /// Returns the number collected.
    std::size_t collectGarbage() noexcept { return graveyard_.collectGarbage(); }

    [[nodiscard]] bool hasGarbage() const noexcept { return graveyard_.hasGarbage(); }

    // --- Audio thread (RT-safe: no alloc, no lock, no free) ------------------

    /// One FX block. `inputs`/`outputs` are `numChannels` views of `numFrames`
    /// (<= the prepared maxBlock); in-place (outputs aliasing inputs) is allowed.
    /// `params` is the per-block snapshot (already loaded from the APVTS atomics
    /// by the caller); `transport` is the host transport for SYNC mode.
    ///
    /// Adopts any pending reconfiguration first (swap-in + graveyard retire of
    /// the old engine), stages the automatable params (applied at the next grain
    /// boundary, ADR-006), runs RealtimeStretcher::process, then applies outTrim.
    void processBlock(const mws::core::ConstAudioView* inputs,
                      mws::core::AudioView* outputs, std::size_t numChannels,
                      const mws::engine::ParamSnapshot& params,
                      const mws::engine::RealtimeStretcher::TransportInfo& transport) noexcept
    {
        // (1) Adopt a pending reconfiguration, if any. atomic_exchange the
        // pending slot to null so we adopt each publication exactly once; retire
        // the engine we were running to the graveyard (freed on the message
        // thread). Wait-free; allocates nothing.
        std::shared_ptr<PreparedFx> incoming = std::atomic_exchange_explicit(
            &pending_, std::shared_ptr<PreparedFx>{}, std::memory_order_acq_rel);
        if (incoming)
        {
            if (active_)
                graveyard_.retire(active_); // never freed here
            active_ = std::move(incoming);
        }

        if (!active_)
        {
            // Not prepared yet (no engine adopted): pass dry.
            passDry(inputs, outputs, numChannels);
            return;
        }

        // (2) Build the effective snapshot: automatable fields from the live
        // block snapshot, non-automatable (model/bandwidth/FS/character) from the
        // ACTIVE engine's own config — so setParams never sees a model it was not
        // prepared for (a transient APVTS/engine mismatch during reconfiguration).
        mws::engine::ParamSnapshot effective = params;
        effective.model = active_->config.model;
        effective.bandwidth = active_->config.bandwidth;
        effective.sampleRateSel = active_->config.sampleRateSel;
        effective.character = active_->config.character;

        // (3) Stage the per-block automatable snapshot (grain-boundary applied).
        active_->stretcher.setParams(effective);

        // (4) Transport for SYNC mode (ignored in FREE; cheap).
        active_->stretcher.setTransport(transport);

        // (5) Stretch (FREE + SYNC, dual-mono shared schedule).
        active_->stretcher.process(inputs, outputs, numChannels);

        // (6) outTrim output gain (dB → linear; 0 dB short-circuits).
        applyOutTrim(outputs, numChannels, params.outTrim);
    }

    /// The model rate the active engine runs at (host rate when character OFF).
    /// Audio/test thread; valid after the first adopt.
    [[nodiscard]] double modelRate() const noexcept
    {
        return active_ ? active_->stretcher.modelRate() : 0.0;
    }

    /// True while the ADR-006 FREE causality clamp is engaged on the active
    /// engine (LCD `FX MIN 100%`). Audio/message thread; valid after adopt.
    [[nodiscard]] bool clampActive() const noexcept
    {
        return active_ && active_->stretcher.clampActive();
    }

    /// LCD mono-sum flag (dsp-engine.md §5): the S900/S950 are mono machines, so
    /// a multi-channel FX input is summed to mono in the character chain (when
    /// CHARACTER is ON) and the LCD states it. Plumbed here so the LCD page model
    /// (task 041) can read it; reflects the active engine's model + character +
    /// the prepared channel count. (The actual mono-sum runs in the streaming
    /// character chain — see the CHARACTER-ON deviation in the file header.)
    [[nodiscard]] bool monoSummed() const noexcept
    {
        if (!active_)
            return false;
        const auto& cfg = active_->config;
        if (!cfg.character || channels_ <= 1)
            return false;
        return mws::model::ModelSpec::get(cfg.model).monoSum;
    }

private:
    /// Message thread: allocate + prepare a PreparedFx for `params` (the 30 s
    /// history-ring allocation happens here, off the audio thread).
    [[nodiscard]] std::shared_ptr<PreparedFx>
    buildPrepared(const mws::engine::ParamSnapshot& params)
    {
        auto prepared = std::make_shared<PreparedFx>();
        prepared->config = params;
        prepared->stretcher.prepare(hostRate_, maxBlock_, channels_, params);
        return prepared;
    }

    static void passDry(const mws::core::ConstAudioView* inputs,
                        mws::core::AudioView* outputs,
                        std::size_t numChannels) noexcept
    {
        for (std::size_t ch = 0; ch < numChannels; ++ch)
        {
            const auto n = outputs[ch].size();
            const auto m = inputs[ch].size();
            for (std::size_t i = 0; i < n; ++i)
                outputs[ch][i] = i < m ? inputs[ch][i] : 0.0f;
        }
    }

    static void applyOutTrim(mws::core::AudioView* outputs, std::size_t numChannels,
                             double trimDb) noexcept
    {
        if (trimDb == 0.0)
            return; // bit-exact passthrough at 0 dB (keeps the null test exact)
        const auto gain = static_cast<float>(std::pow(10.0, trimDb / 20.0));
        for (std::size_t ch = 0; ch < numChannels; ++ch)
            for (float& s : outputs[ch])
                s *= gain;
    }

    // prepare()-time configuration (message thread).
    double hostRate_ = 0.0;
    int maxBlock_ = 0;
    std::size_t channels_ = 0;
    ConfigKey activeKey_{}; // last configured non-automatable key (message thread)

    // Reported latency + a one-shot dirty flag the processor consumes.
    std::atomic<int> latencyHost_{ 0 };
    std::atomic<bool> latencyDirty_{ false };
    std::atomic<bool> prepared_{ false };

    // The message→audio handoff: the message thread stores a freshly prepared
    // engine here; the audio thread atomic_exchanges it to null and adopts it.
    std::shared_ptr<PreparedFx> pending_{};

    // The engine the audio thread is currently running (audio-thread owned once
    // adopted; never touched by the message thread).
    std::shared_ptr<PreparedFx> active_{};

    // Retired engines wait here for the message thread to free them. The
    // graveyard takes shared_ptr<const T>; engines are const-cast on the way in
    // (the graveyard only holds a refcount until collectGarbage() drops it).
    class EngineGraveyard
    {
    public:
        bool retire(std::shared_ptr<PreparedFx>& ptr) noexcept
        {
            if (!ptr)
                return true;
            std::shared_ptr<const PreparedFx> dead =
                std::const_pointer_cast<const PreparedFx>(std::move(ptr));
            const bool ok = fifo_.push(std::move(dead));
            ptr.reset();
            return ok;
        }
        std::size_t collectGarbage() noexcept
        {
            std::size_t n = 0;
            while (fifo_.pop())
                ++n;
            return n;
        }
        [[nodiscard]] bool hasGarbage() const noexcept { return fifo_.hasPending(); }

    private:
        // Generous: model switching is a rare UI action; a handful of in-flight
        // retired engines at most before the 30 Hz UI timer drains them.
        GraveyardFifo<PreparedFx> fifo_{ 64 };
    };

    EngineGraveyard graveyard_{};
};

} // namespace mws::plugin
