// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Resampler.h"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

// Test-case names begin with the tag word so `ctest -R resampler` matches
// (plan/backlog/README.md test-selection rules).
//
// Coverage required by docs/design/testing-strategy.md §2 (Resampler bullet)
// and plan/backlog/004-core-resampler.md:
//   - identity at ratio 1.0 (sinc within 1e-6 after group-delay alignment;
//     linear exact),
//   - sine SNR bound 440 Hz 44.1k -> 48k, >= 60 dB (sinc),
//   - impulse response symmetric; measured delay matches groupDelaySamples()
//     within +-0.5 samples (feeds the FX latency formula, dsp-engine.md §7.4),
//   - downsampling band-limits: energy above the new Nyquist <= -50 dB (sinc).

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::core::LinearResampler;
using mws::core::SincResampler;

constexpr double kPi = std::numbers::pi_v<double>;

/// Deterministic broadband-ish test signal (sum of incommensurate sines).
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

/// Energy centroid of the squared output — sub-sample delay measurement.
double energyCentroid(ConstAudioView view)
{
    double weighted = 0.0;
    double total = 0.0;
    for (std::size_t n = 0; n < view.size(); ++n)
    {
        const double e = static_cast<double>(view[n]) * static_cast<double>(view[n]);
        weighted += e * static_cast<double>(n);
        total += e;
    }
    REQUIRE(total > 0.0);
    return weighted / total;
}
} // namespace

TEST_CASE("resampler: sinc identity at ratio 1.0 within 1e-6 after group-delay alignment",
          "[resampler]")
{
    const std::vector<float> input = makeTestSignal(1024);
    const ConstAudioView in(input.data(), input.size());

    const AudioBuffer out = SincResampler::resample(in, 1.0);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.numFrames() == input.size());

    const double delay = SincResampler::groupDelaySamples(1.0);
    const auto delaySamples = static_cast<std::size_t>(std::llround(delay));
    REQUIRE(std::abs(delay - static_cast<double>(delaySamples)) < 1e-12); // integer at 1.0

    const ConstAudioView view = out.channel(0);
    for (std::size_t n = 0; n + delaySamples < input.size(); ++n)
        REQUIRE(std::abs(static_cast<double>(view[n + delaySamples])
                         - static_cast<double>(input[n]))
                < 1e-6);
}

TEST_CASE("resampler: linear identity at ratio 1.0 is exact", "[resampler]")
{
    const std::vector<float> input = makeTestSignal(512);
    const ConstAudioView in(input.data(), input.size());

    const AudioBuffer out = LinearResampler::resample(in, 1.0);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.numFrames() == input.size());
    REQUIRE(LinearResampler::groupDelaySamples(1.0) == 0.0);

    const ConstAudioView view = out.channel(0);
    for (std::size_t n = 0; n < input.size(); ++n)
        REQUIRE(view[n] == input[n]);
}

TEST_CASE("resampler: sinc 440 Hz sine 44.1k to 48k SNR at least 60 dB", "[resampler]")
{
    constexpr double inRate = 44100.0;
    constexpr double outRate = 48000.0;
    constexpr double ratio = outRate / inRate;
    constexpr double freq = 440.0;
    constexpr std::size_t numFrames = 16384;

    std::vector<float> input(numFrames);
    for (std::size_t n = 0; n < numFrames; ++n)
        input[n] = static_cast<float>(
            std::sin(2.0 * kPi * freq * static_cast<double>(n) / inRate));

    const AudioBuffer out =
        SincResampler::resample(ConstAudioView(input.data(), input.size()), ratio);
    const ConstAudioView view = out.channel(0);

    // Output sample n represents the input signal at input-domain time
    // n/ratio - delayIn, where delayIn = groupDelaySamples(ratio) / ratio.
    const double delayIn = SincResampler::groupDelaySamples(ratio) / ratio;

    constexpr std::size_t margin = 256; // skip edge transients
    double signalPower = 0.0;
    double errorPower = 0.0;
    for (std::size_t n = margin; n < view.size() - margin; ++n)
    {
        const double tIn = static_cast<double>(n) / ratio - delayIn;
        const double reference = std::sin(2.0 * kPi * freq * tIn / inRate);
        const double error = static_cast<double>(view[n]) - reference;
        signalPower += reference * reference;
        errorPower += error * error;
    }
    REQUIRE(errorPower > 0.0);
    const double snrDb = 10.0 * std::log10(signalPower / errorPower);
    INFO("SNR = " << snrDb << " dB");
    REQUIRE(snrDb >= 60.0);
}

TEST_CASE("resampler: sinc impulse response is symmetric", "[resampler]")
{
    // Upsampling an impulse by 4x traces the continuous kernel on a fine grid.
    constexpr std::size_t numFrames = 256;
    constexpr std::size_t impulsePos = 128;
    constexpr double ratio = 4.0;

    std::vector<float> input(numFrames, 0.0f);
    input[impulsePos] = 1.0f;

    const AudioBuffer out =
        SincResampler::resample(ConstAudioView(input.data(), input.size()), ratio);
    const ConstAudioView view = out.channel(0);

    // Peak index.
    std::size_t peak = 0;
    for (std::size_t n = 0; n < view.size(); ++n)
        if (std::abs(view[n]) > std::abs(view[peak]))
            peak = n;
    REQUIRE(view[peak] > 0.9f);

    const auto halfSupport = static_cast<std::size_t>(8.0 * ratio);
    REQUIRE(peak >= halfSupport);
    REQUIRE(peak + halfSupport < view.size());
    for (std::size_t k = 1; k <= halfSupport; ++k)
        REQUIRE(std::abs(static_cast<double>(view[peak + k])
                         - static_cast<double>(view[peak - k]))
                < 1e-4);
}

TEST_CASE("resampler: sinc measured delay matches groupDelaySamples within half a sample",
          "[resampler]")
{
    constexpr std::size_t numFrames = 512;
    constexpr std::size_t impulsePos = 200;

    for (const double ratio : { 1.0, 48000.0 / 44100.0, 2.0, 0.5 })
    {
        std::vector<float> input(numFrames, 0.0f);
        input[impulsePos] = 1.0f;

        const AudioBuffer out =
            SincResampler::resample(ConstAudioView(input.data(), input.size()), ratio);

        const double measuredDelay =
            energyCentroid(out.channel(0)) - static_cast<double>(impulsePos) * ratio;
        const double reported = SincResampler::groupDelaySamples(ratio);
        INFO("ratio = " << ratio << ", measured = " << measuredDelay
                        << ", reported = " << reported);
        REQUIRE(std::abs(measuredDelay - reported) <= 0.5);
    }
}

TEST_CASE("resampler: linear measured delay matches groupDelaySamples within half a sample",
          "[resampler]")
{
    constexpr std::size_t numFrames = 512;
    constexpr std::size_t impulsePos = 200;

    for (const double ratio : { 1.0, 48000.0 / 44100.0, 2.0 })
    {
        std::vector<float> input(numFrames, 0.0f);
        input[impulsePos] = 1.0f;

        const AudioBuffer out =
            LinearResampler::resample(ConstAudioView(input.data(), input.size()), ratio);

        const double measuredDelay =
            energyCentroid(out.channel(0)) - static_cast<double>(impulsePos) * ratio;
        const double reported = LinearResampler::groupDelaySamples(ratio);
        INFO("ratio = " << ratio << ", measured = " << measuredDelay
                        << ", reported = " << reported);
        REQUIRE(std::abs(measuredDelay - reported) <= 0.5);
    }
}

TEST_CASE("resampler: sinc downsampling band-limits energy above the new Nyquist",
          "[resampler]")
{
    // 44.1k -> 22.05k. Stopband tones above the new Nyquist (11.025 kHz) and
    // past the Kaiser beta=8 transition band must come through attenuated by
    // at least 50 dB (they would otherwise alias into the audible band).
    constexpr double inRate = 44100.0;
    constexpr double ratio = 0.5;
    constexpr std::size_t numFrames = 32768;

    for (const double freq : { 16000.0, 19000.0 })
    {
        std::vector<float> input(numFrames);
        for (std::size_t n = 0; n < numFrames; ++n)
            input[n] = static_cast<float>(
                std::sin(2.0 * kPi * freq * static_cast<double>(n) / inRate));

        const double inputRms =
            rms(ConstAudioView(input.data(), input.size()), 0, numFrames);

        const AudioBuffer out =
            SincResampler::resample(ConstAudioView(input.data(), input.size()), ratio);
        const ConstAudioView view = out.channel(0);

        constexpr std::size_t margin = 128;
        const double outputRms = rms(view, margin, view.size() - margin);
        const double attenuationDb = 20.0 * std::log10(outputRms / inputRms);
        INFO("freq = " << freq << " Hz, attenuation = " << attenuationDb << " dB");
        REQUIRE(attenuationDb <= -50.0);
    }
}

TEST_CASE("resampler: empty input yields empty output", "[resampler]")
{
    const AudioBuffer sincOut = SincResampler::resample(ConstAudioView(), 2.0);
    REQUIRE(sincOut.numChannels() == 1);
    REQUIRE(sincOut.numFrames() == 0);

    const AudioBuffer linOut = LinearResampler::resample(ConstAudioView(), 0.5);
    REQUIRE(linOut.numChannels() == 1);
    REQUIRE(linOut.numFrames() == 0);
}
