// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// OfflineRenderer — the hardware-faithful render-to-new-sample
// (docs/design/architecture.md §5.1; docs/design/dsp-engine.md §1, §7.2, §7.3;
// ADR-006 SAMPLE-mode workflow; task plan/backlog/020-offline-renderer.md).
//
// The five-stage authentic pipeline, in hardware order (architecture.md §5.1):
//   [1] ingest character     (CharacterChain::ingest — resample to model rate,
//                             quantize to model depth, S900/S950 mono sum)
//   [2] per-model stretch    (S900 RepitchEngine; S950 S950Engine;
//                             S1000/S1100 CyclicEngine CLASSIC/REVISED)
//   [3] transpose            (routed by Transpose::usesSincTranspose:
//                             fixed-rate models = windowed sinc; varclock
//                             models = virtual-clock multiplier into [4])
//   [4] playback character   (CharacterChain::playback — per-model
//                             reconstruction; S1100 seeded output dither)
//   [5] optional normalize   (norm == ON: peak-normalize to the SOURCE peak,
//                             dsp-engine.md §7.3; OFF is the authentic default)
//
// This is the buffer the golden tests pin (architecture.md §4.1): the same
// (source, ParamSnapshot, engine version) always renders bit-identically on a
// given platform (architecture.md §6 determinism rule — all rng-bearing
// stages use fixed internal seeds).
//
// Memory cap (architecture.md §5.1): the schedule-predicted output length is
// checked against 10 minutes at model rate PER CHANNEL (PI) BEFORE any
// allocation; an over-cap render is refused with
// RenderError::NotEnoughMemory — the plugin LCD renders it in the hardware's
// own "** NOT ENOUGH MEMORY **" idiom [MAN §3 p.47].

#pragma once

#include <cstdint>
#include <functional>

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/stretch/CyclicEngine.h"
#include "mws/stretch/S950Engine.h"

namespace mws::engine {

/// Why a render produced no audio. None == success.
enum class RenderError : std::uint8_t {
    None,            ///< render completed
    NotEnoughMemory, ///< predicted output exceeds the 10-min model-rate cap
    Aborted,         ///< shouldAbort() returned true at a stage boundary
    UnsupportedModel ///< reserved model slot (S3000 at v1, ADR-004)
};

/// Render metadata for the LCD readout and the state-blob render metadata
/// (architecture.md §6: engine version hash + params re-render a session
/// deterministically instead of storing the rendered audio).
struct RenderInfo {
    /// Achieved output length in frames per channel, in the output buffer's
    /// own rate (`outputSampleRate`) — the figure the LCD length readout shows.
    std::int64_t outputFrames = 0;

    /// Sample rate of `RenderResult::out` in Hz (the source rate — the render
    /// is a new sample alongside its source).
    double outputSampleRate = 0.0;

    /// Achieved stretch ratio in percent: 100 x (post-stretch length /
    /// pre-stretch length) in the model-rate domain. CLASSIC quantizes this
    /// away from the requested timeFactor (the schedule-derived "bad timing"
    /// length, dsp-engine.md §3.4) — the LCD shows the achieved figure. On
    /// the S900 it folds transpose in (varispeed couples time and pitch,
    /// ADR-003).
    double achievedTimeFactorPct = 0.0;

    /// mws::core::engineVersionHash() at render time (task 001).
    std::uint64_t engineVersionHash = 0;

    /// True iff a multi-channel source was summed to mono by the S900/S950
    /// rule (dsp-engine.md §5) — stated on the LCD.
    bool monoSummed = false;
};

/// A completed (or refused) offline render.
struct RenderResult {
    core::AudioBuffer out; ///< empty unless error == RenderError::None
    RenderInfo info;
    RenderError error = RenderError::None;

    [[nodiscard]] bool ok() const noexcept
    {
        return error == RenderError::None;
    }
};

/// The hardware-faithful offline renderer. Stateless across renders and
/// deterministic (architecture.md §6); analysis/allocation-heavy — worker
/// thread material, NEVER the audio thread (the worker glue is task 030).
class OfflineRenderer
{
public:
    /// Worker-thread render callbacks. Both optional (default: no progress
    /// reporting, never aborts). Invoked on the calling thread.
    struct Callbacks {
        /// Monotone progress in [0, 1]; 1.0 exactly once, on completion.
        /// Mirrors the hardware's GO remaining-time countdown [MAN §3 p.47].
        std::function<void(float)> progress;

        /// Polled at stage boundaries (after each pipeline stage and between
        /// per-channel engine renders — the engines themselves are
        /// monolithic). True => the render stops with RenderError::Aborted.
        /// Mirrors the hardware's hold-F8 abort (architecture.md §4).
        std::function<bool()> shouldAbort;
    };

    OfflineRenderer() noexcept = default;

    /// Shares the splice calibration with both cyclic-based engines
    /// (dsp-engine.md §3.1; ADR-001: retuning the calibration retunes
    /// every model at once).
    explicit OfflineRenderer(stretch::CyclicEngine::SpliceCal cal) noexcept
        : cyclic_(cal), s950_(cal)
    {
    }

    /// Render memory cap: 10 minutes of output at model rate per channel
    /// (PI, architecture.md §5.1).
    static constexpr double kMaxRenderSeconds = 600.0;

    /// The cap in frames for a given model rate (per channel).
    [[nodiscard]] static std::int64_t maxOutputFrames(double modelRate) noexcept;

    /// Predicted post-stretch/post-transpose output length per channel, in
    /// MODEL-RATE frames, derived from the engine schedules (never
    /// round(N*T)). Pure math — performs no allocation; this is the figure
    /// render() compares against maxOutputFrames() BEFORE allocating, and the
    /// LCD's predicted-length hint. Params are clamped internally via
    /// ModelSpec::clamp (single authority), exactly as render() clamps them.
    /// S950 MON1 note: prediction uses the set cycle length; per-grain snaps
    /// can move the achieved length slightly (the cap is a guard, dsp-engine
    /// §5 (PI)).
    [[nodiscard]] std::int64_t
    predictedOutputFrames(std::int64_t sourceFrames, double sourceSampleRate,
                          ParamSnapshot params) const;

    /// The full authentic pipeline (header comment above). `source` carries
    /// its rate in `source.sampleRate` (> 0 expected; 44.1 kHz assumed when
    /// unset); the output is rendered back at that rate (a new sample
    /// alongside its source). `params.pluginMode` is forced to SAMPLE before
    /// clamping — the offline render IS the SAMPLE-mode path (ADR-006), so
    /// the FX FREE causality clamp never applies and offline compression
    /// (T < 100%) works per dsp-engine.md §3.4.
    [[nodiscard]] RenderResult render(const core::AudioBuffer& source,
                                      ParamSnapshot params,
                                      const Callbacks& callbacks = {}) const;

private:
    stretch::CyclicEngine cyclic_{};
    stretch::S950Engine s950_{};
};

} // namespace mws::engine
