// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "EditorActions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

std::string softKeyPressHint(int index, const engine::ParamSnapshot& params,
                             const model::ModelSpec& spec,
                             const SoftKeyPressContext& ctx)
{
    if (index < 0 || index >= softkey::kCount)
        return {};

    // A mode-disabled key in FX mode: legibility, not silence. The press itself
    // emits nothing (the bar gates it), so the editor calls this to surface WHY
    // — the action is a SAMPLE-mode key (ui-design §6.4). Stretch-only keys that
    // are inert on the S900 (autC/ZONE) report the same idiom.
    if (!softKeyEnabled(index, params, spec))
        return softkeyhint::kSampleModeOnly;

    switch (index)
    {
        // F1 TIME is "page focus" only (PluginEditor::handleSoftKey) — with no
        // engine side effect, a press looked dead. Confirm the page is shown.
        case softkey::kTime:
            return softkeyhint::kTimePage;

        // F2 autC / AUTO-D with no sample: was a silent fallback write. Tell the
        // user to load a sample instead (task 057 scope). With a sample loaded
        // the detector visibly updates the cycle-length field, so no notice.
        case softkey::kAutC:
            return ctx.sampleLoaded ? std::string{} : softkeyhint::kLoadSample;

        // F7 SYNC tap-tempo (ui-design §6.2 step 4): show progress on EACH tap so
        // one tap registers before the 2nd completes the measurement. Once two
        // taps define a BPM, show it; the first tap shows the running count.
        case softkey::kSync:
        {
            if (ctx.tapBpm > 0.0)
            {
                char buf[40]{};
                std::snprintf(buf, sizeof(buf), "** SYNC %ld BPM **",
                              std::lround(ctx.tapBpm));
                return buf;
            }
            char buf[24]{};
            std::snprintf(buf, sizeof(buf), "** TAP %d **", std::max(0, ctx.tapCount));
            return buf;
        }

        // GO/PLAY/A-B/ZONE (SAMPLE-mode, live here) and ABORT (the hold key with
        // its own progress affordance) produce their own visible result.
        default:
            return {};
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

// ---------------------------------------------------------------------------
// Model-switch clamp memory (ui-design §6.5 (PI), task 046)
// ---------------------------------------------------------------------------

namespace {

/// Clamp a bare timeFactor value into a model's engine range via the single
/// clamping authority (ModelSpec::clamp). A SAMPLE-mode snapshot is used so the
/// ADR-006 FX-FREE causality clamp (low end 100%) is NOT applied here — model
/// switching remembers the user's dialed value, which the FX clamp would
/// otherwise floor independently of the model.
double clampTimeFactorForModel(model::ModelId model, double timeFactorPct) noexcept
{
    engine::ParamSnapshot p;
    p.model = model;
    p.pluginMode = engine::PluginMode::Sample;
    p.timeFactor = timeFactorPct;
    return model::ModelSpec::get(model).clamp(p).timeFactor;
}

} // namespace

void ClampMemory::remember(model::ModelId model, double timeFactorPct) noexcept
{
    const auto i = static_cast<std::size_t>(model);
    if (i >= kCount)
        return;
    values_[i] = timeFactorPct;
    set_[i] = true;
}

double ClampMemory::recall(model::ModelId model, double fallback) const noexcept
{
    const auto i = static_cast<std::size_t>(model);
    return (i < kCount && set_[i]) ? values_[i] : fallback;
}

bool ClampMemory::has(model::ModelId model) const noexcept
{
    const auto i = static_cast<std::size_t>(model);
    return i < kCount && set_[i];
}

void ClampMemory::forget(model::ModelId model) noexcept
{
    const auto i = static_cast<std::size_t>(model);
    if (i < kCount)
        set_[i] = false;
}

double applyModelSwitchTimeFactor(ClampMemory& memory, model::ModelId oldModel,
                                  model::ModelId newModel,
                                  double currentHostTimeFactor) noexcept
{
    // (1) Remember the value effective on the model we are LEAVING, but only
    // when that model did not itself clamp it — a clamped old value is the
    // ceiling, not a fresh user intent, so storing it would overwrite a genuine
    // pre-clamp value (e.g. 1500 dialed on the S1000) with the cap.
    const double oldClamped = clampTimeFactorForModel(oldModel, currentHostTimeFactor);
    const bool oldWasClamped =
        (oldClamped < currentHostTimeFactor) || (currentHostTimeFactor < oldClamped);
    if (!oldWasClamped)
        memory.remember(oldModel, currentHostTimeFactor);

    // (2) The new model's candidate is its remembered pre-clamp value if any,
    // else the current value carried across (the host range is the fixed
    // superset, so the value travels with us). Remember the candidate as the
    // new model's pre-clamp memory, then clamp it for the host/engine.
    const double candidate = memory.recall(newModel, currentHostTimeFactor);
    memory.remember(newModel, candidate);
    return clampTimeFactorForModel(newModel, candidate);
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
