// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CrossCheck — the analytic comparison math for the LOCAL-ONLY Akaizer secondary
// cross-check (task plan/backlog/026c-akaizer-crosscheck.md;
// docs/design/testing-strategy.md §7 Wave 2, §8; docs/qa/akaizer-crosscheck.md).
//
// THIS IS A SECONDARY CROSS-CHECK, NOT AN ORACLE. Akaizer's "near-exact"
// fidelity claim was refuted 0-3 (deep-research-report.md Finding 6); the
// primary oracle is hardware captures (task 026b). The comparison is therefore
// ANALYTIC and DESCRIPTIVE: it is *not expected to null* — deviations are
// reported, never gated.
//
// What the comparison covers per case (all three are derivable from the
// dsp-engine.md §3 CLASSIC scheduler with NO Akaizer binary, so the math is
// self-testable on synthetic data):
//
//   1. Splice-comb / flutter frequency — the "metallic ring" sideband spacing
//      that CLASSIC cyclic stretch imprints. Predicted analytically as
//      modelRate / hop_out (testing-strategy.md §3 property 8;
//      akaizer-analysis.md §3 property 1) and measured from a render as the
//      dominant peak of the peak-normalized magnitude spectrum.
//   2. Stutter / grain schedule — the integer grain-launch hop and the
//      Bresenham-like repeat pattern (akaizer-analysis.md §2.4): hop_out =
//      C·(1−F), hop_in = round(hop_out / T) in CLASSIC. Compared as the grain
//      hop in samples and the count of launched grains.
//   3. Schedule-derived output length — (G−1)·hop_out + C with
//      G = floor((N−C)/hop_in)+1, derived FROM THE SCHEDULER, never round(N·T)
//      (dsp-engine.md §3.4; testing-strategy.md §3 property 2).
//
// Two procedural invariants the harness MUST enforce before any comparison:
//   - PEAK-NORMALIZE BOTH SIDES (Akaizer output is peak-normalized to source
//     peak since v1.3 — dsp-engine.md §7.3, akaizer-analysis.md §2.1).
//   - Restrict to the certified window T 120–2000% (akaizer-analysis.md §2.2);
//     outside it Akaizer has no verified oracle (compression 25–119%, S950
//     D-TIME) — those cases are SKIPPED with an explicit note, not compared.
//
// JUCE-free; links only mwstime_core (Buffer/WavIo). Header-only math so the
// self-test (`akzcheck`) can exercise it without any Akaizer binary present.

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <string>
#include <vector>

#include "mws/core/Buffer.h"

namespace mws::akz {

/// Certified Akaizer cross-check window, percent of original duration
/// (akaizer-analysis.md §2.2: "mainly when Time Factor is between 120% and 2000%").
/// Boundaries inclusive. Outside this, there is no Akaizer oracle.
inline constexpr double kWindowLowPct = 120.0;
inline constexpr double kWindowHighPct = 2000.0;

/// Default overlap fraction F (CyclicEngine::SpliceCal, dsp-engine.md §3.1).
/// Used only to PREDICT the engine's grain schedule for the analytic compare;
/// it is a calibration param, so the harness exposes it rather than hard-baking.
inline constexpr double kDefaultOverlapF = 0.20;

/// Is this time factor (percent) inside the certified Akaizer window?
[[nodiscard]] inline bool inWindow(double timeFactorPct) noexcept
{
    return timeFactorPct >= kWindowLowPct && timeFactorPct <= kWindowHighPct;
}

/// The CLASSIC grain schedule for one case, derived from the dsp-engine.md §3.4
/// scheduler (NOT round(N·T)). All fields are integers/derived analytically.
struct Schedule {
    double hopOut = 0.0;        ///< C·(1−F), output spacing of grain launches.
    long hopIn = 0;             ///< round(hopOut / T), CLASSIC integer input hop.
    long grains = 0;            ///< floor((N−C)/hopIn)+1, launched grains.
    long outputLen = 0;         ///< (G−1)·hopOut + C, schedule-derived length.
    double realizedRatio = 0.0; ///< hopOut / hopIn (quantized — "bad timing").
};

/// Computes the CLASSIC schedule for input length `inputFrames`, cycle `C`
/// (samples), time factor `T` (ratio = pct/100), overlap fraction `F`.
/// Mirrors dsp-engine.md §3.4 exactly so the self-test can check it against a
/// hand-computed value. Returns a zeroed schedule for degenerate inputs.
[[nodiscard]] inline Schedule classicSchedule(long inputFrames, long cycleC,
                                              double timeRatio,
                                              double overlapF = kDefaultOverlapF)
{
    Schedule s;
    if (inputFrames <= 0 || cycleC <= 0 || timeRatio <= 0.0)
        return s;
    // Clamp C to the input length if the file is shorter (dsp-engine.md §3.4,
    // [AKZ §2.1] v1.6 behavior).
    const long C = (cycleC > inputFrames) ? inputFrames : cycleC;
    s.hopOut = static_cast<double>(C) * (1.0 - overlapF);
    s.hopIn = std::lround(s.hopOut / timeRatio);
    if (s.hopIn < 1)
        s.hopIn = 1; // CLASSIC requires hop_in >= 1 (dsp-engine.md §3.4).
    s.realizedRatio = s.hopOut / static_cast<double>(s.hopIn);
    // Grains launched: floor((N − C)/hop_in) + 1 (dsp-engine.md §3.4 comment).
    if (inputFrames > C)
        s.grains = (inputFrames - C) / s.hopIn + 1;
    else
        s.grains = 1;
    s.outputLen = static_cast<long>(
        std::llround(static_cast<double>(s.grains - 1) * s.hopOut)) + C;
    return s;
}

/// Predicted splice-comb / flutter frequency (Hz) for a render at `modelRate`
/// with output grain spacing `hopOut`: sidebands spaced at modelRate / hop_out
/// (testing-strategy.md §3 property 8). 0 if undefined.
[[nodiscard]] inline double predictedFlutterHz(double hopOut, double modelRate) noexcept
{
    if (hopOut <= 0.0 || modelRate <= 0.0)
        return 0.0;
    return modelRate / hopOut;
}

/// Peak-normalizes a buffer IN PLACE so its max abs sample equals `targetPeak`
/// (default 1.0). Scales all channels by one common factor (preserves stereo
/// balance). A silent buffer is left unchanged. Both the mwstime-render output
/// AND the Akaizer render MUST pass through this before any comparison
/// (dsp-engine.md §7.3; Akaizer normalizes since v1.3, akaizer-analysis.md §2.1).
inline void peakNormalize(mws::core::AudioBuffer& buf, double targetPeak = 1.0)
{
    double peak = 0.0;
    for (std::size_t ch = 0; ch < buf.numChannels(); ++ch)
    {
        const auto v = buf.channel(ch);
        for (std::size_t n = 0; n < v.size(); ++n)
            peak = std::max(peak, std::fabs(static_cast<double>(v[n])));
    }
    if (peak <= 0.0)
        return;
    const auto g = static_cast<float>(targetPeak / peak);
    for (std::size_t ch = 0; ch < buf.numChannels(); ++ch)
    {
        auto v = buf.channel(ch);
        for (std::size_t n = 0; n < v.size(); ++n)
            v[n] *= g;
    }
}

namespace detail {

/// Largest power of two <= n (0 -> 0).
[[nodiscard]] inline std::size_t floorPow2(std::size_t n) noexcept
{
    if (n == 0)
        return 0;
    std::size_t p = 1;
    while ((p << 1) != 0 && (p << 1) <= n)
        p <<= 1;
    return p;
}

/// In-place iterative radix-2 forward FFT. `data` length MUST be a power of 2.
/// (Same kernel as tools/golden_compare; duplicated to keep this header
/// self-contained and JUCE/3rd-party-free for the self-test.)
inline void fft(std::vector<std::complex<double>>& data)
{
    const std::size_t n = data.size();
    if (n <= 1)
        return;
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    constexpr double kPi = std::numbers::pi_v<double>;
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<double> u = data[i + k];
                const std::complex<double> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace detail

/// Measures the dominant spectral peak (Hz) of channel 0 of `buf` above
/// `floorHz` — used as the empirical flutter/splice-comb frequency. Hann-
/// windowed magnitude spectrum, DC and sub-`floorHz` bins ignored. Returns 0 if
/// the signal is too short or silent. The buffer should be peak-normalized
/// first so the measurement is amplitude-independent.
[[nodiscard]] inline double measureDominantHz(const mws::core::AudioBuffer& buf,
                                              double floorHz = 20.0)
{
    if (buf.numChannels() == 0 || buf.sampleRate <= 0.0)
        return 0.0;
    const auto ch = buf.channel(0);
    const std::size_t fftLen = detail::floorPow2(ch.size());
    if (fftLen < 4)
        return 0.0;

    constexpr double kPi = std::numbers::pi_v<double>;
    std::vector<std::complex<double>> spec(fftLen);
    for (std::size_t i = 0; i < fftLen; ++i)
    {
        const double wnd = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i)
                                                / static_cast<double>(fftLen - 1));
        spec[i] = std::complex<double>(static_cast<double>(ch[i]) * wnd, 0.0);
    }
    detail::fft(spec);

    const double binHz = buf.sampleRate / static_cast<double>(fftLen);
    const auto firstBin = static_cast<std::size_t>(std::ceil(floorHz / binHz));
    std::size_t peakBin = 0;
    double peakMag = 0.0;
    for (std::size_t k = std::max<std::size_t>(firstBin, 1); k <= fftLen / 2; ++k)
    {
        const double m = std::abs(spec[k]);
        if (m > peakMag)
        {
            peakMag = m;
            peakBin = k;
        }
    }
    return static_cast<double>(peakBin) * binHz;
}

/// One per-case cross-check result. Populated for cases inside the window;
/// `skipped` is set (with `skipReason`) for cases outside it.
struct CaseReport {
    std::string id;             ///< case id
    double timeFactorPct = 0.0; ///< T (percent)
    long cycleLen = 0;          ///< C (samples)
    double modelRate = 0.0;     ///< model sample rate (Hz)

    bool skipped = false;       ///< true => no Akaizer oracle, not compared
    std::string skipReason;     ///< why it was skipped (printed in the report)

    // Analytic (engine-side) predictions, valid when !skipped.
    Schedule schedule;          ///< CLASSIC schedule derived from §3.4
    double predictedFlutterHz = 0.0;

    // Measured from the two normalized renders, valid when both supplied.
    bool haveMeasurements = false;
    double mwsFlutterHz = 0.0;      ///< dominant peak, mwstime render
    double akaizerFlutterHz = 0.0;  ///< dominant peak, Akaizer render
    long mwsOutputLen = 0;          ///< measured frames, mwstime render
    long akaizerOutputLen = 0;      ///< measured frames, Akaizer render

    /// Flutter deviation in cents (akaizer vs mws), 0 if not measurable.
    [[nodiscard]] double flutterDeviationCents() const noexcept
    {
        if (mwsFlutterHz <= 0.0 || akaizerFlutterHz <= 0.0)
            return 0.0;
        return 1200.0 * std::log2(akaizerFlutterHz / mwsFlutterHz);
    }

    /// Output-length deviation in frames (akaizer − mws), 0 if not measurable.
    [[nodiscard]] long outputLenDeviationFrames() const noexcept
    {
        if (!haveMeasurements)
            return 0;
        return akaizerOutputLen - mwsOutputLen;
    }
};

/// Builds the analytic part of a case report (window filter + CLASSIC schedule
/// + predicted flutter). No audio required — this is what the self-test checks.
[[nodiscard]] inline CaseReport analyzeCase(const std::string& id,
                                            double timeFactorPct, long cycleLen,
                                            long inputFrames, double modelRate,
                                            double overlapF = kDefaultOverlapF)
{
    CaseReport r;
    r.id = id;
    r.timeFactorPct = timeFactorPct;
    r.cycleLen = cycleLen;
    r.modelRate = modelRate;

    if (!inWindow(timeFactorPct))
    {
        r.skipped = true;
        r.skipReason = "no Akaizer oracle: T outside certified window "
                       + std::to_string(static_cast<long>(kWindowLowPct)) + "-"
                       + std::to_string(static_cast<long>(kWindowHighPct)) + "%";
        return r;
    }

    r.schedule = classicSchedule(inputFrames, cycleLen, timeFactorPct / 100.0, overlapF);
    r.predictedFlutterHz = predictedFlutterHz(r.schedule.hopOut, modelRate);
    return r;
}

} // namespace mws::akz
