// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/MovingLpf.h"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

// Test-case names begin with the tag word so `ctest -R movinglpf` matches
// (plan/backlog/README.md test-selection rules).
//
// Spec: docs/design/dsp-engine.md §8.2 (VOICE FILTER), testing-strategy.md §2
// MovingLpf bullet. The S1000/S1100 -18 dB/oct voice filter is a TRUE 3rd-order
// Butterworth (one real pole + complex pair, Q = 1.0), NOT three identical
// one-poles, and is fully open (transparent) by default.

namespace
{
using mws::core::AudioView;
using mws::core::MovingLpf3;

constexpr double kPi = std::numbers::pi;

/// Fills `samples` with a unit-amplitude sine at `freqHz`.
void fillSine(std::vector<float>& samples, double freqHz, double sampleRate)
{
    for (std::size_t n = 0; n < samples.size(); ++n)
        samples[n] = static_cast<float>(
            std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
}

/// Single-bin DFT amplitude of `samples[start, start+length)` at `freqHz`.
/// `length` must span an integer number of cycles of `freqHz` for an exact read.
double toneAmplitude(const std::vector<float>& samples,
                     std::size_t start,
                     std::size_t length,
                     double freqHz,
                     double sampleRate)
{
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < length; ++i)
    {
        const auto n = static_cast<double>(start + i);
        const double phase = 2.0 * kPi * freqHz * n / sampleRate;
        const auto x = static_cast<double>(samples[start + i]);
        re += x * std::cos(phase);
        im += x * std::sin(phase);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(length);
}

/// Measured steady-state gain (dB) of `filter` at `freqHz`: processes a long
/// unit sine and reads the output tone amplitude after the transient.
double measureGainDb(MovingLpf3& filter, double freqHz, double sampleRate)
{
    constexpr std::size_t kWarmup = 4096;
    constexpr std::size_t kWindow = 9600; // integer cycles for 1/2/4 kHz @ 96 kHz
    std::vector<float> samples(kWarmup + kWindow);
    fillSine(samples, freqHz, sampleRate);

    filter.reset();
    filter.process(AudioView(samples.data(), samples.size()));

    const double amplitude =
        toneAmplitude(samples, kWarmup, kWindow, freqHz, sampleRate);
    return 20.0 * std::log10(amplitude);
}

/// Reference: three IDENTICAL bilinear one-pole low-passes at `cutoffHz` in
/// series — the wrong topology the spec forbids (dsp-engine.md §8.2, panel
/// critique P15). Used only to assert our response measurably differs from it.
double measureCascadedOnePoleGainDb(double freqHz, double cutoffHz, double sampleRate)
{
    const double k = std::tan(kPi * cutoffHz / sampleRate);
    const double b = k / (k + 1.0);
    const double a1 = (k - 1.0) / (k + 1.0);

    constexpr std::size_t kWarmup = 4096;
    constexpr std::size_t kWindow = 9600;
    std::vector<float> samples(kWarmup + kWindow);
    fillSine(samples, freqHz, sampleRate);

    for (int stage = 0; stage < 3; ++stage)
    {
        double x1 = 0.0;
        double y1 = 0.0;
        for (auto& sample : samples)
        {
            const auto x = static_cast<double>(sample);
            const double y = b * (x + x1) - a1 * y1;
            x1 = x;
            y1 = y;
            sample = static_cast<float>(y);
        }
    }

    const double amplitude =
        toneAmplitude(samples, kWarmup, kWindow, freqHz, sampleRate);
    return 20.0 * std::log10(amplitude);
}

/// Analytic 3rd-order Butterworth low-pass magnitude (dB):
/// |H(f)| = 1 / sqrt(1 + (f/fc)^6).
double analyticButterworth3Db(double freqHz, double cutoffHz)
{
    const double ratio = freqHz / cutoffHz;
    return -10.0 * std::log10(1.0 + std::pow(ratio, 6.0));
}
} // namespace

TEST_CASE("movinglpf: matches analytic 3rd-order Butterworth at fc/2fc/4fc "
          "(-18 dB/oct)",
          "[movinglpf]")
{
    // fc small vs fs so bilinear frequency warping stays inside the ±0.5 dB
    // budget out to 4fc.
    constexpr double sampleRate = 96000.0;
    constexpr double cutoffHz = 1000.0;

    MovingLpf3 filter;
    filter.setCutoff(cutoffHz, sampleRate);

    for (const double multiple : { 1.0, 2.0, 4.0 })
    {
        const double freqHz = multiple * cutoffHz;
        const double measuredDb = measureGainDb(filter, freqHz, sampleRate);
        const double analyticDb = analyticButterworth3Db(freqHz, cutoffHz);

        INFO("f = " << freqHz << " Hz, measured " << measuredDb
                    << " dB, analytic " << analyticDb << " dB");
        REQUIRE(std::abs(measuredDb - analyticDb) < 0.5);
    }

    // Slope sanity: a true -18 dB/oct filter loses ~18 dB per octave deep in
    // the stopband (2fc -> 4fc).
    MovingLpf3 slopeFilter;
    slopeFilter.setCutoff(cutoffHz, sampleRate);
    const double at2fc = measureGainDb(slopeFilter, 2.0 * cutoffHz, sampleRate);
    const double at4fc = measureGainDb(slopeFilter, 4.0 * cutoffHz, sampleRate);
    REQUIRE(std::abs((at2fc - at4fc) - 18.0) < 1.0);
}

TEST_CASE("movinglpf: true Butterworth, not three identical one-poles",
          "[movinglpf]")
{
    // Three identical one-poles at fc read ~-21.0 dB at 2fc; the true 3rd-order
    // Butterworth reads ~-18.1 dB. Assert we are on the Butterworth side by
    // more than 1 dB (dsp-engine.md §8.2, panel critique P15).
    constexpr double sampleRate = 96000.0;
    constexpr double cutoffHz = 1000.0;

    MovingLpf3 filter;
    filter.setCutoff(cutoffHz, sampleRate);

    const double butterworthDb = measureGainDb(filter, 2.0 * cutoffHz, sampleRate);
    const double cascadeDb =
        measureCascadedOnePoleGainDb(2.0 * cutoffHz, cutoffHz, sampleRate);

    INFO("measured " << butterworthDb << " dB vs cascaded one-pole reference "
                     << cascadeDb << " dB at 2fc");
    REQUIRE(std::abs(butterworthDb - cascadeDb) > 1.0);
}

TEST_CASE("movinglpf: fully open is transparent across 20 Hz-20 kHz",
          "[movinglpf]")
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t numFrames = 4800;

    const double testFreqsHz[] = { 20.0, 100.0, 1000.0, 5000.0, 10000.0, 20000.0 };

    SECTION("default-constructed state is fully open")
    {
        for (const double freqHz : testFreqsHz)
        {
            std::vector<float> input(numFrames);
            fillSine(input, freqHz, sampleRate);
            std::vector<float> output = input;

            MovingLpf3 filter; // fully open by default — no setFullyOpen() call
            filter.process(AudioView(output.data(), output.size()));

            for (std::size_t n = 0; n < numFrames; ++n)
            {
                INFO("f = " << freqHz << " Hz, frame " << n);
                REQUIRE(std::abs(output[n] - input[n]) < 1e-6f);
            }
        }
    }

    SECTION("setFullyOpen() restores transparency after a cutoff was set")
    {
        for (const double freqHz : testFreqsHz)
        {
            std::vector<float> input(numFrames);
            fillSine(input, freqHz, sampleRate);
            std::vector<float> output = input;

            MovingLpf3 filter;
            filter.setCutoff(500.0, sampleRate);
            filter.process(AudioView(output.data(), output.size())); // colour it
            filter.setFullyOpen();
            filter.reset();

            output = input;
            filter.process(AudioView(output.data(), output.size()));

            for (std::size_t n = 0; n < numFrames; ++n)
            {
                INFO("f = " << freqHz << " Hz, frame " << n);
                REQUIRE(std::abs(output[n] - input[n]) < 1e-6f);
            }
        }
    }
}

TEST_CASE("movinglpf: reset clears filter state", "[movinglpf]")
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t numFrames = 512;

    std::vector<float> first(numFrames, 1.0f); // step input charges the state
    std::vector<float> second(numFrames);
    fillSine(second, 1000.0, sampleRate);
    std::vector<float> reference = second;

    MovingLpf3 filter;
    filter.setCutoff(2000.0, sampleRate);
    filter.process(AudioView(first.data(), first.size()));
    filter.reset();
    filter.process(AudioView(second.data(), second.size()));

    MovingLpf3 freshFilter;
    freshFilter.setCutoff(2000.0, sampleRate);
    freshFilter.process(AudioView(reference.data(), reference.size()));

    for (std::size_t n = 0; n < numFrames; ++n)
    {
        INFO("frame " << n);
        REQUIRE(std::abs(second[n] - reference[n]) < 1e-6f);
    }
}
