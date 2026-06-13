// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// EditorActions (task 045b) — the headless decision logic behind the editor's
// soft-key interaction flows (docs/design/ui-design.md §6.1–§6.4, §1 soft-key
// TIME set). The PluginEditor wires the SoftKeyBar / WaveformView callbacks to
// the engine; the per-mode/per-model RULES that drive what the keys DO, what
// they READ, when they GREY, and the tap-tempo averaging math live here as
// plain functions so they unit-test without a window or a processor
// (tests/plugin/test_editor_actions.cpp).
//
// Soft-key TIME-page set (ui-design §1; §6):
//   F1 TIME  — page focus
//   F2 autC  — auto cycle detection trigger (reads AUTO-D on the S950, [MAN §2])
//   F3 ZONE  — CYCLIC zone-preview toggle (§6.3 step 1)
//   F4 GO    — offline render request (§6.3 step 2)
//   F5 PLAY  — audition the render (§6.3 step 3)
//   F6 A/B   — original/stretched audition toggle (§6.3 step 3)
//   F7 SYNC  — source-BPM entry (typed + tap), §6.2 step 4
//   F8 ABORT — hold >= 600 ms to raise the abort flag (§6.3 step 2)
//
// FX mode (§6.4): GO/PLAY/A-B grey out (no offline render / audition in the
// realtime path); TIME/autC/ZONE/SYNC/ABORT stay available. On the S900
// (varispeed, no timestretch — ADR-003) the stretch-only keys (autC/ZONE) are
// inert too.
//
// HEADLESS RULE: no JUCE includes — std::string / std::array only, like
// LcdPageModel. The PluginEditor maps these strings onto SoftKeyBar captions.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

namespace mws::ui {

/// Stable soft-key indices for the TIME page set (matches SoftKeyBar 0..7).
namespace softkey {
inline constexpr int kTime  = 0;  ///< F1 TIME — page focus
inline constexpr int kAutC  = 1;  ///< F2 autC / AUTO-D — auto cycle detect
inline constexpr int kZone  = 2;  ///< F3 ZONE — CYCLIC zone-preview toggle
inline constexpr int kGo    = 3;  ///< F4 GO — offline render
inline constexpr int kPlay  = 4;  ///< F5 PLAY — audition
inline constexpr int kAb    = 5;  ///< F6 A/B — original/stretched toggle
inline constexpr int kSync  = 6;  ///< F7 SYNC — source-BPM entry
inline constexpr int kAbort = 7;  ///< F8 ABORT — hold-to-abort
inline constexpr int kCount = 8;
} // namespace softkey

/// The eight TIME-page soft-key captions for a model/mode. F2 reads `AUTO-D`
/// on the S950 (the variable-clock 2-line machine, [MAN §2]) and `autC`
/// elsewhere (ui-design §6.2 step 3). Everything else is the §1 mockup set.
[[nodiscard]] std::array<std::string, softkey::kCount>
softKeyLabels(const engine::ParamSnapshot& params, const model::ModelSpec& spec);

/// Whether soft key `index` is enabled (emits its action) for this snapshot.
/// FX mode greys GO/PLAY/A-B (no offline render / audition in the realtime
/// path — ui-design §6.4). The S900 has no timestretch (ADR-003), so the
/// stretch-only autC/ZONE keys are inert on it in every mode; GO still
/// renders the varispeed repitch. Out-of-range indices return false.
[[nodiscard]] bool softKeyEnabled(int index, const engine::ParamSnapshot& params,
                                  const model::ModelSpec& spec) noexcept;

/// Whether ZONE preview is permitted for this model: CYCLIC stretch models
/// only (the S900 varispeed model has no stretch; ui-design §6.3 step 1, the
/// EngineHost ZONE call is a no-op there). True for S950/S1000/S1100.
[[nodiscard]] bool zonePreviewSupported(const model::ModelSpec& spec) noexcept;

// ---------------------------------------------------------------------------
// F2 autC / AUTO-D auto cycle detection (ui-design §6.2 step 3).
// ---------------------------------------------------------------------------

/// Run the task-014 cycle detector over the selected stretch zone of `source`
/// and return the proposed CYCLE length in samples. The zone is given in
/// absolute SOURCE frames [zoneStart, zoneEnd); a degenerate / out-of-range
/// zone falls back to the whole buffer. With no source (nullptr / empty) the
/// detector's documented fallback (AutoCycle::kFallbackCycleLen) is returned
/// so F2 always lands a sensible value in the cycle field. Message-thread /
/// analysis-time (AutoCycle allocates — NOT real-time-safe).
[[nodiscard]] int autoCycleForZone(const std::shared_ptr<const core::AudioBuffer>& source,
                                   std::int64_t zoneStart, std::int64_t zoneEnd);

// ---------------------------------------------------------------------------
// Tap tempo (ui-design §6.2 step 4: SYNC source BPM "typed or tap").
// ---------------------------------------------------------------------------

/// Tap-tempo averaging over recorded tap timestamps (milliseconds, monotone).
/// The BPM is 60000 / mean(inter-tap intervals). Returns 0 when fewer than
/// two taps are available (one tap defines no interval). Stale taps are the
/// caller's concern (the editor resets the buffer after a gap); this is the
/// pure averaging math the QA fleet can pin.
[[nodiscard]] double tapTempoBpm(const std::vector<double>& tapTimesMs) noexcept;

/// A small fixed-capacity ring of tap timestamps with the averaging built in
/// (the editor's F7-tap state). A tap that arrives more than `kResetGapMs`
/// after the previous one starts a fresh measurement (a new tempo, not a
/// continuation). Headless + deterministic (the clock is injected by the
/// caller passing the timestamp).
class TapTempo
{
public:
    /// Taps older than this many ms before the newest tap are discarded; a
    /// gap this large also restarts the measurement. (PI) — a comfortable
    /// 2 s window holds ~4 taps at 120 BPM without dragging in a stale one.
    static constexpr double kResetGapMs = 2000.0;
    /// How many recent taps the average spans (PI): 4 taps = 3 intervals,
    /// enough to settle hand jitter without lagging a tempo change.
    static constexpr std::size_t kMaxTaps = 4;

    /// Record a tap at `nowMs`. Returns the BPM implied by the running window
    /// (0 until at least two taps exist). A gap > kResetGapMs since the last
    /// tap restarts the window with this tap as the new first.
    double tap(double nowMs) noexcept;

    /// Drop all recorded taps (e.g. when SYNC entry closes).
    void reset() noexcept { taps_.clear(); }

    [[nodiscard]] std::size_t numTaps() const noexcept { return taps_.size(); }

private:
    std::vector<double> taps_;
};

} // namespace mws::ui
