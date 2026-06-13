// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// RealtimeStretcher — the streaming FX front-end over the ONE shared two-grain
// scheduler (plan/decisions/006-fx-vs-sample-mode.md option C; dsp-engine.md
// §3.5; architecture.md §5.2). Task 022 implements the FREE-mode causality
// contract; task 023 adds SYNC window mode (below); the JUCE
// processBlock/character wiring is task 033.
//
// SYNC-mode contract (ADR-006 SYNC column; dsp-engine.md §3.5 SYNC bullet;
// task 023), engaged when ParamSnapshot::tempoSync == TempoSync::Host:
//   - The host transport is supplied per block via setTransport(TransportInfo)
//     — plain data, no JUCE here (the AudioPlayHead glue is task 033/037).
//   - At every transport-aligned `fxWindow` window boundary (boundaries from
//     TempoMap, task 021) the read head HARD-RESYNCS to the start of the
//     just-captured window (writePos - windowLen) — "stretch the last bar".
//     The window length follows `fxWindow` (1/4…8 bars, default 1 bar).
//   - T > 100%: the captured window plays stretched and fills the whole next
//     window (the read head, advancing at < 1 sample/sample, never reaches the
//     write head before the boundary). T = 100%: a 1:1 replay of the window.
//   - T < 100% (allowed in SYNC, NOT clamped — unlike FREE): the captured
//     window plays COMPRESSED (read head advances at > 1 sample/sample),
//     finishes before the boundary, then the engine outputs exactly SILENCE to
//     the window boundary (PI; loop-fill is a possible later option).
//   - Resyncs occur ONLY at window boundaries — never mid-window. The FREE
//     history-exhaustion resync is superseded: in SYNC the window boundary IS
//     the resync rule, so a window never outruns its own captured content
//     (windowLen <= history by construction within the supported tempo range).
//   - Tempo changes mid-play: the next boundary is recomputed from the new
//     tempo on the next block (the window follows the host — testing-strategy
//     §6 REAPER row).
//   - No transport / not playing: a wall-clock window from TempoMap's fallback
//     (last-known tempo, else 120 BPM (PI)) — Standalone has no bar grid.
//   - SYNC reuses the same scheduler, geometry, latency and ring as FREE; only
//     the read-head reset policy differs.
//
// FREE-mode contract (ADR-006, normative):
//   - T = 100%: pure delay of exactly the reported latency (null-testable
//     with character OFF).
//   - T > 100%: the read head lags the write head (lag grows at 1 - 100/T per
//     output sample, unbounded over time); the lag is bounded by the 30 s
//     (PI) history — on exhaustion the read head jumps to writePos - latency
//     at the NEXT GRAIN BOUNDARY (documented audible resync).
//   - T < 100%: impossible as a pure insert (needs future input) — the engine
//     clamps effective T to 100% and raises clampActive() for the LCD
//     (`FX MIN 100%`; automation values are preserved upstream).
//   - Parameter changes apply at grain boundaries only — no smoothing inside
//     the authentic scheduler.
//   - S900 model (ADR-003, dsp-engine.md §6): varispeed ZOH read head on the
//     same history ring, rate = 100/T clamped <= 1 in FREE (rate > 1 consumes
//     input faster than it arrives — same causality, same fix). Transpose is
//     NOT consumed here: it modulates the virtual DAC clock in the §8.1
//     character chain at playback (task 016/033).
//
// Processing domain (dsp-engine.md §3.5 host-rate rule): `cycleLen` is in
// MODEL-RATE samples and process() streams MODEL-RATE audio — the FX chain
// host (task 033) runs the ingest character (host -> model rate) before this
// engine and the playback character (model -> host rate) after it, so the
// sound matches the offline render at any host rate. With character OFF the
// model rate IS the host rate (CharacterChain §8.4) and the engine runs
// directly on host audio — the basis of the null tests. The history ring and
// the read-head delay are therefore model-domain quantities; latencySamples()
// reports the host-domain figure.
//
// Latency (dsp-engine.md §7.4; architecture.md §5.2), in HOST samples:
//
//   L = ceil(2000 * hostRate / modelRate)   worst-case cycle, host domain
//     + crossfadeLen                        = ceil(fadeMax * hostRate /
//                                             modelRate), the worst-case
//                                             cycle's crossfade in host
//                                             samples (fadeMax = 2000 -
//                                             ovStart(2000), 400 at the
//                                             default SpliceCal)
//     + SRC group delay                     = 0 when modelRate == hostRate,
//                                             else ceil of the host<->model
//                                             round trip through the §7.2
//                                             SincResampler (ingest delay
//                                             expressed in host samples +
//                                             playback delay)
//
// L depends only on model/bandwidth/FS (all non-automatable) and the host
// rate: recomputed in prepare() only, NEVER by setParams() — cycle-length and
// time-factor automation cannot change PDC. The internal model-domain read
// delay is D = 2000 + fadeMax (= L when character is OFF), which keeps every
// scheduled read causal and gives the resync rule its landing point
// (writePos - latency, model domain).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/stretch/CyclicEngine.h"
#include "mws/stretch/detail/TwoGrainScheduler.h"

namespace mws::engine {

class RealtimeStretcher
{
public:
    /// History ring span in seconds at model rate (PI — product constant,
    /// architecture.md §5.2; changing it is state-compatible but needs
    /// release notes).
    static constexpr double kHistorySeconds = 30.0;

    /// Worst-case cycle length in model-rate samples (superset max — the
    /// latency formula's "2000", dsp-engine.md §7.4).
    static constexpr std::int64_t kMaxCycleLen = 2000;

    RealtimeStretcher() = default;

    /// Splice calibration shared with the offline engine (dsp-engine.md §3.1
    /// — retuning is a data change, never an engine change).
    explicit RealtimeStretcher(stretch::CyclicEngine::SpliceCal cal) noexcept
        : cal_(cal)
    {
    }

    /// Preallocates the 30 s (PI) history ring (numChannels x ceil(30 s at
    /// model rate)) and all grain state, computes the latency, and resets the
    /// stream. `params` is clamped through ModelSpec internally (the single
    /// clamping authority). Everything latency-relevant (model, bandwidth,
    /// FS, character, host rate) is captured HERE — setParams() cannot change
    /// it (those inputs are non-automatable; a change requires re-prepare,
    /// wired by task 033's prepareToPlay).
    void prepare(double hostRate, int maxBlockFrames, std::size_t numChannels,
                 const ParamSnapshot& params);

    /// Streams one block: `inputs`/`outputs` are `numChannels` channel views
    /// of identical length <= maxBlockFrames, at MODEL rate (== host rate
    /// when character is OFF). Allocation-free and multichannel: dual-mono
    /// with ONE shared grain/hop schedule across channels (architecture.md
    /// §5.2 stereo rule; S900/S950 mono-summing happens upstream in the
    /// CharacterChain ingest). In-place processing (outputs aliasing inputs)
    /// is allowed — the block is written to the history ring before any
    /// output sample is rendered.
    void process(const core::ConstAudioView* inputs, core::AudioView* outputs,
                 std::size_t numChannels);

    /// Stages a parameter change; it takes effect at the NEXT grain boundary
    /// (ADR-006 — never mid-grain, no smoothing). Allocation-free. Only the
    /// automatable stretch parameters are consumed (timeFactor, cycleLen,
    /// hopMode); model/bandwidth/FS/character are prepare()-time inputs and
    /// are ignored here (debug-asserted unchanged).
    void setParams(const ParamSnapshot& params) noexcept;

    /// Reported FX latency in HOST samples (formula above). Constant between
    /// prepare() calls by contract.
    [[nodiscard]] int latencySamples() const noexcept { return latencyHost_; }

    /// The pure read-head delay the scheduler ACTUALLY realizes at T=100%, in
    /// STREAM samples (= the internal model-domain delay D = 2000 + fadeMax;
    /// the read head starts at -D, so process() is a D-sample pure delay). When
    /// the stream rate equals the model rate (character OFF, the null-test
    /// path) this equals latencySamples(). It diverges from latencySamples()
    /// ONLY in the task-033 character-ON deviation, where the FX glue feeds
    /// HOST-rate audio straight in (no host<->model resampler exists yet, see
    /// FxEngine.h DEVIATION + plan/backlog/053): there the stream is host-rate,
    /// so the realized delay is D host samples and latencySamples()'s SRC
    /// group-delay + model-rate cycle scaling are never applied. FxEngine
    /// reports THIS figure for that path so PDC stays honest (task 053 removes
    /// the divergence by adding the streaming chain). Constant between
    /// prepare() calls by contract.
    [[nodiscard]] int realizedDelaySamples() const noexcept
    {
        return static_cast<int>(delayModel_);
    }

    /// True while the ADR-006 FREE causality clamp is engaged — T < 100% on
    /// the cyclic models, varispeed rate > 1 (T < 100%) on the S900. The LCD
    /// shows `FX MIN 100%` (task 041); automation values are preserved.
    [[nodiscard]] bool clampActive() const noexcept { return clampActive_; }

    /// The model rate captured at prepare() (CharacterChain::modelRateFor —
    /// host rate when character is OFF).
    [[nodiscard]] double modelRate() const noexcept { return modelRate_; }

    /// History ring length in model-rate samples (= ceil(30 s * modelRate)).
    [[nodiscard]] std::int64_t historyLengthSamples() const noexcept
    {
        return historyLen_;
    }

    /// Total input samples written to the history ring since prepare() — the
    /// stream-domain write position (test instrumentation for the resync
    /// contract: the jump lands at samplesWritten() - latency).
    [[nodiscard]] std::int64_t samplesWritten() const noexcept { return written_; }

    /// Task-012 schedule-observation hook, streamed: fires once per grain
    /// launch with the GLOBAL output index and the stream-domain source
    /// offset (negative during the initial pre-roll: the read head starts at
    /// -latency). The resync jump is observable as a launch whose offset
    /// breaks the hop progression. Not owned; pass nullptr to detach. The
    /// default (nullptr) path performs no observation and the observer never
    /// alters the output (testing-strategy.md §3.6).
    void setGrainLaunchObserver(const stretch::GrainLaunchObserver* observer) noexcept
    {
        observer_ = observer;
    }

    // --- SYNC mode (task 023) ----------------------------------------------

    /// Host transport snapshot consumed per block in SYNC mode. Plain data —
    /// the JUCE AudioPlayHead glue that fills it is task 033/037. `playing`
    /// false (or a non-positive `bpm`) means "no transport": SYNC falls back to
    /// the TempoMap wall-clock window (last-known tempo, else 120 BPM (PI)).
    struct TransportInfo
    {
        bool playing = false;
        double ppqPosition = 0.0;   ///< quarter-note position of the block start
        double bpm = 0.0;           ///< host tempo; <= 0 ⇒ unknown
        int timeSigNumerator = 4;   ///< 3/4 vs 4/4 change the bar length
        int timeSigDenominator = 4;
    };

    /// Updates the transport for the NEXT process() call (SYNC mode only;
    /// ignored when tempoSync == Off). Allocation-free; call once per block
    /// before process(). A positive `bpm` is remembered as the last-known
    /// tempo for the no-transport wall-clock fallback.
    void setTransport(const TransportInfo& transport) noexcept;

    /// A SYNC window-boundary hard resync, reported through the optional
    /// observation hook (test instrumentation, mirrors the §3.6 grain-launch
    /// hook: default nullptr observes nothing and never alters the output).
    struct SyncResync
    {
        std::int64_t outIndex = 0;       ///< global output sample of the resync
        std::int64_t windowLenModel = 0; ///< window length, model-rate samples
        double windowStartPpq = 0.0;     ///< transport window start (0 in fallback)
    };
    using SyncResyncObserver = std::function<void(const SyncResync&)>;

    /// Observe SYNC window-boundary resyncs (test-only). Not owned; nullptr
    /// detaches. MUST NOT alter the rendered output (testing-strategy §3.6).
    void setSyncResyncObserver(const SyncResyncObserver* observer) noexcept
    {
        resyncObserver_ = observer;
    }

private:
    void applyParams(const ParamSnapshot& raw) noexcept;
    void processCyclic(core::AudioView* outputs, std::size_t numChannels,
                       std::size_t numFrames);
    void processVarispeed(core::AudioView* outputs, std::size_t numChannels,
                          std::size_t numFrames);

    /// SYNC (task 023): recompute the window length and the model-frame
    /// countdown to the next transport-aligned boundary for the block about to
    /// render (uses TempoMap; falls back to a free-running wall-clock window
    /// when the transport is absent). No-op in FREE mode. Allocation-free.
    void beginSyncBlock() noexcept;

    /// SYNC (task 023): hard-resync the read head to the start of the
    /// just-captured window (writePos - windowLen) and report it. Applies any
    /// staged parameter change (the boundary is the param-change point) and
    /// pins the readable region to the captured window so T<100% compression
    /// runs out into silence instead of bleeding the next window.
    void syncResyncReadHead() noexcept;

    /// Stream-position read with the streaming edge rules: positions before
    /// the stream start return 0 (pre-roll), positions at/beyond the readable
    /// ceiling return 0 (FREE: the write head; SYNC: the captured-window end,
    /// so compression decays to silence — task 023).
    [[nodiscard]] float ringAt(std::size_t channel, std::int64_t pos) const noexcept;

    /// Fractional stream-position read — the REVISED 2-point interpolation
    /// over the ring (zero fraction short-circuits to the verbatim sample, so
    /// CLASSIC's integer-valued positions stay bit-exact verbatim copies).
    [[nodiscard]] float ringRead(std::size_t channel, double pos) const noexcept;

    stretch::CyclicEngine::SpliceCal cal_{};

    // --- prepare()-time state ----------------------------------------------
    core::AudioBuffer history_;     ///< numChannels x historyLen_ ring
    std::int64_t historyLen_ = 0;   ///< model samples
    double hostRate_ = 0.0;
    double modelRate_ = 0.0;
    int maxBlock_ = 0;
    std::size_t channels_ = 0;
    int latencyHost_ = 0;           ///< reported latency, host samples
    std::int64_t delayModel_ = 0;   ///< internal read-head delay D, model samples
    std::int64_t resyncMargin_ = 0; ///< exhaustion-check margin (= D)
    bool varispeed_ = false;        ///< S900 path (ADR-003)
    ParamSnapshot active_{};        ///< last applied (raw) snapshot

    // --- stream state -------------------------------------------------------
    std::int64_t written_ = 0;  ///< input samples absorbed (write head)
    std::int64_t produced_ = 0; ///< output samples emitted (global out index)

    /// Readable ceiling: positions at/beyond it read as 0. FREE pins it to the
    /// write head each block (positions past the write head are pre-roll/edge
    /// silence); SYNC pins it to the captured-window end so compression decays
    /// to silence to the boundary instead of bleeding the next window.
    std::int64_t readCeil_ = 0;

    // --- cyclic scheduler (shared implementation, task 022 "no fork") -------
    // The streaming front-end always drives the RevisedGrainPolicy scheduler;
    // CLASSIC is served through it with integer-VALUED hops (same integer
    // rounding as the offline CLASSIC schedule), so every read carries a zero
    // fraction and short-circuits to the verbatim sample — bit-identical to
    // the fixed-point path, and hopMode changes at grain boundaries need no
    // scheduler swap.
    stretch::detail::TwoGrainScheduler<stretch::detail::RevisedGrainPolicy> sched_;
    stretch::detail::GrainGeometry geom_{};
    double hopIn_ = 0.0;

    // --- S900 varispeed read head (ADR-003) ---------------------------------
    double readPos_ = 0.0;
    double readRate_ = 1.0; ///< 100/T, clamped <= 1 in FREE

    // --- grain-boundary parameter staging (ADR-006) -------------------------
    ParamSnapshot pending_{};
    bool hasPending_ = false;
    bool clampActive_ = false;
    bool initialLaunchPending_ = false; ///< report prepare()'s grain on first block

    // --- SYNC mode (task 023) ----------------------------------------------
    bool syncActive_ = false;        ///< tempoSync == Host (captured at applyParams)
    TransportInfo transport_{};      ///< last setTransport() snapshot
    double lastKnownBpm_ = 0.0;      ///< most recent positive host tempo (fallback)
    std::int64_t windowLenModel_ = 0;       ///< current window length, model frames
    std::int64_t syncFramesToBoundary_ = 0; ///< model frames until the next boundary
    double syncWindowStartPpq_ = 0.0;       ///< current window start (transport)
    double nextBoundaryPpq_ = 0.0;          ///< next transport boundary to resync at
    double ppqPerFrameModel_ = 0.0;         ///< ppq advance per rendered model frame
    bool syncFallbackArmed_ = false;        ///< wall-clock countdown initialized
    bool syncTransportStarted_ = false;     ///< first transport block consumed
    bool syncFirstResyncDone_ = false;      ///< first window boundary reached

    const stretch::GrainLaunchObserver* observer_ = nullptr;
    const SyncResyncObserver* resyncObserver_ = nullptr;
};

} // namespace mws::engine
