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
// DEVIATION (documented; plan/backlog/033 PR + follow-up task plan/backlog/053):
// the FX path runs RealtimeStretcher directly on the host-rate block. With
// CHARACTER OFF the model rate IS the host rate (CharacterChain §8.4), so this
// is exactly correct and null-testable (RealtimeStretcher.h §56-63) — the path
// every task-033 verification test exercises. With CHARACTER ON the
// dsp-engine.md §3.5 host-rate rule wants the ingest character (host -> model
// rate, 12/16-bit quantize) run BEFORE the stretcher and the playback character
// (model -> host) AFTER it. That needs an allocation-free STREAMING resampler
// with continuous cross-block phase (the per-block 12-bit/16-bit character
// chain). The mwstime-core only ships an OFFLINE allocating SincResampler
// (core/Resampler.h) — no RT-safe streaming variant exists — so a faithful
// streaming character chain is a separate engine-layer effort, NOT plugin glue.
// That effort is tracked in plan/backlog/053-fx-streaming-character-chain.md
// (the allocation-free streaming host<->model resampler + per-block 12/16-bit
// character chain). Until 053 lands, character-ON FX runs the stretcher at host
// rate and the SAMPLE-mode offline render remains the bit-faithful character
// path.
//
// HONEST PDC (review item 2a; superseded by 053): because no resampler runs in
// the character-ON deviation, the dsp-engine.md §7.4 SRC group-delay term and
// the model-rate cycle scaling in RealtimeStretcher::latencySamples() are NEVER
// realized by this path. Reporting them would over-report PDC by a delay the
// path does not apply (the original review finding). So for the character-ON
// deviation ONLY, FxEngine reports the scheduler's actually-realized read-head
// delay (RealtimeStretcher::realizedDelaySamples()) — see realizedLatencyOf().
// With character OFF (modelRate == hostRate, every null-tested path) that figure
// is identical to latencySamples(), so the contract null is untouched. When 053
// lands, the SRC term IS realized and realizedLatencyOf()'s deviation branch is
// deleted so latencySamples() is reported unconditionally.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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
/// run on the message thread; the LCD accessors clampActive()/monoSummed()/
/// modelRate() run on the message thread too (the 30 Hz UI timer); processBlock()
/// runs on the audio thread. The two sides communicate only through lock-free
/// atomics: the pending slot (message→audio), the active slot (audio-published,
/// message-read via std::atomic_load — task 055), the published LCD snapshot
/// (audio→message), and the graveyard. No plain cross-thread pointer reads.
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

    /// The message-thread-readable LCD state, published atomically by the audio
    /// thread (task 055; QA finding F2). The LCD accessors (clampActive() /
    /// monoSummed() / modelRate()) used to dereference the live, audio-thread-
    /// mutating `active_` shared_ptr and read RealtimeStretcher::clampActive_ (a
    /// plain bool rewritten every block) directly from the message thread — a
    /// use-after-free-class data race plus a torn-bool read. Instead the audio
    /// thread publishes this small trivially-copyable POD as ONE atomic store and
    /// the message thread loads a consistent snapshot; nothing on the message side
    /// touches `active_` or the stretcher's internals. modelRate/monoSummed change
    /// only on adopt; clampActive is restated every block — all three travel
    /// together so the LCD never shows a mismatched mix.
    ///
    /// SIZE CONTRACT (task 058): this POD must stay <= 8 bytes with <= 8-byte
    /// alignment so `std::atomic<LcdSnapshot>` is a 64-bit atomic — which is
    /// ALWAYS lock-free on every supported 64-bit target (x86-64 AND arm64),
    /// keeping processBlock lock-free portably (see the static_asserts at the
    /// `lcd_` member). A 16-byte snapshot (e.g. a `double` modelRate, which pads
    /// the struct to 16 bytes) is lock-free on macOS/clang-arm64 but NOT on
    /// Linux/GCC x86_64 (no guaranteed 128-bit lock-free atomic without -mcx16),
    /// so it would break the Linux build (the latent 055 bug 058 fixes). Hence
    /// modelRate is stored as `float`: all supported model/host rates are
    /// integers <= 2^24 (44100/22050/48000/96000/192000 and the S950 variable
    /// clock = 2500 x bandwidth), exactly representable in `float`, and the
    /// public accessor modelRate() widens back to `double` so no caller changes.
    /// The two bools then pack into the remaining 4 bytes => 8 bytes total.
    struct LcdSnapshot
    {
        float modelRate = 0.0f;
        bool clampActive = false;
        bool monoSummed = false;
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

        // Preallocate the model-switch cross-fade scratch (ui-design §6.5, task
        // 046): one block of the OUTGOING engine's output, blended against the
        // incoming engine's first block. Sized here, off the audio thread, so
        // processBlock's fade allocates nothing.
        fadeScratch_.resize(numChannels,
                            static_cast<std::size_t>(std::max(1, maxBlockFrames)));
        fadeViews_.assign(numChannels, mws::core::AudioView{});

        auto prepared = buildPrepared(params);
        latencyHost_.store(realizedLatencyOf(*prepared),
                           std::memory_order_release);

        // prepare() is prepareToPlay only — the audio thread is stopped — so the
        // freshly built engine becomes the active one directly (no handoff race),
        // and modelRate()/clampActive() reflect it immediately. The pending slot
        // and graveyard stay clear; only requestReconfigure() (audio running)
        // uses the publish/adopt handoff. The dirty flag is NOT set here:
        // prepareToPlay reports the latency unconditionally.
        std::atomic_store_explicit(&pending_, std::shared_ptr<PreparedFx>{},
                                   std::memory_order_release);
        publishLcd(*prepared); // initial LCD snapshot for the message thread
        std::atomic_store_explicit(&active_, std::move(prepared),
                                   std::memory_order_release);
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
        latencyHost_.store(realizedLatencyOf(*prepared),
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
        // The audio thread is the SOLE writer of `active_`; the message thread
        // only atomic_loads it (task 055). Load it ONCE into a local working
        // pointer (refcount bump, no alloc/lock), mutate the local freely, then
        // republish it with one atomic store at the end. This makes the handoff
        // race-free on BOTH sides — matching the `pending_` discipline — without
        // ever exposing a half-reassigned pointer to a concurrent LCD read.
        std::shared_ptr<PreparedFx> active = std::atomic_load_explicit(
            &active_, std::memory_order_acquire);

        // (1) Adopt a pending reconfiguration, if any. atomic_exchange the
        // pending slot to null so we adopt each publication exactly once. Wait-
        // free; allocates nothing. The OLD engine is NOT retired yet — when a
        // reconfiguration is adopted we run it once more for this block so its
        // output can be cross-faded against the incoming engine (ui-design §6.5
        // model-switch one-block fade, task 046); it is retired after the fade.
        std::shared_ptr<PreparedFx> incoming = std::atomic_exchange_explicit(
            &pending_, std::shared_ptr<PreparedFx>{}, std::memory_order_acq_rel);
        const bool reconfigured = static_cast<bool>(incoming);

        if (reconfigured && active
            && numChannels <= fadeScratch_.numChannels()
            && numChannels <= fadeViews_.size())
        {
            // (1a) Run the OUTGOING engine for this block into the fade scratch,
            // so its tail can be cross-faded against the incoming engine's first
            // block (no hard step when the fresh engine's history ring is empty).
            const std::size_t n = numChannels > 0 ? outputs[0].numFrames() : 0;
            for (std::size_t ch = 0; ch < numChannels; ++ch)
                fadeViews_[ch] = mws::core::AudioView{ fadeScratch_.channel(ch).data(), n };

            runEngine(*active, inputs, fadeViews_.data(), numChannels, params, transport);
            graveyard_.retire(active);        // outgoing engine freed on msg thread
            active = std::move(incoming);

            // (1b) Run the INCOMING engine in place, then a one-block linear
            // cross-fade: old fades out, new fades in. Click-free.
            runEngine(*active, inputs, outputs, numChannels, params, transport);
            for (std::size_t ch = 0; ch < numChannels; ++ch)
            {
                float* out = outputs[ch].data();
                const float* old = fadeScratch_.channel(ch).data();
                const std::size_t len = outputs[ch].numFrames();
                for (std::size_t i = 0; i < len; ++i)
                {
                    const float t = (len > 1)
                                        ? static_cast<float>(i) / static_cast<float>(len - 1)
                                        : 1.0f;
                    out[i] = old[i] * (1.0f - t) + out[i] * t;
                }
            }
            applyOutTrim(outputs, numChannels, params.outTrim);
            publishActive(active); // republish active_ + LCD snapshot atomically
            return;
        }

        if (incoming)
        {
            // Adopt without a fade (no prior active engine, or a degenerate
            // channel/scratch mismatch — fall back to the plain swap).
            if (active)
                graveyard_.retire(active);
            active = std::move(incoming);
        }

        if (!active)
        {
            // Not prepared yet (no engine adopted): pass dry. No active_ change.
            passDry(inputs, outputs, numChannels);
            return;
        }

        // (2..6) Run the active engine in place + outTrim.
        runEngine(*active, inputs, outputs, numChannels, params, transport);
        applyOutTrim(outputs, numChannels, params.outTrim);
        publishActive(active); // republish active_ + LCD snapshot atomically
    }

    /// The model rate the active engine runs at (host rate when character OFF).
    /// Message/audio/test thread; valid after the first adopt. Reads the
    /// atomically-published LCD snapshot — NOT the live `active_` pointer (task
    /// 055; QA F2). 0 before the first prepare()/adopt.
    [[nodiscard]] double modelRate() const noexcept
    {
        return std::atomic_load_explicit(&lcd_, std::memory_order_acquire)
            .modelRate;
    }

    /// True while the ADR-006 FREE causality clamp is engaged on the active
    /// engine (LCD `FX MIN 100%`). Message/audio thread; valid after adopt.
    /// Reads the atomically-published snapshot, so it never tears the bool the
    /// audio thread rewrites every block (task 055; QA F2).
    [[nodiscard]] bool clampActive() const noexcept
    {
        return std::atomic_load_explicit(&lcd_, std::memory_order_acquire)
            .clampActive;
    }

    /// LCD mono-sum flag (dsp-engine.md §5): the S900/S950 are mono machines, so
    /// a multi-channel FX input is summed to mono in the character chain (when
    /// CHARACTER is ON) and the LCD states it. Plumbed here so the LCD page model
    /// (task 041) can read it; reflects the active engine's model + character +
    /// the prepared channel count. (The actual mono-sum runs in the streaming
    /// character chain — see the CHARACTER-ON deviation in the file header.)
    /// Reads the atomically-published snapshot (task 055; QA F2).
    [[nodiscard]] bool monoSummed() const noexcept
    {
        return std::atomic_load_explicit(&lcd_, std::memory_order_acquire)
            .monoSummed;
    }

private:
    /// Compute the LCD snapshot for a prepared engine, given the prepared channel
    /// count. monoSummed mirrors the old live computation (mono machine + CHARACTER
    /// ON + multi-channel input); modelRate/clampActive come from the stretcher.
    /// `channels_` is set in prepare() (message thread) and is stable for the life
    /// of the stream, so reading it here on the audio thread is race-free.
    [[nodiscard]] LcdSnapshot computeLcd(const PreparedFx& engine) const noexcept
    {
        LcdSnapshot s;
        // modelRate is stored as float (the <= 8-byte lock-free size contract,
        // task 058). Every supported rate is an integer <= 2^24, exactly
        // representable in float, so this narrowing is lossless for the
        // null/equality path; the accessor widens it back to double.
        s.modelRate = static_cast<float>(engine.stretcher.modelRate());
        s.clampActive = engine.stretcher.clampActive();
        const auto& cfg = engine.config;
        s.monoSummed = cfg.character && channels_ > 1
                       && mws::model::ModelSpec::get(cfg.model).monoSum;
        return s;
    }

    /// Publish the LCD snapshot for `engine` as ONE atomic store (release) so the
    /// message-thread LCD accessors load a consistent, never-torn snapshot.
    void publishLcd(const PreparedFx& engine) noexcept
    {
        std::atomic_store_explicit(&lcd_, computeLcd(engine),
                                   std::memory_order_release);
    }

    /// Audio thread: republish the active engine pointer (release) AND its LCD
    /// snapshot. The LCD snapshot is published FIRST so the message thread can
    /// never observe a newer `active_` paired with a stale LCD reading; the LCD
    /// accessors read only the snapshot, never `active_`, so ordering here is for
    /// freshness, not correctness. Wait-free; no alloc/lock.
    void publishActive(std::shared_ptr<PreparedFx>& engine) noexcept
    {
        publishLcd(*engine);
        std::atomic_store_explicit(&active_, engine, std::memory_order_release);
    }

    /// Audio thread: run one prepared engine over the channel views (steps 2–5
    /// of the legacy processBlock body — effective-snapshot build, setParams,
    /// transport, stretch). outTrim is applied by the caller (so the cross-fade
    /// runs on the pre-trim outputs and trims the blended result once).
    static void runEngine(PreparedFx& engine, const mws::core::ConstAudioView* inputs,
                          mws::core::AudioView* outputs, std::size_t numChannels,
                          const mws::engine::ParamSnapshot& params,
                          const mws::engine::RealtimeStretcher::TransportInfo& transport) noexcept
    {
        // Effective snapshot: automatable fields from the live block, non-
        // automatable (model/bandwidth/FS/character) from THIS engine's own
        // config — so setParams never sees a model it was not prepared for.
        mws::engine::ParamSnapshot effective = params;
        effective.model = engine.config.model;
        effective.bandwidth = engine.config.bandwidth;
        effective.sampleRateSel = engine.config.sampleRateSel;
        effective.character = engine.config.character;

        engine.stretcher.setParams(effective);
        engine.stretcher.setTransport(transport);
        engine.stretcher.process(inputs, outputs, numChannels);
    }

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

    /// The latency the FX path ACTUALLY realizes, in host samples — honest PDC
    /// (review item 2a; task 033 PR). RealtimeStretcher::latencySamples() is the
    /// dsp-engine.md §7.4 formula for the INTENDED streaming chain: it scales
    /// the cycle/crossfade by hostRate/modelRate AND adds the §7.2 SRC
    /// group-delay round trip whenever modelRate != hostRate. That formula is
    /// correct for the offline render and for the streaming chain once it lands
    /// (plan/backlog/053). It is NOT correct for the current character-ON
    /// deviation (FxEngine.h DEVIATION header): there the FX glue feeds
    /// HOST-rate audio straight into a stretcher whose internal geometry is
    /// model-rate, with NO resampler in the path — so neither the model-rate
    /// cycle scaling nor the SRC group delay is ever realized. Reporting the
    /// full formula there would over-report PDC by a delay the path never
    /// applies (the original review finding). For that one case we report the
    /// scheduler's pure read-head delay (realizedDelaySamples(), = D stream
    /// samples = D host samples since the stream IS host-rate). With character
    /// OFF — every null-tested path, where modelRate == hostRate — the two are
    /// identical, so the honest figure leaves the contract null untouched.
    [[nodiscard]] int realizedLatencyOf(const PreparedFx& prepared) const noexcept
    {
        const auto& s = prepared.stretcher;
        // modelRate() != hostRate_ via the not-less-not-greater form: exact
        // (CharacterChain::modelRateFor returns hostRate_ verbatim when the
        // model rate matches), -Wfloat-equal-clean, and false for any NaN.
        const bool ratesDiffer =
            (s.modelRate() < hostRate_) || (hostRate_ < s.modelRate());
        const bool resamplesInPath = prepared.config.character && ratesDiffer;
        // (resamplesInPath is the character-ON deviation: model rate diverges
        //  from host rate but no resampler runs. Once task 053's streaming
        //  resampler lands, the path DOES realize the SRC term and this branch
        //  should be deleted so latencySamples() is reported unconditionally.)
        return resamplesInPath ? s.realizedDelaySamples() : s.latencySamples();
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

    // Model-switch one-block cross-fade scratch (ui-design §6.5, task 046):
    // the outgoing engine's block output, blended against the incoming engine's
    // first block. Sized in prepare() (off the audio thread); the view array
    // points into it each block. processBlock allocates nothing.
    mws::core::AudioBuffer fadeScratch_{};
    std::vector<mws::core::AudioView> fadeViews_{};

    // Reported latency + a one-shot dirty flag the processor consumes.
    std::atomic<int> latencyHost_{ 0 };
    std::atomic<bool> latencyDirty_{ false };
    std::atomic<bool> prepared_{ false };

    // The message→audio handoff: the message thread stores a freshly prepared
    // engine here; the audio thread atomic_exchanges it to null and adopts it.
    std::shared_ptr<PreparedFx> pending_{};

    // The engine the audio thread is currently running. The audio thread is its
    // SOLE writer (processBlock republishes it with one atomic store); the
    // message thread used to read it directly for the LCD accessors, which raced
    // the reassignment (QA finding F2 / task 055). All access — write and read —
    // now goes through std::atomic_store_explicit/atomic_load_explicit, the same
    // discipline pending_ already uses, and the message thread reads LCD state
    // from the published snapshot below rather than dereferencing this pointer.
    std::shared_ptr<PreparedFx> active_{};

    // The atomically-published LCD snapshot (task 055). The audio thread stores
    // it (one release store per adopt/block via publishLcd); the message-thread
    // LCD accessors load it. A small trivially-copyable POD so the store/load is
    // a single lock-free atomic op — keeping processBlock lock-free. The
    // static_asserts fail the build loudly on any target where the type would
    // need a lock (would violate the RT contract) rather than silently degrading.
    std::atomic<LcdSnapshot> lcd_{ LcdSnapshot{} };
    // The portable guard (task 058): an 8-byte naturally-aligned type is a 64-bit
    // atomic, which is ALWAYS lock-free on every supported 64-bit target (x86-64
    // AND arm64). This makes the lock-free guarantee PROVABLE without a Linux
    // host — it is what stops the 055 regression (a 16-byte snapshot was lock-free
    // on macOS/clang-arm64 but not on Linux/GCC x86_64, breaking the build).
    static_assert(sizeof(LcdSnapshot) <= 8,
                  "FxEngine LCD snapshot must be <= 8 bytes so std::atomic<> is a "
                  "64-bit atomic — always lock-free on x86-64 AND arm64; a 16-byte "
                  "snapshot is lock-free on macOS/clang but not Linux/GCC (task 058)");
    static_assert(std::atomic<LcdSnapshot>::is_always_lock_free,
                  "FxEngine LCD snapshot must be lock-free to keep processBlock "
                  "lock-free (task 055)");

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
