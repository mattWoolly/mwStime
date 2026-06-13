// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Cyclic property suite (plan/backlog/012) — the remaining research-pinned
// invariants of the cyclic core (docs/design/testing-strategy.md §3 items
// 5, 6, 8):
//
//  - splice-comb signature (§3.8): the FFT of a stretched steady sine shows
//    sidebands spaced at modelRate / hop_out within one bin of prediction —
//    the "metallic ring" made measurable (akaizer-analysis.md §3 property 1:
//    splice discontinuities repeat at the grain rate sr / hop_out);
//  - stereo coherence (§3.5): stereo runs two linked instances with an
//    identical, shared hop schedule (dsp-engine.md §3 intro, §3.4 stereo edge
//    rule), so identical params + identical content give per-channel
//    sample-identical output, and with differing content the grain launch
//    times are still identical across channels (CLASSIC and REVISED);
//  - determinism (§3.6, core part): same (input, params) twice gives
//    bit-identical buffers (the TSan/threading half lives in task 030).
//
// The expected hop constants are derived INDEPENDENTLY from the adopted
// [AKZ §4.2] scheduler as specified in dsp-engine.md §3.1 — never by calling
// the engine's own scheduling code. The in-test radix-2 FFT below is local to
// the test tree by design (task 012: do NOT add it to mwstime-core).
//
// Test-case names begin with "properties" so `ctest -R properties` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mws/stretch/CyclicEngine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::engine::HopMode;
using mws::stretch::CyclicEngine;
using mws::stretch::GrainLaunch;
using mws::stretch::GrainLaunchObserver;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Signal generators (deterministic — testing-strategy.md §3.6 needs fixed
// content, no platform-dependent randomness).
// ---------------------------------------------------------------------------

AudioBuffer makeSine(std::int64_t numFrames, double freqHz, double sampleRate,
                     float amplitude = 0.5f)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] = amplitude
            * static_cast<float>(
                std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
    return buffer;
}

/// Fixed-seed LCG noise in (-0.5, 0.5) — reproducible on every platform
/// (Numerical Recipes constants; integer arithmetic only until the final
/// exact power-of-two scale).
AudioBuffer makeNoise(std::int64_t numFrames, std::uint32_t seed)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    std::uint32_t state = seed;
    for (std::int64_t n = 0; n < numFrames; ++n)
    {
        state = state * 1664525u + 1013904223u;
        view[static_cast<std::size_t>(n)] =
            static_cast<float>(static_cast<std::int32_t>(state))
            * (0.5f / 2147483648.0f);
    }
    return buffer;
}

// ---------------------------------------------------------------------------
// Independent §3.1 hop derivation (mirrors test_cyclic_classic.cpp — NOT the
// engine's code): ovStart = C * (1 - F) clamped to [1, C]; hop_out = ovStart;
// CLASSIC hop_in = round(hop_out / T) with integer-% T, clamped >= 1.
// ---------------------------------------------------------------------------
struct IndependentHops
{
    std::int64_t hopOut = 0;
    std::int64_t hopInClassic = 0;

    IndependentHops(std::int64_t cycleLen, double timeFactorPct)
    {
        const double overlapF = static_cast<double>(CyclicEngine::SpliceCal{}.overlapF);
        std::int64_t ovStart =
            std::llround(static_cast<double>(cycleLen) * (1.0 - overlapF));
        ovStart = std::clamp<std::int64_t>(ovStart, 1, cycleLen);
        hopOut = ovStart;
        const auto tPct = std::max<std::int64_t>(1, std::llround(timeFactorPct));
        hopInClassic = std::max<std::int64_t>(
            1, std::llround(static_cast<double>(hopOut) * 100.0
                            / static_cast<double>(tPct)));
    }
};

// ---------------------------------------------------------------------------
// Minimal in-test radix-2 FFT (iterative, in place). Test-tree only — task
// 012 forbids adding it to mwstime-core.
// ---------------------------------------------------------------------------
void fftRadix2(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    REQUIRE((n & (n - 1)) == 0); // power of two

    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

/// Hann-windowed magnitude spectrum of out[skip .. skip+fftLen).
std::vector<double> magnitudeSpectrum(ConstAudioView out, std::size_t skip,
                                      std::size_t fftLen)
{
    REQUIRE(out.size() >= skip + fftLen);
    std::vector<std::complex<double>> bins(fftLen);
    for (std::size_t i = 0; i < fftLen; ++i)
    {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i)
                                 / static_cast<double>(fftLen));
        bins[i] = { static_cast<double>(out[skip + i]) * w, 0.0 };
    }
    fftRadix2(bins);
    std::vector<double> mags(fftLen / 2);
    for (std::size_t i = 0; i < fftLen / 2; ++i)
        mags[i] = std::abs(bins[i]);
    return mags;
}

/// Index of the largest magnitude in [lo, hi).
std::size_t argmaxBin(const std::vector<double>& mags, std::size_t lo, std::size_t hi)
{
    return static_cast<std::size_t>(
        std::max_element(mags.begin() + static_cast<std::ptrdiff_t>(lo),
                         mags.begin() + static_cast<std::ptrdiff_t>(hi))
        - mags.begin());
}

/// Sub-bin peak position by parabolic interpolation around an argmax bin
/// (standard three-point fit; accurate to a small fraction of a bin for a
/// Hann-windowed line).
double refinePeakBin(const std::vector<double>& mags, std::size_t peakBin)
{
    REQUIRE(peakBin >= 1);
    REQUIRE(peakBin + 1 < mags.size());
    const double m0 = mags[peakBin - 1];
    const double m1 = mags[peakBin];
    const double m2 = mags[peakBin + 1];
    const double denom = m0 - 2.0 * m1 + m2;
    const double delta = (denom == 0.0) ? 0.0 : 0.5 * (m0 - m2) / denom;
    return static_cast<double>(peakBin) + delta;
}

// ---------------------------------------------------------------------------
// Shared helpers for bit-identity and schedule capture.
// ---------------------------------------------------------------------------

bool bitIdentical(ConstAudioView a, ConstAudioView b)
{
    return a.size() == b.size()
           && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::vector<GrainLaunch> captureSchedule(const CyclicEngine& engine,
                                         ConstAudioView src, int cycleLen,
                                         double timeFactorPct, HopMode mode,
                                         AudioBuffer* outCopy = nullptr)
{
    std::vector<GrainLaunch> schedule;
    const GrainLaunchObserver observer = [&schedule](const GrainLaunch& launch)
    { schedule.push_back(launch); };
    AudioBuffer out = engine.render(src, cycleLen, timeFactorPct, mode, &observer);
    if (outCopy != nullptr)
        *outCopy = std::move(out);
    return schedule;
}

} // namespace

// ===========================================================================
// 1. Splice-comb signature (testing-strategy.md §3.8; akaizer-analysis.md §3
//    property 1): splice discontinuities of the content-blind cycle repeat at
//    the grain rate modelRate / hop_out, so a stretched steady sine carries
//    spectral lines spaced exactly modelRate / hop_out apart.
// ===========================================================================
TEST_CASE("properties: cyclic splice-comb signature — sidebands spaced at "
          "modelRate/hop_out",
          "[cyclic][properties]")
{
    constexpr double fs = 44100.0;     // model rate (S1000 native)
    constexpr double sineHz = 440.0;   // steady sine (task example)
    constexpr int cycleLen = 1000;     // C = 1000 (task example)
    constexpr double timeFactor = 300.0; // T = 300% (task example)
    constexpr std::int64_t numIn = 44100;
    constexpr std::size_t fftLen = 1 << 16; // bin width fs/65536 ~ 0.673 Hz
    constexpr std::size_t skip = 2048;      // skip the un-spliced onset

    const IndependentHops hops(cycleLen, timeFactor); // hop_out = 800
    REQUIRE(hops.hopOut == 800);
    const double spacingBins =
        static_cast<double>(fftLen) / static_cast<double>(hops.hopOut); // 81.92

    const AudioBuffer src = makeSine(numIn, sineHz, fs);
    const CyclicEngine engine;

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    const AudioBuffer out =
        engine.render(src.channel(0), cycleLen, timeFactor, mode);
    REQUIRE(out.numFrames() >= skip + fftLen);

    const std::vector<double> mags = magnitudeSpectrum(out.channel(0), skip, fftLen);

    // Carrier-region line: strongest bin overall (excluding DC leakage).
    const std::size_t mainBin = argmaxBin(mags, 16, mags.size() - 2);
    const double mainPos = refinePeakBin(mags, mainBin);
    const double mainMag = mags[mainBin];
    REQUIRE(mainMag > 0.0);

    // Median magnitude = broadband floor reference for sideband prominence.
    std::vector<double> sorted = mags;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double medianMag = sorted[sorted.size() / 2];

    // Sidebands at mainPos + k * spacing, k = ±1, ±2: each must be a real
    // spectral line (well above the floor) within ONE BIN of the prediction.
    for (const int k : { -2, -1, 1, 2 })
    {
        CAPTURE(k);
        const double predicted = mainPos + static_cast<double>(k) * spacingBins;
        const auto lo = static_cast<std::size_t>(
            std::llround(predicted - 0.4 * spacingBins));
        const auto hi = static_cast<std::size_t>(
            std::llround(predicted + 0.4 * spacingBins));
        REQUIRE(lo >= 1);
        REQUIRE(hi + 1 < mags.size());

        const std::size_t found = argmaxBin(mags, lo, hi + 1);
        const double foundPos = refinePeakBin(mags, found);
        CAPTURE(predicted, foundPos, mags[found] / mainMag);

        // §3.8: within one bin of prediction.
        REQUIRE(std::abs(foundPos - predicted) <= 1.0);

        // The sideband is a genuine line, not the noise floor: at least 20 dB
        // above the median floor and no more than 60 dB below the carrier.
        REQUIRE(mags[found] >= 10.0 * medianMag);
        REQUIRE(mags[found] >= 1.0e-3 * mainMag);
    }
}

// ===========================================================================
// 2. Stereo coherence (testing-strategy.md §3.5): stereo = two linked engine
//    instances with an IDENTICAL, SHARED hop schedule (dsp-engine.md §3 intro;
//    §3.4 "stereo: run twice with the identical hop_in schedule").
// ===========================================================================
TEST_CASE("properties: cyclic stereo coherence — identical params + content give "
          "sample-identical channels",
          "[cyclic][properties]")
{
    constexpr std::int64_t numIn = 8000;
    constexpr int cycleLen = 1000;
    constexpr double timeFactor = 300.0;

    // One stereo buffer, both channels carrying the identical content.
    const AudioBuffer mono = makeNoise(numIn, 0xA5A5A5A5u);
    AudioBuffer stereo(2, static_cast<std::size_t>(numIn));
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        auto dst = stereo.channel(ch);
        const auto srcView = mono.channel(0);
        std::memcpy(dst.data(), srcView.data(), srcView.size() * sizeof(float));
    }

    const CyclicEngine engine;
    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    const AudioBuffer left =
        engine.render(stereo.channel(0), cycleLen, timeFactor, mode);
    const AudioBuffer right =
        engine.render(stereo.channel(1), cycleLen, timeFactor, mode);

    REQUIRE(left.numFrames() == right.numFrames());
    REQUIRE(bitIdentical(left.channel(0), right.channel(0)));
}

TEST_CASE("properties: cyclic stereo coherence — grain launch schedule identical "
          "across channels with differing content",
          "[cyclic][properties]")
{
    constexpr double fs = 44100.0;
    constexpr std::int64_t numIn = 8000;
    constexpr int cycleLen = 1000;
    constexpr double timeFactor = 300.0;

    // Two channels with DIFFERENT content (sine left, noise right): the hop
    // schedule is content-blind, so the launch times must still be identical.
    const AudioBuffer left = makeSine(numIn, 440.0, fs);
    const AudioBuffer right = makeNoise(numIn, 0xDEADBEEFu);

    const CyclicEngine engine;
    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    const std::vector<GrainLaunch> scheduleL =
        captureSchedule(engine, left.channel(0), cycleLen, timeFactor, mode);
    const std::vector<GrainLaunch> scheduleR =
        captureSchedule(engine, right.channel(0), cycleLen, timeFactor, mode);

    REQUIRE(!scheduleL.empty());
    REQUIRE(scheduleL.size() == scheduleR.size());
    for (std::size_t i = 0; i < scheduleL.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(scheduleL[i].outIndex == scheduleR[i].outIndex);
        // Bit-exact offset equality (REVISED offsets are doubles).
        REQUIRE(std::memcmp(&scheduleL[i].srcOffset, &scheduleR[i].srcOffset,
                            sizeof(double))
                == 0);
    }

    // Schedule sanity against the independent §3.1/§3.4 derivation: grains
    // launch every hop_out output samples, advancing hop_in input samples
    // (CLASSIC integer hop; REVISED fractional hop_out/T).
    const IndependentHops hops(cycleLen, timeFactor);
    REQUIRE(scheduleL[0].outIndex == 0);
    REQUIRE(scheduleL[0].srcOffset == 0.0);
    for (std::size_t i = 0; i < scheduleL.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(scheduleL[i].outIndex
                == static_cast<std::int64_t>(i) * hops.hopOut);
        if (mode == HopMode::Classic)
            REQUIRE(scheduleL[i].srcOffset
                    == static_cast<double>(static_cast<std::int64_t>(i)
                                           * hops.hopInClassic));
        else
        {
            const double hopInRevised =
                static_cast<double>(hops.hopOut) / (timeFactor / 100.0);
            REQUIRE(std::abs(scheduleL[i].srcOffset
                             - static_cast<double>(i) * hopInRevised)
                    < 1.0e-6);
        }
    }
}

TEST_CASE("properties: cyclic observation hook does not change the rendered output",
          "[cyclic][properties]")
{
    constexpr std::int64_t numIn = 8000;
    constexpr int cycleLen = 700;
    constexpr double timeFactor = 250.0;

    const AudioBuffer src = makeNoise(numIn, 0x12345678u);
    const CyclicEngine engine;

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    const AudioBuffer plain =
        engine.render(src.channel(0), cycleLen, timeFactor, mode);
    AudioBuffer observed;
    const std::vector<GrainLaunch> schedule = captureSchedule(
        engine, src.channel(0), cycleLen, timeFactor, mode, &observed);

    REQUIRE(!schedule.empty());
    REQUIRE(plain.numFrames() == observed.numFrames());
    REQUIRE(bitIdentical(plain.channel(0), observed.channel(0)));
}

// ===========================================================================
// 3. Determinism (testing-strategy.md §3.6, core part): same (input, params)
//    twice gives bit-identical buffers — no uninitialized state, no hidden
//    randomness. (The TSan/threading half is task 030.)
// ===========================================================================
TEST_CASE("properties: cyclic determinism — same input and params twice give "
          "bit-identical buffers",
          "[cyclic][properties]")
{
    constexpr std::int64_t numIn = 12000;
    constexpr int cycleLen = 1000;

    const AudioBuffer noise = makeNoise(numIn, 0xC0FFEE42u);
    const AudioBuffer sine = makeSine(numIn, 440.0, 44100.0);

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    // Fresh engine instances per render: determinism must come from the
    // (input, params) pair alone, not shared instance state.
    for (const double timeFactor : { 153.0, 300.0, 61.5 })
    {
        CAPTURE(timeFactor);
        for (const AudioBuffer* src : { &noise, &sine })
        {
            const AudioBuffer first =
                CyclicEngine{}.render(src->channel(0), cycleLen, timeFactor, mode);
            const AudioBuffer second =
                CyclicEngine{}.render(src->channel(0), cycleLen, timeFactor, mode);
            REQUIRE(first.numFrames() == second.numFrames());
            REQUIRE(bitIdentical(first.channel(0), second.channel(0)));
        }
    }
}
