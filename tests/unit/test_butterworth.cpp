// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mws/core/Butterworth.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <utility>
#include <vector>

// Test-case names begin with the tag word so `ctest -R butterworth` matches
// (plan/backlog/README.md test-selection rules).

// ---------------------------------------------------------------------------
// Global operator-new replacement: counts every allocation in the test binary
// so the "setCutoff does not allocate" acceptance criterion (task 005) is
// verified at runtime, not just by inspection. Counting is branch-free and
// always on; tests snapshot the counter around the calls under test. The
// counter has external linkage (TestAllocationCounter.h) so other
// allocation-freedom tests (e.g. test_rtstretch_free.cpp, task 022) share
// this single replacement.
// ---------------------------------------------------------------------------
#include "TestAllocationCounter.h"

std::atomic<std::size_t> mwsTestGlobalAllocationCount{0};

void* operator new(std::size_t size)
{
    mwsTestGlobalAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size != 0 ? size : 1))
        return p;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
using mws::core::AudioView;
using mws::core::Butterworth6LP;

constexpr double kPi = 3.14159265358979323846;

/// Analytic 6th-order Butterworth low-pass magnitude in dB at f = ratio * fc:
/// |H|^2 = 1 / (1 + (f/fc)^12)  (maximally flat; -3.0103 dB at fc).
double analyticGainDb(double ratio)
{
    return -10.0 * std::log10(1.0 + std::pow(ratio, 12.0));
}

/// Black-box steady-state gain measurement: drive a fresh filter with a unit
/// sine at freqHz, discard the transient (60 cutoff periods >> the slowest
/// pole's decay), then estimate the output amplitude by correlation against
/// quadrature references over an integer number of signal cycles.
double measureGainDb(double freqHz, double cutoffHz, double sampleRate)
{
    Butterworth6LP filter;
    filter.setCutoff(cutoffHz, sampleRate);

    const double w = 2.0 * kPi * freqHz / sampleRate;
    const auto settle =
        static_cast<std::size_t>(std::ceil(60.0 * sampleRate / cutoffHz));
    const auto cycles = std::max<std::size_t>(
        64, static_cast<std::size_t>(std::ceil(16384.0 * freqHz / sampleRate)));
    const auto window = static_cast<std::size_t>(
        std::llround(static_cast<double>(cycles) * sampleRate / freqHz));

    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < settle + window; ++n)
    {
        const double phase = w * static_cast<double>(n);
        const float y = filter.processSample(static_cast<float>(std::sin(phase)));
        if (n >= settle)
        {
            re += y * std::sin(phase);
            im += y * std::cos(phase);
        }
    }

    const double amplitude =
        2.0 * std::sqrt(re * re + im * im) / static_cast<double>(window);
    return 20.0 * std::log10(amplitude);
}
} // namespace

// setCutoff must be callable per note-on on the audio thread
// (architecture.md §4.2): noexcept by contract.
static_assert(noexcept(std::declval<Butterworth6LP&>().setCutoff(1000.0, 48000.0)));
static_assert(noexcept(std::declval<Butterworth6LP&>().processSample(0.0f)));
static_assert(noexcept(std::declval<Butterworth6LP&>().reset()));

TEST_CASE("butterworth: -3 dB point lies within 1% of the requested cutoff",
          "[butterworth]")
{
    // fs >> fc so bilinear warping is negligible against the analytic check.
    const double fc = 1000.0;
    const double fs = 128.0 * fc;

    const double gainAtCutoff = measureGainDb(fc, fc, fs);
    REQUIRE_THAT(gainAtCutoff, Catch::Matchers::WithinAbs(-3.0103, 0.1));

    // Magnitude is monotonically decreasing, so bracketing the -3.01 dB level
    // between 0.99*fc and 1.01*fc pins the crossing to within 1% of fc.
    REQUIRE(measureGainDb(0.99 * fc, fc, fs) > -3.0103);
    REQUIRE(measureGainDb(1.01 * fc, fc, fs) < -3.0103);
}

TEST_CASE("butterworth: ~-36 dB/oct slope verified at 2x cutoff", "[butterworth]")
{
    const double fc = 1000.0;
    const double fs = 128.0 * fc;

    // On the stopband asymptote the analytic octave 2fc -> 4fc drops by
    // 10*log10((1+4^12)/(1+2^12)) = -36.12 dB.
    const double slopePerOctave = measureGainDb(4.0 * fc, fc, fs)
                                  - measureGainDb(2.0 * fc, fc, fs);
    REQUIRE_THAT(slopePerOctave, Catch::Matchers::WithinAbs(-36.12, 1.0));
}

TEST_CASE("butterworth: matches the analytic 6th-order response at three clock "
          "rates (cutoff tracks the clock)",
          "[butterworth]")
{
    // Clock rates spanning the S900/S950 7.5-48 kHz variable-clock range
    // (dsp-engine.md §8.1, deep-research-report.md Finding 4). Reconstruction
    // cutoff = clock / 2.5. The filter runs at the chain's oversampled rate;
    // here fs is high enough that bilinear warping stays well inside the
    // +/-0.5 dB budget even at 4*fc for the 48 kHz clock.
    const double fs = 64.0 * 48000.0;

    for (const double clockHz : { 7500.0, 24000.0, 48000.0 })
    {
        const double fc = clockHz / 2.5;
        for (const double ratio : { 1.0, 2.0, 4.0 })
        {
            INFO("clock " << clockHz << " Hz, cutoff " << fc << " Hz, ratio "
                          << ratio);
            REQUIRE_THAT(measureGainDb(ratio * fc, fc, fs),
                         Catch::Matchers::WithinAbs(analyticGainDb(ratio), 0.5));
        }
    }
}

TEST_CASE("butterworth: bounded output on noise across the clock range",
          "[butterworth]")
{
    const double fs = 4.0 * 48000.0; // oversampled-chain rate (dsp-engine.md §8.1)
    std::mt19937 rng(0x5eed0005);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    for (const double clockHz : { 7500.0, 20000.0, 48000.0 })
    {
        Butterworth6LP filter;
        filter.setCutoff(clockHz / 2.5, fs);

        bool allFinite = true;
        float maxAbs = 0.0f;
        for (int n = 0; n < (1 << 17); ++n)
        {
            const float y = filter.processSample(noise(rng));
            allFinite = allFinite && std::isfinite(y);
            maxAbs = std::max(maxAbs, std::abs(y));
        }

        INFO("clock " << clockHz << " Hz");
        REQUIRE(allFinite);
        REQUIRE(maxAbs < 8.0f); // |H| <= 1 everywhere; generous transient headroom
    }
}

TEST_CASE("butterworth: stable under repeated setCutoff recompute mid-stream",
          "[butterworth]")
{
    // Per-note retuning recomputes coefficients while audio keeps flowing
    // (architecture.md §4.2); state is retained across retunes, and the output
    // must stay bounded as the cutoff jumps across the whole clock range.
    const double fs = 4.0 * 48000.0;
    std::mt19937 rng(0xC10C0005);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::uniform_real_distribution<double> clock(7500.0, 48000.0);

    Butterworth6LP filter;
    filter.setCutoff(48000.0 / 2.5, fs);

    bool allFinite = true;
    float maxAbs = 0.0f;
    for (int n = 0; n < (1 << 16); ++n)
    {
        if (n % 64 == 0)
            filter.setCutoff(clock(rng) / 2.5, fs);

        const float y = filter.processSample(noise(rng));
        allFinite = allFinite && std::isfinite(y);
        maxAbs = std::max(maxAbs, std::abs(y));
    }

    REQUIRE(allFinite);
    REQUIRE(maxAbs < 8.0f);
}

TEST_CASE("butterworth: process(view) equals per-sample processSample",
          "[butterworth]")
{
    const double fs = 96000.0;
    const double fc = 4000.0;

    std::mt19937 rng(0xB10C0005);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    std::vector<float> blockInput(512);
    for (float& x : blockInput)
        x = noise(rng);
    std::vector<float> perSampleInput = blockInput;

    Butterworth6LP blockFilter;
    blockFilter.setCutoff(fc, fs);
    blockFilter.process(AudioView(blockInput.data(), blockInput.size()));

    Butterworth6LP sampleFilter;
    sampleFilter.setCutoff(fc, fs);
    for (std::size_t n = 0; n < perSampleInput.size(); ++n)
    {
        const float y = sampleFilter.processSample(perSampleInput[n]);
        REQUIRE(blockInput[n] == y); // bit-identical paths
    }
}

TEST_CASE("butterworth: reset clears state to silence", "[butterworth]")
{
    Butterworth6LP filter;
    filter.setCutoff(3000.0, 192000.0);

    std::mt19937 rng(0x0E5E0005);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (int n = 0; n < 4096; ++n)
        (void) filter.processSample(noise(rng));

    filter.reset();

    for (int n = 0; n < 256; ++n)
        REQUIRE(filter.processSample(0.0f) == 0.0f);
}

TEST_CASE("butterworth: setCutoff and processSample never allocate",
          "[butterworth]")
{
    Butterworth6LP filter;
    float sink = 0.0f;

    const std::size_t before = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    for (int i = 0; i < 1024; ++i)
    {
        filter.setCutoff(3000.0 + 16.0 * static_cast<double>(i), 192000.0);
        sink += filter.processSample(0.25f);
    }
    const std::size_t after = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    REQUIRE(after == before);
    REQUIRE(std::isfinite(sink));
}
