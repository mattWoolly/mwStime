// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// The SpliceCal / D-TIME calibration fitter implementation (task 026b;
// docs/qa/hardware-capture-plan.md §4). See Calibrate.h for the contract.

#include "Calibrate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "mws/core/Buffer.h"
#include "mws/core/WavIo.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/model/ModelSpec.h"
#include "mws/stretch/S950Engine.h"

namespace mwscal {
namespace {

using mws::core::AudioBuffer;
using mws::core::WavIo;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::model::ModelSpec;
using mws::stretch::CyclicEngine;

/// Joins a directory and a filename with a single '/'. (Mirrors the renderer's
/// joinPath so the diagnostic paths read identically; std::filesystem avoided
/// for stable cross-platform messages.)
std::string joinPath(const std::string& dir, const std::string& file)
{
    if (dir.empty())
        return file;
    if (dir.back() == '/' || dir.back() == '\\')
        return dir + file;
    return dir + "/" + file;
}

/// A SpliceCal with a given overlapF and the defaults for shape/rounding. v1
/// has exactly one FadeShape (Linear) and one HopRounding (RoundNearest) in the
/// authentic engines (dsp-engine.md §3.1/§3.3), so overlapF is the only
/// continuous fit dimension; shape/rounding are reported as their fixed values.
CyclicEngine::SpliceCal calWith(float overlapF) noexcept
{
    CyclicEngine::SpliceCal cal;
    cal.overlapF = overlapF;
    return cal;
}

/// Renders one case through the full authentic offline pipeline at a given
/// splice calibration. The OfflineRenderer shares the SpliceCal with both
/// cyclic-based engines (dsp-engine.md §3.1), so this is exactly the path a
/// real capture would be compared against. Returns an empty buffer on refusal
/// (the calibration corpus never hits the render cap, so this only guards a
/// reserved model).
AudioBuffer renderAt(const AudioBuffer& src, const ParamSnapshot& params,
                     float overlapF)
{
    const OfflineRenderer renderer(calWith(overlapF));
    const mws::engine::RenderResult r = renderer.render(src, params);
    return r.ok() ? r.out : AudioBuffer{};
}

/// Normalization-robust distance between a candidate render and a capture
/// (docs/qa/hardware-capture-plan.md §4): peak-normalize both (captures are
/// unnormalized and a real A/D has an arbitrary gain — the splice GEOMETRY,
/// not the level, is what we fit), then combine a length-mismatch penalty with
/// the per-sample RMS over the overlapping region. A length mismatch dominates
/// (the schedule-derived length is the strongest splice observable), so the
/// minimum sits at the overlapF that reproduces both the length and the
/// waveform — exactly the planted value for a synthetic capture (residual 0).
double distance(const AudioBuffer& cand, const AudioBuffer& capture)
{
    const auto na = static_cast<std::int64_t>(cand.numFrames());
    const auto nb = static_cast<std::int64_t>(capture.numFrames());
    if (na == 0 || nb == 0)
        return 1e18;

    auto peak = [](const AudioBuffer& b) {
        double p = 0.0;
        const auto v = b.channel(0);
        for (std::size_t i = 0; i < b.numFrames(); ++i)
            p = std::max(p, std::fabs(static_cast<double>(v[i])));
        return p;
    };
    const double pa = peak(cand);
    const double pb = peak(capture);
    const double ga = pa > 0.0 ? 1.0 / pa : 0.0;
    const double gb = pb > 0.0 ? 1.0 / pb : 0.0;

    const double lenPenalty =
        std::fabs(static_cast<double>(na - nb)) / static_cast<double>(nb);

    const std::int64_t n = std::min(na, nb);
    const auto va = cand.channel(0);
    const auto vb = capture.channel(0);
    double sumSq = 0.0;
    for (std::int64_t i = 0; i < n; ++i)
    {
        const double d = ga * static_cast<double>(va[static_cast<std::size_t>(i)])
                         - gb * static_cast<double>(vb[static_cast<std::size_t>(i)]);
        sumSq += d * d;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(n));

    // The length penalty weight (PI): big enough that a one-grain length error
    // (a wrong hop_out) always loses to the correct schedule, small enough that
    // the RMS still discriminates between equal-length candidates.
    return rms + 10.0 * lenPenalty;
}

} // namespace

double flutterRateHz(double modelRateHz, std::int64_t hopOut) noexcept
{
    if (hopOut <= 0 || modelRateHz <= 0.0)
        return 0.0;
    return modelRateHz / static_cast<double>(hopOut);
}

std::int64_t hopOutForCycle(int cycleLen, float overlapF) noexcept
{
    const auto c = static_cast<std::int64_t>(std::max(1, cycleLen));
    auto ov = static_cast<std::int64_t>(
        std::llround(static_cast<double>(c) * (1.0 - static_cast<double>(overlapF))));
    ov = std::clamp<std::int64_t>(ov, 1, c);
    return ov; // hop_out == ovStart (GrainGeometry::fromCycle, dsp-engine §3.1)
}

float overlapFForHopOut(int cycleLen, std::int64_t hopOut) noexcept
{
    const auto c = static_cast<double>(std::max(1, cycleLen));
    const double f = 1.0 - static_cast<double>(hopOut) / c;
    return static_cast<float>(std::clamp(f, 1.0e-3, 1.0 - 1.0e-3));
}

CaseFit fitCase(const CalCase& c, const FitConfig& cfg)
{
    CaseFit fit;
    fit.id = c.id;
    fit.set = c.set;

    // Read the committed synthetic input and the (uncommitted, local) capture.
    const WavIo::ReadResult src = WavIo::read(joinPath(cfg.inputsDir, c.inputFile));
    if (!src.ok())
    {
        fit.error = "cannot read input '" + c.inputFile + "': " + src.error;
        return fit;
    }
    const WavIo::ReadResult cap = WavIo::read(joinPath(cfg.capturesDir, c.id + ".wav"));
    if (!cap.ok())
    {
        fit.error = "cannot read capture '" + c.id + ".wav': " + cap.error;
        return fit;
    }

    // Clamp the snapshot through the single authority so C / T / model rate
    // match what the engine will actually use (ModelSpec::clamp — dsp-engine
    // §2). For the S950, cycleLen IS the D-TIME value; ModelSpec/S950Engine map
    // it to a cycle length in samples (the (PI) mapping under test, §5).
    const ModelSpec& spec = ModelSpec::get(c.params.model);
    const ParamSnapshot clamped = spec.clamp(c.params);

    // The cycle length the schedule uses. For S950 this is D-TIME → C via the
    // mapping under test; for the cyclic models it is cycleLen directly. Both
    // go through the same kDTime/kCycleLen range (20–2000) — using the engine's
    // own mapper keeps the observable in lock-step with the rendered audio.
    const int effCycle = (c.params.model == mws::model::ModelId::S950)
                             ? mws::stretch::S950Engine::mapDTimeToCycleLen(clamped.cycleLen)
                             : clamped.cycleLen;

    fit.obs.cycleLen = effCycle;
    fit.obs.modelRateHz = spec.modelRateHz(clamped);
    fit.obs.captureLen = static_cast<std::int64_t>(cap.buffer.numFrames());

    // --- fit (Calibration) or predict (Validation) -----------------------
    // Validation cases are PREDICTED at the frozen overlapF — never searched
    // (the no-overfitting rule, docs/qa/...-plan.md §3). A run that supplies no
    // frozen F (self-test combined run) falls back to the same grid; production
    // splits the two runs so the held-out set is genuinely predicted.
    const bool predictOnly =
        (c.set == CaseSet::Validation) && (cfg.frozenOverlapF >= 0.0f);

    float bestF = (cfg.frozenOverlapF >= 0.0f) ? cfg.frozenOverlapF : cfg.gridLo;
    double bestD = 1e18;

    if (predictOnly)
    {
        const AudioBuffer cand = renderAt(src.buffer, c.params, bestF);
        bestD = distance(cand, cap.buffer);
    }
    else
    {
        for (float F = cfg.gridLo; F <= cfg.gridHi + 1.0e-6f; F += cfg.gridStep)
        {
            const AudioBuffer cand = renderAt(src.buffer, c.params, F);
            const double d = distance(cand, cap.buffer);
            if (d < bestD)
            {
                bestD = d;
                bestF = F;
            }
        }
    }

    fit.overlapF = bestF;
    fit.residual = bestD;
    fit.ok = true;

    // Schedule-derived observables at the fitted/frozen F (dsp-engine §3.1/§3.4).
    fit.obs.hopOut = hopOutForCycle(effCycle, bestF);
    const auto tPct = std::max<std::int64_t>(1, std::llround(clamped.timeFactor));
    fit.obs.hopIn =
        std::max<std::int64_t>(1, (2 * fit.obs.hopOut * 100 + tPct) / (2 * tPct));
    fit.obs.flutterRateHz = flutterRateHz(fit.obs.modelRateHz, fit.obs.hopOut);

    // Schedule-derived output length from the fitted engine (the "bad timing"
    // length, NOT round(N·T) — dsp-engine §3.4 / testing-strategy §3 prop 2).
    const CyclicEngine sched(calWith(bestF));
    fit.obs.scheduleLen = sched.expectedOutputLength(
        static_cast<std::int64_t>(src.buffer.numFrames()), effCycle,
        clamped.timeFactor, clamped.hopMode);

    // --- S950 D-TIME mapping check (the v1-freeze gate, dsp-engine §5) -----
    // The mapping under test is "D-TIME ≡ cycle length in samples". It is
    // CONSISTENT when, at the fitted F, re-rendering with that C reproduces the
    // capture within tolerance: i.e. the residual is at or below the tolerance.
    // (A grossly wrong D-TIME→C mapping would force a different hop_out and the
    // residual would not collapse at any F.)
    if (c.params.model == mws::model::ModelId::S950)
    {
        fit.dTimeChecked = true;
        fit.dTimeConsistent = (bestD <= cfg.consistencyTol);
    }

    return fit;
}

} // namespace mwscal
