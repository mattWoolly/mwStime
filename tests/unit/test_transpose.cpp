// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/engine/Transpose.h"

#include "mws/core/Resampler.h"
#include "mws/model/ModelId.h"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

// Test-case names begin with the tag word so `ctest -R transpose` matches
// (plan/backlog/README.md test-selection rules).
//
// Coverage required by plan/backlog/018-transpose-stage.md and
// docs/design/dsp-engine.md §7.2:
//   - +12 semitones halves length (±1) and shifts a 440 Hz sine to 880 Hz
//     (peak bin),
//   - −12 doubles length, no imaging above the original band (≤ −50 dB),
//   - 0 semitones = identity within sinc tolerance,
//   - pitching up band-limits (no alias of a near-Nyquist tone, ≤ −50 dB),
//   - clockRatioFor(+12) == 2.0 exactly (§8.1 clock = f_s × 2^(transpose/12)),
//   - routing rule: S900/S950 modulate the virtual DAC clock (NO resampling),
//     S1000/S1100 use the sinc stage [DRR F7].

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::core::SincResampler;
using mws::engine::Transpose;
using mws::model::ModelId;

constexpr double kPi = std::numbers::pi_v<double>;

std::vector<float> makeSine(std::size_t numFrames, double freqHz, double sampleRate)
{
    std::vector<float> signal(numFrames);
    for (std::size_t n = 0; n < numFrames; ++n)
        signal[n] = static_cast<float>(
            std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
    return signal;
}

/// Goertzel single-bin amplitude estimate, normalized so a unit sine at
/// `freqHz` yields ~1.0 regardless of `n` or `sampleRate` (same helper shape
/// as tests/unit/test_varclock.cpp). Analyses exactly `n` samples starting at
/// `offset`; callers pick `n` so probe frequencies are exact bins
/// (freqHz * n / sampleRate integral — no leakage between probes).
double goertzelAmplitude(ConstAudioView signal, std::size_t offset, std::size_t n,
                         double freqHz, double sampleRate)
{
    REQUIRE(offset + n <= signal.size());
    const double w = 2.0 * kPi * freqHz / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        s0 = static_cast<double>(signal[offset + i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * std::cos(w);
    const double imag = s2 * std::sin(w);
    return 2.0 * std::sqrt(real * real + imag * imag) / static_cast<double>(n);
}

double rms(ConstAudioView view, std::size_t begin, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t n = begin; n < end; ++n)
        sum += static_cast<double>(view[n]) * static_cast<double>(view[n]);
    return std::sqrt(sum / static_cast<double>(end - begin));
}

double db(double amplitudeRatio)
{
    return 20.0 * std::log10(amplitudeRatio);
}
} // namespace

// ---------------------------------------------------------------------------
// transposeSinc — ratio p = 2^(−semitones/12) as output-length resample
// [AKZ §10].
// ---------------------------------------------------------------------------

TEST_CASE("transpose: +12 semitones halves length and shifts 440 Hz to 880 Hz",
          "[transpose]")
{
    constexpr double sampleRate = 44100.0;
    constexpr std::size_t numFrames = 16384;
    const std::vector<float> input = makeSine(numFrames, 440.0, sampleRate);

    const AudioBuffer out =
        Transpose::transposeSinc(ConstAudioView(input.data(), input.size()), 12.0);
    REQUIRE(out.numChannels() == 1);

    // ceil(N * 2^(-1)) = N/2; allow ±1 per the task spec.
    const auto expected = static_cast<long long>(numFrames / 2);
    const auto actual = static_cast<long long>(out.numFrames());
    REQUIRE(std::abs(actual - expected) <= 1);

    // Peak-bin scan: the output, played at the same rate, must peak at 880 Hz.
    // n = 4410 makes every 10 Hz probe an exact bin.
    const ConstAudioView view = out.channel(0);
    constexpr std::size_t n = 4410;
    constexpr std::size_t offset = 1024;
    REQUIRE(offset + n <= view.size());

    double peakFreq = 0.0;
    double peakAmp = 0.0;
    for (double freq = 100.0; freq <= 2000.0; freq += 10.0)
    {
        const double amp = goertzelAmplitude(view, offset, n, freq, sampleRate);
        if (amp > peakAmp)
        {
            peakAmp = amp;
            peakFreq = freq;
        }
    }
    INFO("peak " << peakFreq << " Hz, amplitude " << peakAmp);
    REQUIRE(peakFreq == 880.0);
    REQUIRE(peakAmp > 0.9);
}

TEST_CASE("transpose: -12 semitones doubles length with no imaging above the "
          "original band",
          "[transpose]")
{
    constexpr double sampleRate = 44100.0;
    constexpr std::size_t numFrames = 16384;
    const std::vector<float> input = makeSine(numFrames, 440.0, sampleRate);

    const AudioBuffer out =
        Transpose::transposeSinc(ConstAudioView(input.data(), input.size()), -12.0);
    REQUIRE(out.numChannels() == 1);

    // ceil(N * 2^(+1)) = 2N; allow ±1 per the task spec.
    const auto expected = static_cast<long long>(numFrames * 2);
    const auto actual = static_cast<long long>(out.numFrames());
    REQUIRE(std::abs(actual - expected) <= 1);

    // n = 8820 puts 220 Hz (bin 44) and every 5 Hz probe on exact bins, so the
    // fundamental leaks nothing into the image probes.
    const ConstAudioView view = out.channel(0);
    constexpr std::size_t n = 8820;
    constexpr std::size_t offset = 2048;
    REQUIRE(offset + n <= view.size());

    const double fundamental = goertzelAmplitude(view, offset, n, 220.0, sampleRate);
    REQUIRE(fundamental > 0.9);

    // Interpolation images of the 440 Hz source would land above the original
    // band; everything from 1 kHz to Nyquist must sit at or below −50 dB
    // relative to the output fundamental.
    for (double freq = 1000.0; freq < 22000.0; freq += 250.0)
    {
        const double amp = goertzelAmplitude(view, offset, n, freq, sampleRate);
        INFO("probe " << freq << " Hz: " << db(amp / fundamental) << " dB");
        REQUIRE(db(amp / fundamental) <= -50.0);
    }
}

TEST_CASE("transpose: 0 semitones is identity within sinc tolerance", "[transpose]")
{
    // Deterministic multi-tone signal (same recipe as test_resampler.cpp).
    constexpr std::size_t numFrames = 1024;
    std::vector<float> input(numFrames);
    for (std::size_t n = 0; n < numFrames; ++n)
    {
        const auto t = static_cast<double>(n);
        input[n] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 0.01 * t)
                                      + 0.3 * std::sin(2.0 * kPi * 0.037 * t + 0.7)
                                      + 0.2 * std::sin(2.0 * kPi * 0.113 * t + 1.3));
    }

    const AudioBuffer out =
        Transpose::transposeSinc(ConstAudioView(input.data(), input.size()), 0.0);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.numFrames() == numFrames);

    // 2^(0/12) = 1 exactly -> the sinc identity path: integer group delay,
    // matches the source within 1e-6 (test_resampler.cpp identity bound).
    const double delay = SincResampler::groupDelaySamples(1.0);
    const auto delaySamples = static_cast<std::size_t>(std::llround(delay));
    REQUIRE(std::abs(delay - static_cast<double>(delaySamples)) < 1e-12);

    const ConstAudioView view = out.channel(0);
    for (std::size_t n = 0; n + delaySamples < numFrames; ++n)
        REQUIRE(std::abs(static_cast<double>(view[n + delaySamples])
                         - static_cast<double>(input[n]))
                < 1e-6);
}

TEST_CASE("transpose: pitching up band-limits a near-Nyquist tone (no alias, "
          "<= -50 dB)",
          "[transpose]")
{
    // A 20 kHz tone pitched up +12 would land at 40 kHz — above Nyquist at
    // 44.1 kHz. The anti-alias band-limit (dsp-engine.md §7.2, [AKZ §2.4])
    // must suppress it rather than fold it back into the audible band.
    constexpr double sampleRate = 44100.0;
    constexpr std::size_t numFrames = 32768;
    const std::vector<float> input = makeSine(numFrames, 20000.0, sampleRate);

    const double inputRms =
        rms(ConstAudioView(input.data(), input.size()), 0, numFrames);

    const AudioBuffer out =
        Transpose::transposeSinc(ConstAudioView(input.data(), input.size()), 12.0);
    const ConstAudioView view = out.channel(0);
    REQUIRE(view.size() > 512);

    constexpr std::size_t margin = 128;
    const double outputRms = rms(view, margin, view.size() - margin);
    const double attenuationDb = db(outputRms / inputRms);
    INFO("attenuation = " << attenuationDb << " dB");
    REQUIRE(attenuationDb <= -50.0);
}

// ---------------------------------------------------------------------------
// clockRatioFor — the S900/S950 virtual-DAC-clock multiplier
// (dsp-engine.md §8.1: clock = f_s × 2^(transpose/12)). No resampling here.
// ---------------------------------------------------------------------------

TEST_CASE("transpose: clockRatioFor is exact at octaves", "[transpose]")
{
    REQUIRE(Transpose::clockRatioFor(12.0) == 2.0);
    REQUIRE(Transpose::clockRatioFor(0.0) == 1.0);
    REQUIRE(Transpose::clockRatioFor(-12.0) == 0.5);
    REQUIRE(Transpose::clockRatioFor(24.0) == 4.0);
}

TEST_CASE("transpose: clockRatioFor is monotonic in semitones", "[transpose]")
{
    double previous = 0.0;
    for (double st = -24.0; st <= 24.0; st += 1.0)
    {
        const double ratio = Transpose::clockRatioFor(st);
        REQUIRE(ratio > previous);
        previous = ratio;
    }
}

// ---------------------------------------------------------------------------
// Routing rule — encoded once so OfflineRenderer (task 020) can't mis-route
// (dsp-engine.md §7.2; the clock-tracking property RX950 lacks [DRR F7]).
// ---------------------------------------------------------------------------

TEST_CASE("transpose: routing sends varclock models to the clock, fixed-rate "
          "models to the sinc",
          "[transpose]")
{
    // S900/S950: NO resampling in this stage — clock modulation only.
    REQUIRE_FALSE(Transpose::usesSincTranspose(ModelId::S900));
    REQUIRE_FALSE(Transpose::usesSincTranspose(ModelId::S950));

    // S1000/S1100: post-stretch windowed-sinc repitch.
    REQUIRE(Transpose::usesSincTranspose(ModelId::S1000));
    REQUIRE(Transpose::usesSincTranspose(ModelId::S1100));

    // Reserved S3000 slot (ADR-004, no behavior at v1): defaults to the sinc
    // path — it is a fixed-rate machine, never a varclock one.
    REQUIRE(Transpose::usesSincTranspose(ModelId::S3000));
}
