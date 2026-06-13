// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// EngineHost (threading half — task 030; FX wiring is task 033).
//
// The plugin's threading backbone (docs/design/architecture.md §4):
//   - RenderWorker: ONE worker thread (juce::Thread) that consumes render
//     requests from a lock-free request FIFO, runs OfflineRenderer::render with
//     progress posts onto a UI-feedback FIFO and an atomic abort flag (hold-F8
//     semantics arrive in the UI tasks; here it is a plain atomic), and
//     publishes std::shared_ptr<const RenderedSample> on completion.
//   - Publication protocol (architecture.md §4): published render results are
//     immutable; the audio thread copies the shared_ptr once per block via the
//     shared Published<RenderedSample> mechanism (Published.h) and returns it to
//     the graveyard; deallocation NEVER on the audio thread. The render result,
//     the loaded sample (031), and the FX history reconfig (033) ALL use the
//     same one Published<T> — no ad-hoc atomics (task 030 acceptance criterion).
//   - Refusal (render cap) and abort surface as TYPED enums on the UI FIFO
//     (LCD strings are the UI layer's job — architecture.md §9; this layer
//     passes enums only).
//
// This file is the only place these threads are owned. The audio thread's
// per-block buffer access is acquireForAudioBlock()/retireFromAudioBlock();
// it allocates nothing, takes no lock, and never frees a buffer.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h> // juce::AbstractFifo
#include <juce_core/juce_core.h>                  // juce::Thread, juce::WaitableEvent

#include "mws/core/Buffer.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/engine/RealtimeStretcher.h"

#include "FxEngine.h"
#include "Published.h"

namespace mws::ui::waveform {
class ScopeFifo; // FX input-scope feed; declared in plugin/ui/WaveformView.h (consumer 043/045b)
}

namespace mws::plugin {

class SamplePlayer; // SAMPLE-mode playback voice (SamplePlayer.h includes this
                    // header for RenderedSample, so it is forward-declared here
                    // and held by unique_ptr to break the cycle).
enum class AuditionSource : std::uint8_t; // A (original) / B (render)

/// An immutable, published render result. Once published it is never mutated;
/// the audio thread holds a shared_ptr<const RenderedSample> for the duration
/// of a block (architecture.md §4 ownership/publication protocol).
struct RenderedSample {
    mws::core::AudioBuffer audio;     ///< the rendered audio (immutable once published)
    mws::engine::RenderInfo info{};   ///< metadata for the LCD / state blob
    std::uint64_t requestId = 0;      ///< echoes the originating request
};

/// Why a render finished, posted on the UI FIFO as a typed enum (no LCD strings
/// at this layer — architecture.md §9). Mirrors mws::engine::RenderError plus
/// the success case.
enum class RenderOutcome : std::uint8_t {
    Completed,        ///< published a RenderedSample
    NotEnoughMemory,  ///< over the 10-min model-rate cap; nothing published
    Aborted,          ///< abort flag was raised mid-render; nothing published
    UnsupportedModel, ///< reserved model slot (S3000 at v1)
};

/// One event posted from the render worker onto the UI-feedback FIFO and drained
/// on the message thread (timer poll). POD — copied by value through the FIFO.
struct WorkerEvent {
    enum class Kind : std::uint8_t { Started, Progress, Finished };

    Kind kind = Kind::Progress;
    std::uint64_t requestId = 0;

    /// Progress in [0, 1] for Kind::Progress (else unused).
    float progress = 0.0f;

    /// Outcome for Kind::Finished (else Completed).
    RenderOutcome outcome = RenderOutcome::Completed;
};

/// The threading backbone. Owns the render worker thread, the lock-free request
/// and UI-feedback FIFOs, and the shared Published<RenderedSample>.
///
/// Lifetime: construct on the message thread; startWorker() before use;
/// stopWorker() (or the destructor) joins the worker deterministically.
class EngineHost
{
public:
    /// FIFO capacities (PI): the request FIFO is shallow (the UI issues one
    /// render at a time, latest-wins via coalescing); the event FIFO must
    /// absorb a full render's progress posts between UI timer polls.
    static constexpr int kRequestCapacity = 8;
    static constexpr int kEventCapacity = 1024;

    EngineHost();
    ~EngineHost();

    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    // --- Message-thread API ---------------------------------------------------

    /// Set the source audio the worker renders from. Message thread only; the
    /// worker reads it only while servicing a request it dequeued AFTER this
    /// call (the request carries no buffer — the worker reads the latest source
    /// under a published snapshot). Stored as an immutable published pointer so
    /// it is itself handed off race-free (architecture.md §4).
    void setSource(std::shared_ptr<const mws::core::AudioBuffer> source);

    /// A render zone — the slice of the source the GO render covers, as
    /// normalized [0, 1] fractions of the source length (the state tree's
    /// zoneStart/zoneEnd fields, task 029). The worker converts them to source
    /// frames against the published source length. The full-source request is
    /// the default {0, 1} (start >= end renders nothing — an empty result).
    struct Zone {
        double start = 0.0;
        double end = 1.0;
    };

    /// Enqueue a render request (message thread). Returns the assigned request
    /// id (monotone), or 0 if the request FIFO is momentarily full. The worker
    /// wakes and services it; progress and the final outcome arrive on the UI
    /// FIFO; on success a RenderedSample is published. Renders the FULL source.
    std::uint64_t requestRender(mws::engine::ParamSnapshot params);

    /// GO render of a ZONE slice (ui-design.md §6.3 step 2): builds the request
    /// from `params` + the normalized `zone` and enqueues it. The worker slices
    /// the published source to the zone (off the audio thread) before running
    /// the OfflineRenderer, so the published render covers exactly the selection.
    /// Returns the request id (0 if the FIFO is momentarily full).
    std::uint64_t requestRender(mws::engine::ParamSnapshot params, Zone zone);

    /// Raise/lower the abort flag (message thread). Hold-F8 semantics arrive in
    /// the UI tasks; here it is the plain atomic the worker polls at stage
    /// boundaries. Cleared automatically at the start of each new request.
    void requestAbort() noexcept { abortFlag_.store(true, std::memory_order_release); }

    /// Drain one worker event from the UI FIFO (message thread, timer poll).
    /// Returns false when the FIFO is empty.
    bool popEvent(WorkerEvent& out) noexcept;

    /// Drain the publication graveyard, freeing retired render buffers HERE on
    /// the message thread (timer poll) — never on the audio thread. Returns the
    /// number of buffers collected.
    std::size_t collectGarbage() noexcept { return published_.collectGarbage(); }

    /// The currently published render result (message thread / tests).
    [[nodiscard]] std::shared_ptr<const RenderedSample> currentRender() const noexcept
    {
        return published_.current();
    }

    /// Start / stop the worker thread. stopWorker() joins deterministically.
    void startWorker();
    void stopWorker();

    // --- Audio-thread API (RT-safe: no alloc, no lock, no free) ---------------

    /// Audio thread: copy the published render pointer once for this block
    /// (RCU). Returns an empty pointer when nothing has been rendered yet.
    [[nodiscard]] std::shared_ptr<const RenderedSample> acquireForAudioBlock() const noexcept
    {
        return published_.acquire();
    }

    /// Audio thread: return the per-block pointer to the graveyard (never frees
    /// it inline). After this returns, `ptr` is empty.
    void retireFromAudioBlock(std::shared_ptr<const RenderedSample>& ptr) noexcept
    {
        (void) published_.retire(ptr);
    }

    // --- FX path (task 033) ---------------------------------------------------
    //
    // FX mode runs RealtimeStretcher (FREE + SYNC) directly in processBlock via
    // the FxEngine wrapper. prepareToPlay computes/reports the latency; the
    // message thread reconfigures the engine (and re-reports latency) when a
    // non-automatable, latency-relevant input (model/bandwidth/FS/character)
    // changes — through the same RCU/graveyard publication protocol as renders.

    /// Decimation stride for the FX input scope feed (architecture.md §4
    /// FIFO→timer-poll; ui-design §6.4). One in `kScopeDecimation` input samples
    /// (channel 0) is pushed — enough for a rolling scope without flooding the
    /// FIFO.
    static constexpr int kScopeDecimation = 8;

    /// prepareToPlay (message thread): allocate + prepare the FX engine for
    /// `params` at `hostRate`. Returns the reported FX latency in host samples
    /// (the caller passes it to setLatencySamples).
    int prepareFx(double hostRate, int maxBlockFrames, std::size_t numChannels,
                  const mws::engine::ParamSnapshot& params);

    /// Message thread (APVTS-change / timer poll): reconfigure the FX engine if a
    /// non-automatable, latency-relevant input changed. Returns true iff the
    /// latency was re-reported (so the processor calls setLatencySamples). No-op
    /// when only automatable params (timeFactor/cycleLen/hopMode) changed.
    bool reconfigureFxIfNeeded(const mws::engine::ParamSnapshot& params);

    /// Reported FX latency in host samples (dsp-engine.md §7.4). Message thread.
    [[nodiscard]] int fxLatencySamples() const noexcept { return fx_.latencySamples(); }

    /// Audio thread: process one FX block in place over the JUCE channel
    /// pointers. Snapshots params -> feeds transport -> RealtimeStretcher -> out
    /// trim, and pushes the decimated channel-0 input into the scope FIFO. Pass a
    /// transport for SYNC mode (FREE ignores it). Allocation/lock/IO-free.
    void processFxBlock(float* const* channelData, int numChannels, int numFrames,
                        const mws::engine::ParamSnapshot& params,
                        const mws::engine::RealtimeStretcher::TransportInfo& transport) noexcept;

    /// Message thread: free retired FX engines HERE (folded into the same timer
    /// poll that drains the render graveyard).
    std::size_t collectFxGarbage() noexcept { return fx_.collectGarbage(); }

    /// The FX-mode input-scope FIFO (the producer is processFxBlock; the consumer
    /// is WaveformView, wired in 043/045b). Owned here; non-owning pointer handed
    /// to the view. nullptr until prepareFx() has run.
    [[nodiscard]] mws::ui::waveform::ScopeFifo* scopeFifo() const noexcept
    {
        return scopeFifo_.get();
    }

    /// LCD feedback (task 041 consumer): the ADR-006 FREE causality clamp is
    /// engaged (`FX MIN 100%`).
    [[nodiscard]] bool fxClampActive() const noexcept { return fx_.clampActive(); }

    /// LCD feedback (task 041 consumer): the S900/S950 mono-sum flag
    /// (dsp-engine.md §5).
    [[nodiscard]] bool fxMonoSummed() const noexcept { return fx_.monoSummed(); }

    // --- Host tempo sync (task 037) -------------------------------------------
    //
    // When tempoSync == HOST in FX mode the effective time factor is derived
    // from sourceBPM + hostBPM via TempoMap and applied AT THE ENGINE (an
    // override on the per-block snapshot copy, never on the APVTS automation
    // value — architecture.md §6 clamp/override pattern). The host BPM comes
    // from the per-block transport (AudioPlayHead glue lives in the processor);
    // the last-known host tempo is retained so a momentary "no transport" block
    // keeps a sensible factor. The sourceBPM is plugin state with a setter API
    // and a filename `_174bpm`-style auto-guess that never clobbers a user value.

    /// The computed sync readout for the LCD model (task 041): the inputs and
    /// the resulting effective time factor (CLASSIC integer-quantized when the
    /// snapshot's hopMode is CLASSIC — dsp-engine.md §2). `active` is false
    /// unless tempoSync == HOST with a usable source + host BPM. Updated on the
    /// audio thread each FX block; read on the message thread (plain atomics).
    struct SyncReadout
    {
        bool active = false;        ///< tempoSync == HOST with usable BPMs
        double sourceBpm = 0.0;     ///< the source loop tempo
        double hostBpm = 0.0;       ///< the host tempo (or last-known fallback)
        double resultPercent = 0.0; ///< effective time factor the engine uses
    };

    /// Set the source loop BPM (message thread). `userSet` marks an explicit
    /// user/typed/tap value so a later filename auto-guess never clobbers it
    /// (overridable, ui-design §6.2 step 4). A non-positive value clears it
    /// (back to "unknown").
    void setSourceBpm(double bpm, bool userSet) noexcept;

    /// Parse a `_174bpm`-style tempo tag from a loaded file name (the regex
    /// `(\d+(?:\.\d+)?)bpm`, case-insensitive — ui-design §6.2 step 4) and adopt
    /// it as the source BPM, but ONLY when the user has not set a value (never
    /// clobbers). Returns true iff a tag was found AND adopted. Message thread.
    bool guessSourceBpmFromFilename(const std::string& fileName) noexcept;

    /// The current source BPM (0 == unknown). Message thread / tests.
    [[nodiscard]] double sourceBpm() const noexcept
    {
        return sourceBpm_.load(std::memory_order_acquire);
    }

    /// The computed SYNC readout (task 041 consumer). Message thread / tests.
    [[nodiscard]] SyncReadout fxSyncReadout() const noexcept;

    /// Test accessor: the FX engine wrapper (model rate / clamp flag / dirty).
    [[nodiscard]] FxEngine& fxEngine() noexcept { return fx_; }

    // --- SAMPLE path (task 034) -----------------------------------------------
    //
    // SAMPLE mode is the authentic GO -> render -> audition pipeline (ADR-006
    // SAMPLE-mode bullet; ui-design.md §6.3). PLAY/A-B drive the SamplePlayer
    // (audio thread plays the published RenderedSample (B) or the loaded source
    // (A)); the ZONE soft key loops the selected zone through a preview
    // RealtimeStretcher (CYCLIC models only). The worker render path
    // (requestRender) and its publication are shared with task 030 above.

    /// One-block crossfade length applied when switching process paths FX<->
    /// SAMPLE, in fraction of a block — a NAMED TUNABLE flagged for the PI audit
    /// (task 034b): the design specifies a one-block crossfade only for MODEL
    /// switches (ui-design.md §6.5); applying it to MODE switches is our
    /// invention. The fade covers exactly one block (the processor cross-fades
    /// the old path's last output against the new path's first output).
    static constexpr int kModeSwitchFadeBlocks = 1;

    /// prepareToPlay (message thread): prepare the SAMPLE-mode playback voice
    /// and the ZONE-preview stretcher for `hostRate`/`maxBlockFrames`/channels.
    /// Allocates here (off the audio thread). Idempotent across prepareToPlay.
    void prepareSample(double hostRate, int maxBlockFrames, std::size_t numChannels,
                       const mws::engine::ParamSnapshot& params);

    /// Set the ORIGINAL (A) audio for A/B audition (message thread). This is the
    /// loaded source the worker also renders from; published race-free. A
    /// convenience alias of setSource so the SAMPLE half reads one source.
    void setAuditionSource(std::shared_ptr<const mws::core::AudioBuffer> source)
    {
        setSource(std::move(source));
    }

    /// PLAY / stop the SAMPLE audition (message thread, F5 PLAY soft key).
    void startSamplePlayback() noexcept;
    void stopSamplePlayback() noexcept;
    [[nodiscard]] bool isSamplePlaying() const noexcept;

    /// A/B audition source toggle (message thread, F6 A/B soft key): play the
    /// published render (B) or the loaded original (A).
    void setAuditionSource(AuditionSource mode) noexcept;
    [[nodiscard]] AuditionSource auditionSource() const noexcept;

    /// Toggle the ZONE live preview on/off (message thread, F3 ZONE soft key).
    /// When ON, processSampleBlock loops the selected zone through the preview
    /// RealtimeStretcher at the current params (CYCLIC models only —
    /// ui-design.md §6.3; the S900 varispeed model has no stretch so ZONE
    /// preview is inert there and the call is a no-op for it). `zone` is the
    /// normalized selection from the state tree; `params` supplies the current
    /// stretch settings.
    void setZonePreview(bool enabled, Zone zone,
                        const mws::engine::ParamSnapshot& params) noexcept;
    [[nodiscard]] bool zonePreviewActive() const noexcept
    {
        return zonePreviewOn_.load(std::memory_order_acquire);
    }

    /// Audio thread: render one SAMPLE-mode block in place over the JUCE channel
    /// pointers. When ZONE preview is ON it loops the selected zone through the
    /// preview stretcher; otherwise it plays the audition buffer (PLAY/A-B)
    /// through the SamplePlayer. Either way outTrim is applied (dsp-engine.md §2
    /// OUTPUT "Applies to: all"). Acquires the published render (B) and source
    /// (A) once per block (RCU) and retires them. Allocation/lock/IO-free.
    void processSampleBlock(float* const* channelData, int numChannels,
                            int numFrames,
                            const mws::engine::ParamSnapshot& params) noexcept;

    /// Test accessor: the SAMPLE-mode playback voice.
    [[nodiscard]] SamplePlayer& samplePlayer() noexcept { return *player_; }

private:
    /// A queued render request (POD). The source buffer is NOT carried here —
    /// the worker reads the latest published source — so the request stays
    /// trivially copyable through the lock-free FIFO. `zoneStart`/`zoneEnd` are
    /// the normalized [0, 1] slice the worker crops the source to before
    /// rendering (full source = {0, 1}).
    struct RenderRequest {
        std::uint64_t id = 0;
        mws::engine::ParamSnapshot params{};
        double zoneStart = 0.0;
        double zoneEnd = 1.0;
    };

    /// The worker thread body: block on the wake event, drain the request FIFO
    /// (latest wins), run OfflineRenderer with progress/abort, publish on
    /// success, post the outcome.
    class Worker final : public juce::Thread
    {
    public:
        explicit Worker(EngineHost& host) : juce::Thread("mwStime render worker"), host_(host) {}
        void run() override;

        /// Public bridge to the protected juce::Thread::threadShouldExit().
        [[nodiscard]] bool shouldExit() const noexcept { return threadShouldExit(); }

    private:
        EngineHost& host_;
    };

    /// Worker-side: service one dequeued request end to end.
    void serviceRequest(const RenderRequest& req);

    /// Worker-side: push an advisory event onto the UI FIFO; drops the event if
    /// the FIFO is full (progress events are advisory). Returns whether it was
    /// accepted.
    bool pushEvent(const WorkerEvent& ev) noexcept;

    /// Worker-side: push the terminal Finished event; retries until accepted so
    /// the UI never misses a render's completion/abort/refusal outcome.
    void pushFinished(std::uint64_t requestId, RenderOutcome outcome) noexcept;

    /// Worker-side: whether the worker has been asked to exit (public bridge to
    /// juce::Thread::threadShouldExit, which is protected).
    [[nodiscard]] bool workerShouldExit() const noexcept;

    // Render-request FIFO (message thread -> worker).
    juce::AbstractFifo requestFifo_{ kRequestCapacity };
    RenderRequest requestSlots_[kRequestCapacity]{};

    // UI-feedback FIFO (worker -> message thread).
    juce::AbstractFifo eventFifo_{ kEventCapacity };
    WorkerEvent eventSlots_[kEventCapacity]{};

    // The ONE publication mechanism for the render result (architecture.md §4).
    Published<RenderedSample> published_{};

    // The source audio, itself published so setSource is race-free.
    Published<mws::core::AudioBuffer> source_{};

    mws::engine::OfflineRenderer renderer_{};

    std::atomic<bool> abortFlag_{ false };
    std::atomic<std::uint64_t> nextRequestId_{ 1 };

    juce::WaitableEvent wake_{};
    Worker worker_{ *this };

    // --- FX path (task 033) ---------------------------------------------------
    FxEngine fx_{};

    // --- Host tempo sync (task 037) -------------------------------------------
    // sourceBPM is plugin state (set on the message thread); the audio thread
    // reads it each block to compute the effective time factor. lastKnownHostBpm
    // is updated on the audio thread (last positive transport tempo) and read as
    // the fallback when a block reports no transport tempo. The readout cache is
    // written on the audio thread and read on the message thread (LCD poll).
    std::atomic<double> sourceBpm_{ 0.0 };       ///< source loop BPM (0 = unknown)
    std::atomic<bool> sourceBpmUserSet_{ false };///< explicit user/typed/tap value
    std::atomic<double> lastKnownHostBpm_{ 0.0 };///< last positive transport tempo

    std::atomic<bool> syncReadoutActive_{ false };
    std::atomic<double> syncReadoutSource_{ 0.0 };
    std::atomic<double> syncReadoutHost_{ 0.0 };
    std::atomic<double> syncReadoutResult_{ 0.0 };

    /// Audio thread: derive the effective time factor for a HOST-synced FX
    /// block and refresh the readout cache. Returns the snapshot with
    /// `timeFactor` overridden when sync is engaged (the input snapshot — and
    /// thus the APVTS automation value — is never mutated); otherwise returns
    /// `params` unchanged and marks the readout inactive.
    [[nodiscard]] mws::engine::ParamSnapshot applyTempoSync(
        const mws::engine::ParamSnapshot& params,
        const mws::engine::RealtimeStretcher::TransportInfo& transport) noexcept;

    // Scratch channel-view arrays so processFxBlock allocates nothing per block
    // (sized to the prepared channel count in prepareFx).
    std::vector<mws::core::ConstAudioView> fxIns_{};
    std::vector<mws::core::AudioView> fxOuts_{};

    // The FX input-scope FIFO (heap so EngineHost.h need not include the GUI
    // header that declares ScopeFifo). Allocated in prepareFx (message thread).
    std::unique_ptr<mws::ui::waveform::ScopeFifo> scopeFifo_{};

    // --- SAMPLE path (task 034) -----------------------------------------------
    //
    // The playback voice (heap so SamplePlayer.h, which includes this header for
    // RenderedSample, need only be included from EngineHost.cpp — breaks the
    // include cycle). Allocated in the constructor; never reallocated.
    std::unique_ptr<SamplePlayer> player_;

    // The ZONE live-preview stretcher (CYCLIC models only — ui-design.md §6.3).
    // Prepared on the message thread (its 30 s history ring allocates there);
    // driven on the audio thread by looping the selected zone of the source
    // through it. A scratch input buffer (sized in prepareSample) lets the loop
    // feed the stretcher without allocating per block.
    mws::engine::RealtimeStretcher zonePreview_{};
    mws::core::AudioBuffer zoneScratch_{}; // numChannels x maxBlock (message-thread sized)
    std::atomic<bool> zonePreviewOn_{ false };
    std::atomic<bool> zonePreviewSupported_{ false }; // false for the S900 (no stretch)
    // Normalized zone selection (message thread writes, audio thread reads —
    // plain doubles published atomically). The audio thread converts to source
    // frames against the per-block source length.
    std::atomic<double> zoneStartNorm_{ 0.0 };
    std::atomic<double> zoneEndNorm_{ 1.0 };
    // The looped read head into the zone, in SOURCE frames (audio-thread owned).
    double zoneReadPos_ = 0.0;

    // Per-block channel-view scratch for the SAMPLE path (sized in prepareSample
    // so processSampleBlock allocates nothing).
    std::vector<mws::core::ConstAudioView> sampleIns_{};
    std::vector<mws::core::AudioView> sampleOuts_{};

    double sampleHostRate_ = 0.0;
    int sampleMaxBlock_ = 0;
    std::size_t sampleChannels_ = 0;

    /// Apply an OUTPUT trim (dB) to a JUCE channel-pointer block in place; 0 dB
    /// short-circuits so verbatim playback stays bit-exact (dsp-engine.md §2).
    static void applyOutTrimChannels(float* const* channelData, std::size_t numChannels,
                                     std::size_t numFrames, double trimDb) noexcept;
};

} // namespace mws::plugin
