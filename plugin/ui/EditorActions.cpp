// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "EditorActions.h"

#include <algorithm>
#include <numeric>

#include "mws/stretch/AutoCycle.h"

namespace mws::ui {

using engine::PluginMode;
using model::ModelId;

bool zonePreviewSupported(const model::ModelSpec& spec) noexcept
{
    // CYCLIC stretch models only. The S900 is the varispeed-repitch machine
    // (ADR-003: no timestretch), so it has no zone to preview through the
    // stretcher; every other shipping model stretches (S950 / S1000 / S1100).
    return spec.shipping && spec.id != ModelId::S900;
}

int autoCycleForZone(const std::shared_ptr<const core::AudioBuffer>& source,
                     std::int64_t zoneStart, std::int64_t zoneEnd)
{
    if (source == nullptr || source->numChannels() == 0 || source->numFrames() == 0)
        return stretch::AutoCycle::kFallbackCycleLen;

    const auto total = static_cast<std::int64_t>(source->numFrames());

    // Normalize the zone: clamp into the buffer, fall back to the whole buffer
    // for a degenerate / inverted selection (the detector wants a real span).
    std::int64_t start = std::clamp<std::int64_t>(zoneStart, 0, total);
    std::int64_t end = std::clamp<std::int64_t>(zoneEnd, 0, total);
    if (end <= start)
    {
        start = 0;
        end = total;
    }

    // Channel 0 is the analysis source (autC analyzes the (summed) signal; the
    // mono machines already sum, and one channel is a faithful pitch proxy).
    const auto chan = source->channel(0);
    const auto* data = chan.data() + static_cast<std::size_t>(start);
    const auto len = static_cast<std::size_t>(end - start);
    const double rate = source->sampleRate > 0.0 ? source->sampleRate : 44100.0;

    return stretch::AutoCycle::detectCycleLen(core::ConstAudioView{ data, len }, rate);
}

std::array<std::string, softkey::kCount>
softKeyLabels(const engine::ParamSnapshot& params, const model::ModelSpec& spec)
{
    // The §1 mockup TIME-page set; F2 relabels per model (autC vs AUTO-D).
    std::array<std::string, softkey::kCount> labels{
        "TIME", "autC", "ZONE", "GO", "PLAY", "A/B", "SYNC", "ABORT",
    };

    // F2 reads AUTO-D on the S950 (the 2-line variable-clock machine, [MAN §2];
    // ui-design §6.2 step 3) and autC on the S1000-family page.
    if (spec.id == ModelId::S950)
        labels[softkey::kAutC] = "AUTO-D";

    (void) params; // labels do not (yet) depend on mode — only enable does
    return labels;
}

bool softKeyEnabled(int index, const engine::ParamSnapshot& params,
                    const model::ModelSpec& spec) noexcept
{
    if (index < 0 || index >= softkey::kCount)
        return false;

    const bool fx = params.pluginMode == PluginMode::Fx;
    const bool stretches = zonePreviewSupported(spec);

    switch (index)
    {
        case softkey::kTime:  return true;            // page focus, always live
        case softkey::kSync:  return true;            // source-BPM entry, both modes
        case softkey::kAbort: return true;            // abort is always armable

        // autC / ZONE are stretch-only: inert on the S900 (no timestretch).
        case softkey::kAutC:  return stretches;
        case softkey::kZone:  return stretches && !fx; // ZONE preview is SAMPLE-mode

        // GO / PLAY / A-B grey out in FX mode (no offline render / audition in
        // the realtime path — ui-design §6.4).
        case softkey::kGo:    return !fx;
        case softkey::kPlay:  return !fx;
        case softkey::kAb:    return !fx;

        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Tap tempo
// ---------------------------------------------------------------------------

double tapTempoBpm(const std::vector<double>& tapTimesMs) noexcept
{
    if (tapTimesMs.size() < 2)
        return 0.0;

    // Mean inter-tap interval. The taps are assumed in arrival order; the mean
    // interval is just (last - first) / (count - 1), which equals the average
    // of the consecutive deltas (telescoping). Guard against a degenerate
    // zero/negative span.
    const double span = tapTimesMs.back() - tapTimesMs.front();
    const auto intervals = static_cast<double>(tapTimesMs.size() - 1);
    if (span <= 0.0 || intervals <= 0.0)
        return 0.0;

    const double meanIntervalMs = span / intervals;
    return 60000.0 / meanIntervalMs;
}

double TapTempo::tap(double nowMs) noexcept
{
    // A long gap since the previous tap means a fresh measurement.
    if (!taps_.empty() && (nowMs - taps_.back()) > kResetGapMs)
        taps_.clear();

    taps_.push_back(nowMs);

    // Keep only the most recent kMaxTaps so the average tracks the latest tempo.
    while (taps_.size() > kMaxTaps)
        taps_.erase(taps_.begin());

    return tapTempoBpm(taps_);
}

} // namespace mws::ui
