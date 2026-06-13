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

    /// Enqueue a render request (message thread). Returns the assigned request
    /// id (monotone), or 0 if the request FIFO is momentarily full. The worker
    /// wakes and services it; progress and the final outcome arrive on the UI
    /// FIFO; on success a RenderedSample is published.
    std::uint64_t requestRender(mws::engine::ParamSnapshot params);

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

    /// Test accessor: the FX engine wrapper (model rate / clamp flag / dirty).
    [[nodiscard]] FxEngine& fxEngine() noexcept { return fx_; }

private:
    /// A queued render request (POD). The source buffer is NOT carried here —
    /// the worker reads the latest published source — so the request stays
    /// trivially copyable through the lock-free FIFO.
    struct RenderRequest {
        std::uint64_t id = 0;
        mws::engine::ParamSnapshot params{};
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

    // Scratch channel-view arrays so processFxBlock allocates nothing per block
    // (sized to the prepared channel count in prepareFx).
    std::vector<mws::core::ConstAudioView> fxIns_{};
    std::vector<mws::core::AudioView> fxOuts_{};

    // The FX input-scope FIFO (heap so EngineHost.h need not include the GUI
    // header that declares ScopeFifo). Allocated in prepareFx (message thread).
    std::unique_ptr<mws::ui::waveform::ScopeFifo> scopeFifo_{};
};

} // namespace mws::plugin
