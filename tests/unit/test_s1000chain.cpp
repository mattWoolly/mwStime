// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/Resampler.h"
#include "mws/model/FixedRateChain.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

// Test-case names begin with the tag word so `ctest -R s1000chain` matches
// (plan/backlog/README.md test-selection rules).
//
// Spec: docs/design/dsp-engine.md §8.2 (S1000/S1100 fixed-rate 16-bit chain),
// §8 intro (ingest BEFORE stretch — TAL-validated order [DRR F8]), §2
// `sampleRateSel` (44.1/22.05) and `outTrim` row (NO per-model level offset);
// docs/research/deep-research-report.md Finding 5 (fixed-rate interpolating
// playback). Task: plan/backlog/017-s1000-character-chain.md.

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::core::SincResampler;
using mws::model::FixedRateChain;

constexpr double kPi = std::numbers::pi_v<double>;

/// Fills a vector with a unit-amplitude sine at `freqHz`.
std::vector<float> makeSine(std::size_t numFrames, double freqHz, double sampleRate)
{
    std::vector<float> samples(numFrames);
    for (std::size_t n = 0; n < numFrames; ++n)
        samples[n] = static_cast<float>(
            std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
    return samples;
}

/// Deterministic broadband-ish signal (sum of incommensurate sines), scaled
/// to stay inside +-1.0 full scale.
std::vector<float> makeTestSignal(std::size_t numFrames)
{
    std::vector<float> signal(numFrames);
    for (std::size_t n = 0; n < numFrames; ++n)
    {
        const auto t = static_cast<double>(n);
        signal[n] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 0.01 * t)
                                       + 0.3 * std::sin(2.0 * kPi * 0.037 * t + 0.7)
                                       + 0.2 * std::sin(2.0 * kPi * 0.113 * t + 1.3));
    }
    return signal;
}

double rms(ConstAudioView view, std::size_t begin, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t n = begin; n < end; ++n)
        sum += static_cast<double>(view[n]) * static_cast<double>(view[n]);
    return std::sqrt(sum / static_cast<double>(end - begin));
}

/// True when every sample sits exactly on the 16-bit mid-tread lattice:
/// sample * 32768 is an integer code in [-32768, 32767]. Exact in float32
/// (the quantizer step 1/32768 is a power of two).
bool onSixteenBitLattice(ConstAudioView view)
{
    for (std::size_t n = 0; n < view.size(); ++n)
    {
        const double scaled = static_cast<double>(view[n]) * 32768.0;
        if (scaled != std::nearbyint(scaled))
            return false;
        if (scaled < -32768.0 || scaled > 32767.0)
            return false;
    }
    return true;
}
} // namespace

TEST_CASE("s1000chain: ingest at 22.05 kHz band-limits above the new Nyquist",
          "[s1000chain]")
{
    // sampleRateSel = 22.05 (dsp-engine.md §2): stopband tones above
    // 11.025 kHz must come through attenuated by at least 50 dB — they would
    // otherwise alias into the audible band. The 16-bit quantization noise
    // floor (~-98 dB) is far below the -50 dB budget.
    constexpr double inRate = 44100.0;
    constexpr double modelRate = 22050.0;
    constexpr std::size_t numFrames = 32768;

    for (const double freq : { 16000.0, 19000.0 })
    {
        const std::vector<float> input = makeSine(numFrames, freq, inRate);
        const double inputRms =
            rms(ConstAudioView(input.data(), input.size()), 0, numFrames);

        const AudioBuffer out = FixedRateChain::ingest(
            ConstAudioView(input.data(), input.size()), inRate, modelRate);
        REQUIRE(out.numChannels() == 1);
        REQUIRE(out.numFrames() > 0);
        REQUIRE(out.sampleRate == modelRate);

        const ConstAudioView view = out.channel(0);
        constexpr std::size_t margin = 128;
        const double outputRms = rms(view, margin, view.size() - margin);
        const double attenuationDb = 20.0 * std::log10(outputRms / inputRms);
        INFO("freq = " << freq << " Hz, attenuation = " << attenuationDb << " dB");
        REQUIRE(attenuationDb <= -50.0);
    }
}

TEST_CASE("s1000chain: ingest output is already on the 16-bit lattice "
          "(quantize before the stretch — chain order)",
          "[s1000chain]")
{
    // §8 intro: the hardware stretched already-quantized, already-band-limited
    // sample RAM [DRR F8]. API-level order assertion: everything ingest()
    // hands to the stretch engine is already 16-bit quantized.
    constexpr std::size_t numFrames = 8192;
    const std::vector<float> input = makeTestSignal(numFrames);

    SECTION("44.1 kHz model rate from 48 kHz input")
    {
        const AudioBuffer out = FixedRateChain::ingest(
            ConstAudioView(input.data(), input.size()), 48000.0, 44100.0);
        REQUIRE(out.numFrames() > 0);
        REQUIRE(out.sampleRate == 44100.0);
        REQUIRE(onSixteenBitLattice(out.channel(0)));
    }

    SECTION("22.05 kHz model rate from 44.1 kHz input")
    {
        const AudioBuffer out = FixedRateChain::ingest(
            ConstAudioView(input.data(), input.size()), 44100.0, 22050.0);
        REQUIRE(out.numFrames() > 0);
        REQUIRE(out.sampleRate == 22050.0);
        REQUIRE(onSixteenBitLattice(out.channel(0)));
    }

    SECTION("rate-matched input (44.1 -> 44.1) is still quantized")
    {
        const AudioBuffer out = FixedRateChain::ingest(
            ConstAudioView(input.data(), input.size()), 44100.0, 44100.0);
        REQUIRE(out.numFrames() > 0);
        REQUIRE(onSixteenBitLattice(out.channel(0)));
    }
}

TEST_CASE("s1000chain: playback with the voice filter fully open is "
          "transparent for a 1 kHz sine at 44.1 -> 48 kHz",
          "[s1000chain]")
{
    // §8.2: the voice filter is FULLY OPEN by default on hardware — hard-wiring
    // it active would fabricate an artifact. With S1100 dither off, playback
    // must be the sinc SRC and nothing else.
    constexpr double modelRate = 44100.0;
    constexpr double hostRate = 48000.0;
    constexpr double ratio = hostRate / modelRate;
    constexpr double freq = 1000.0;
    constexpr std::size_t numFrames = 16384;

    const std::vector<float> input = makeSine(numFrames, freq, modelRate);
    const ConstAudioView in(input.data(), input.size());

    const AudioBuffer out =
        FixedRateChain::playback(in, modelRate, hostRate,
                                 /*s1100Dither=*/false, /*seed=*/0);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.sampleRate == hostRate);

    // (a) Identical to the bare sinc resampler — the open filter adds nothing.
    const AudioBuffer reference = SincResampler::resample(in, ratio);
    REQUIRE(out.numFrames() == reference.numFrames());
    const ConstAudioView view = out.channel(0);
    const ConstAudioView refView = reference.channel(0);
    for (std::size_t n = 0; n < view.size(); ++n)
    {
        INFO("frame " << n);
        REQUIRE(std::abs(static_cast<double>(view[n])
                         - static_cast<double>(refView[n]))
                < 1e-6);
    }

    // (b) Within resampler tolerance of the ideal delayed sine (>= 60 dB SNR,
    // the SincResampler quality bound from plan/backlog/004).
    const double delayIn = SincResampler::groupDelaySamples(ratio) / ratio;
    constexpr std::size_t margin = 256; // skip edge transients
    double signalPower = 0.0;
    double errorPower = 0.0;
    for (std::size_t n = margin; n < view.size() - margin; ++n)
    {
        const double tIn = static_cast<double>(n) / ratio - delayIn;
        const double ideal = std::sin(2.0 * kPi * freq * tIn / modelRate);
        const double error = static_cast<double>(view[n]) - ideal;
        signalPower += ideal * ideal;
        errorPower += error * error;
    }
    REQUIRE(errorPower > 0.0);
    const double snrDb = 10.0 * std::log10(signalPower / errorPower);
    INFO("SNR = " << snrDb << " dB");
    REQUIRE(snrDb >= 60.0);
}

TEST_CASE("s1000chain: S1100 dither differs from S1000 only at the +-1 LSB "
          "level and is deterministic per seed",
          "[s1000chain]")
{
    // §8.2 S1100 delta: 16-bit output quantize with TPDF dither (the
    // 20-bit-DAC noise-floor model, PI). NO output-level offset versus the
    // S1000 (panel ruling, §2 outTrim row) — the bounded +-2 LSB diff below
    // is also the no-level-offset assertion: any baked-in trim would blow it.
    constexpr double modelRate = 44100.0;
    constexpr double hostRate = 48000.0;
    constexpr std::size_t numFrames = 8192;
    constexpr std::uint64_t seed = 0x5EEDF00DULL;

    const std::vector<float> input = makeTestSignal(numFrames);
    const ConstAudioView in(input.data(), input.size());

    const AudioBuffer s1000 =
        FixedRateChain::playback(in, modelRate, hostRate, false, seed);
    const AudioBuffer s1100 =
        FixedRateChain::playback(in, modelRate, hostRate, true, seed);
    REQUIRE(s1000.numFrames() == s1100.numFrames());

    const ConstAudioView a = s1000.channel(0);
    const ConstAudioView b = s1100.channel(0);

    // Dithered output sits on the 16-bit lattice; the undithered S1000 path
    // does not get an output quantize.
    REQUIRE(onSixteenBitLattice(b));

    constexpr float maxLsbDiff = 2.0f / 32768.0f;
    float maxDiff = 0.0f;
    for (std::size_t n = 0; n < a.size(); ++n)
        maxDiff = std::max(maxDiff, std::abs(b[n] - a[n]));
    INFO("max |S1100 - S1000| = " << maxDiff << " (budget " << maxLsbDiff << ")");
    REQUIRE(maxDiff <= maxLsbDiff);
    REQUIRE(maxDiff > 0.0f); // the dither stage actually did something

    SECTION("bit-identical for the same seed")
    {
        const AudioBuffer again =
            FixedRateChain::playback(in, modelRate, hostRate, true, seed);
        const ConstAudioView c = again.channel(0);
        REQUIRE(c.size() == b.size());
        for (std::size_t n = 0; n < b.size(); ++n)
        {
            INFO("frame " << n);
            REQUIRE(b[n] == c[n]);
        }
    }

    SECTION("a different seed yields a different dither pattern")
    {
        const AudioBuffer other =
            FixedRateChain::playback(in, modelRate, hostRate, true, seed + 1);
        const ConstAudioView c = other.channel(0);
        REQUIRE(c.size() == b.size());
        bool anyDifferent = false;
        for (std::size_t n = 0; n < b.size() && !anyDifferent; ++n)
            anyDifferent = (b[n] != c[n]);
        REQUIRE(anyDifferent);
    }
}

TEST_CASE("s1000chain: empty input yields empty output from both stages",
          "[s1000chain]")
{
    const AudioBuffer ingestOut = FixedRateChain::ingest(ConstAudioView(),
                                                         48000.0, 44100.0);
    REQUIRE(ingestOut.numChannels() == 1);
    REQUIRE(ingestOut.numFrames() == 0);

    const AudioBuffer playbackOut =
        FixedRateChain::playback(ConstAudioView(), 44100.0, 48000.0, true, 1);
    REQUIRE(playbackOut.numChannels() == 1);
    REQUIRE(playbackOut.numFrames() == 0);
}
