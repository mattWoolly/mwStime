// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Property tests for mws::model::VarClockChain — the §8.1 S900/S950
// variable-clock 12-bit character chain (plan/backlog/016-varclock-character-chain.md,
// written FIRST per docs/design/testing-strategy.md §3 item 9):
//   - distinct output values <= 4096 lattice pre-reconstruction,
//   - ZOH images present pre-filter, attenuated >= 24 dB post-Butterworth (PI),
//   - high-band energy tracks downward under pitch-down (clock-tracking, DRR F4),
//   - determinism,
// plus the early-risk CPU micro-benchmark (architecture.md §10 risk 3).

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/model/VarClockChain.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <set>
#include <vector>

// Test-case names begin with the tag word so `ctest -R varclock` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::core::ConstAudioView;
using mws::model::VarClockChain;

/// Deterministic broadband test signal: a sum of incommensurate sines, so no
/// value lands on the quantizer grid and many lattice codes get exercised.
void fillArbitrary(AudioView view, float amplitude)
{
    for (std::size_t i = 0; i < view.size(); ++i)
    {
        const auto n = static_cast<float>(i);
        view[i] = amplitude
                  * (0.55f * std::sin(0.0731f * n) + 0.30f * std::sin(0.4173f * n + 1.3f)
                     + 0.15f * std::sin(1.9241f * n + 0.7f));
    }
}

/// Deterministic full-band pseudo-noise in [-amplitude, amplitude] (LCG —
/// no <random> so the sequence is pinned across platforms/runs).
void fillNoise(AudioView view, float amplitude, std::uint64_t seed)
{
    std::uint64_t state = seed;
    for (float& sample : view)
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const auto top = static_cast<std::uint32_t>(state >> 40); // 24 bits
        sample = amplitude * (static_cast<float>(top) / 8388607.5f - 1.0f);
    }
}

void fillSine(AudioView view, double freqHz, double sampleRate, float amplitude)
{
    for (std::size_t i = 0; i < view.size(); ++i)
        view[i] = amplitude
                  * static_cast<float>(std::sin(2.0 * std::numbers::pi * freqHz
                                                * static_cast<double>(i) / sampleRate));
}

/// Goertzel single-bin amplitude estimate, normalized so a unit sine at
/// `freqHz` yields ~1.0 regardless of `n` or `sampleRate`. Analyses exactly
/// `n` samples starting at `offset` (caller picks `n` so freqHz is an exact
/// bin: freqHz * n / sampleRate integral — no leakage of the probe itself).
double goertzelAmplitude(ConstAudioView signal, std::size_t offset, std::size_t n,
                         double freqHz, double sampleRate)
{
    REQUIRE(offset + n <= signal.size());
    const double w = 2.0 * std::numbers::pi * freqHz / sampleRate;
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

double db(double amplitudeRatio)
{
    return 20.0 * std::log10(amplitudeRatio);
}
} // namespace

// ---------------------------------------------------------------------------
// testing-strategy.md §3.9: distinct output values <= 4096 lattice
// pre-reconstruction (after ingest + quantize).
// ---------------------------------------------------------------------------
TEST_CASE("varclock: ingest lands on the 12-bit lattice (<= 4096 distinct values)",
          "[varclock]")
{
    AudioBuffer input(1, 96000);
    fillArbitrary(input.channel(0), 0.95f);

    // S950 default-ish: bandwidth 10 kHz => f_s = 25 kHz (DRR F4: rate = 2.5 x BW).
    const double bandwidthKHz = 10.0;
    const AudioBuffer ingested =
        VarClockChain::ingest(input.channel(0), 44100.0, bandwidthKHz);

    REQUIRE(ingested.numChannels() == 1);
    REQUIRE(ingested.numFrames() > 0);
    REQUIRE(ingested.sampleRate == 25000.0); // f_s = 2.5 x bandwidth

    // Every sample is an exact integer multiple of the 12-bit step (1/2048
    // full scale — a power of two, exact in float32), and the lattice has at
    // most 4096 codes.
    const float step = 1.0f / 2048.0f;
    std::set<float> distinct;
    for (const float v : ingested.channel(0))
    {
        const float code = v / step;
        REQUIRE(code == std::floor(code));
        REQUIRE(v >= -1.0f);
        REQUIRE(v <= 1.0f);
        distinct.insert(v);
    }
    REQUIRE(distinct.size() <= 4096);
    REQUIRE(distinct.size() > 256); // the signal actually exercises the lattice
}

TEST_CASE("varclock: ingest model rate is 2.5 x bandwidth across the S900/S950 range",
          "[varclock]")
{
    REQUIRE(VarClockChain::modelRateFor(3.0) == 7500.0);   // min BW [MAN §2 p.74]
    REQUIRE(VarClockChain::modelRateFor(16.0) == 40000.0); // S900 max [MAN §1]
    REQUIRE(VarClockChain::modelRateFor(19.2) == 48000.0); // S950 max [MAN §2]
}

// ---------------------------------------------------------------------------
// testing-strategy.md §3.9: ZOH images present pre-filter (spectral peak near
// k*clock +/- f0), attenuated >= 24 dB post-Butterworth (PI bound, tunable).
// ---------------------------------------------------------------------------
TEST_CASE("varclock: ZOH images present pre-filter, attenuated >= 24 dB post-Butterworth",
          "[varclock]")
{
    // Post-stretch material: a 3 kHz sine living at f_s = 25 kHz (BW 10 kHz).
    const double fs = 25000.0;
    const double f0 = 3000.0;
    const double hostRate = 96000.0; // high host rate so the 22 kHz image is
                                     // comfortably inside host Nyquist
    AudioBuffer stretched(1, 25000); // 1 s at f_s
    fillSine(stretched.channel(0), f0, fs, 0.5f);

    // Pre-filter: the internal oversampled ZOH render. Images sit at
    // k*clock +/- f0 — first image pair at 22 kHz / 28 kHz for clock 25 kHz.
    const VarClockChain::OversampledZoh zoh =
        VarClockChain::renderZoh(stretched.channel(0), fs, 1.0, hostRate);

    REQUIRE(zoh.clockHz == fs);                       // clockRatio 1.0
    REQUIRE(zoh.rate >= 2.0 * hostRate);              // dsp-engine §8.1 note
    REQUIRE(zoh.rate >= 2.0 * zoh.clockHz);           // images representable
    REQUIRE(zoh.audio.numFrames() > 0);

    // Analysis windows chosen so every probe is an exact bin (rate is 4x host
    // = 384000; 0.1 s window => 10 Hz bins).
    const auto preN = static_cast<std::size_t>(zoh.rate / 10.0);
    const std::size_t preOffset = preN / 4; // skip start-up
    const ConstAudioView pre = zoh.audio.channel(0);
    const double preFund = goertzelAmplitude(pre, preOffset, preN, f0, zoh.rate);
    const double preImageLo =
        goertzelAmplitude(pre, preOffset, preN, zoh.clockHz - f0, zoh.rate);
    const double preImageHi =
        goertzelAmplitude(pre, preOffset, preN, zoh.clockHz + f0, zoh.rate);

    INFO("pre-filter fundamental " << preFund << ", image(22k) " << preImageLo
                                   << " (" << db(preImageLo / preFund) << " dB), image(28k) "
                                   << preImageHi << " (" << db(preImageHi / preFund)
                                   << " dB)");

    // Images PRESENT pre-filter: ZOH theory puts the 22 kHz image at
    // sinc(0.88) ~ -17.5 dB and the 28 kHz one at ~ -19.6 dB re the
    // fundamental. Require them within 30 dB — clearly present, with slack.
    REQUIRE(preFund > 0.3);
    REQUIRE(preImageLo > preFund * std::pow(10.0, -30.0 / 20.0));
    REQUIRE(preImageHi > preFund * std::pow(10.0, -30.0 / 20.0));

    // Post-Butterworth (full playback to host rate): the tracking 6th-order
    // filter at cutoff clock/2.5 = 10 kHz must knock the image down by at
    // least 24 dB relative to its pre-filter level (PI bound; theory says
    // ~41 dB at 2.2x cutoff).
    const AudioBuffer out =
        VarClockChain::playback(stretched.channel(0), fs, 1.0, hostRate);
    REQUIRE(out.numChannels() == 1);
    REQUIRE(out.sampleRate == hostRate);

    const auto postN = static_cast<std::size_t>(hostRate / 10.0); // 10 Hz bins
    const std::size_t postOffset = postN / 4;
    const ConstAudioView post = out.channel(0);
    const double postFund = goertzelAmplitude(post, postOffset, postN, f0, hostRate);
    const double postImageLo =
        goertzelAmplitude(post, postOffset, postN, zoh.clockHz - f0, hostRate);

    INFO("post-filter fundamental " << postFund << ", image(22k) " << postImageLo
                                    << " (" << db(postImageLo / preImageLo)
                                    << " dB re pre-filter image)");

    REQUIRE(postImageLo < preImageLo * std::pow(10.0, -24.0 / 20.0));

    // And the passband survives: the 3 kHz fundamental is well below the
    // 10 kHz cutoff, so it must come through within 3 dB of its pre-filter
    // level (ZOH droop at 0.12 f_clk is only ~0.2 dB).
    REQUIRE(postFund > preFund * std::pow(10.0, -3.0 / 20.0));
}

// ---------------------------------------------------------------------------
// testing-strategy.md §3.9: high-band energy tracks downward under pitch-down
// (clockRatio 0.5 => output energy above clock/2 drops vs clockRatio 1.0) —
// the clock-tracking property, DRR Finding 4. This is what a static bitcrush
// (RX950-style) gets wrong: its filter does NOT follow the notes [DRR F7].
// ---------------------------------------------------------------------------
TEST_CASE("varclock: high-band energy tracks the clock downward under pitch-down",
          "[varclock]")
{
    const double fs = 25000.0; // BW 10 kHz
    const double hostRate = 96000.0;

    AudioBuffer stretched(1, 25000); // 1 s of full-band material at f_s
    fillNoise(stretched.channel(0), 0.5f, 0x5eed5eedull);

    const AudioBuffer outUnity =
        VarClockChain::playback(stretched.channel(0), fs, 1.0, hostRate);
    const AudioBuffer outDown =
        VarClockChain::playback(stretched.channel(0), fs, 0.5, hostRate);

    // Pitch-down halves the clock, so the same samples take twice as long.
    REQUIRE(outDown.numFrames() > static_cast<std::size_t>(
                1.9 * static_cast<double>(outUnity.numFrames())));

    // Per-sample band energy 6..12 kHz — above the pitched-down clock/2
    // (6.25 kHz) and pitched-down cutoff (5 kHz), below the unity cutoff
    // tail. Probes at exact 10 Hz bins; energies are window-normalized, so
    // the differing output lengths do not bias the comparison.
    const auto measureBand = [&](const AudioBuffer& out) {
        const auto n = static_cast<std::size_t>(hostRate / 10.0);
        const std::size_t offset = n / 4;
        double energy = 0.0;
        for (double f = 6000.0; f <= 12000.0; f += 500.0)
        {
            const double a = goertzelAmplitude(out.channel(0), offset, n, f, hostRate);
            energy += a * a;
        }
        return energy;
    };

    const double bandUnity = measureBand(outUnity);
    const double bandDown = measureBand(outDown);

    INFO("6-12 kHz band energy: unity clock " << bandUnity << ", half clock "
                                              << bandDown);
    REQUIRE(bandUnity > 0.0);
    // Clock-tracking: with the clock (and the tracking cutoff, 10 kHz -> 5 kHz)
    // halved, high-band energy must drop hard — require at least 6 dB
    // (theory: > 20 dB across the band).
    REQUIRE(bandDown < 0.25 * bandUnity);
}

// ---------------------------------------------------------------------------
// testing-strategy.md §3 (determinism, architecture.md §7): identical inputs
// yield bit-identical outputs, run to run.
// ---------------------------------------------------------------------------
TEST_CASE("varclock: ingest + playback are bit-deterministic", "[varclock]")
{
    AudioBuffer input(1, 44100);
    fillArbitrary(input.channel(0), 0.9f);

    const auto runOnce = [&] {
        const AudioBuffer ingested =
            VarClockChain::ingest(input.channel(0), 44100.0, 12.0);
        return VarClockChain::playback(ingested.channel(0), VarClockChain::modelRateFor(12.0),
                                       std::pow(2.0, -3.0 / 12.0), 44100.0);
    };

    const AudioBuffer a = runOnce();
    const AudioBuffer b = runOnce();

    REQUIRE(a.numFrames() == b.numFrames());
    REQUIRE(a.sampleRate == b.sampleRate);
    for (std::size_t i = 0; i < a.numFrames(); ++i)
        REQUIRE(a.channel(0)[i] == b.channel(0)[i]); // bitwise equal
}

TEST_CASE("varclock: empty input yields empty output", "[varclock]")
{
    const AudioBuffer empty(1, 0);
    const AudioBuffer ingested = VarClockChain::ingest(empty.channel(0), 44100.0, 10.0);
    REQUIRE(ingested.numFrames() == 0);
    const AudioBuffer out = VarClockChain::playback(empty.channel(0), 25000.0, 1.0, 44100.0);
    REQUIRE(out.numFrames() == 0);
    REQUIRE(out.sampleRate == 44100.0);
}

// ---------------------------------------------------------------------------
// Early-risk CPU datum (architecture.md §10 risk 3; task 016 acceptance):
// cost of 1 s of audio through the full chain. Hidden behind [!benchmark] so
// ctest stays fast; run manually with
//   ./build/default/tests/mwstime_tests "[varclock][!benchmark]"
// and the measured cost is recorded in the PR description.
// ---------------------------------------------------------------------------
TEST_CASE("varclock: benchmark — 1 s of audio through ingest + playback",
          "[!benchmark][varclock]")
{
    AudioBuffer input(1, 44100); // 1 s at 44.1 kHz host rate
    fillArbitrary(input.channel(0), 0.9f);
    const double bandwidthKHz = 10.0;
    const double fs = VarClockChain::modelRateFor(bandwidthKHz);

    BENCHMARK("ingest 1 s (44.1k -> 25k, 12-bit)")
    {
        return VarClockChain::ingest(input.channel(0), 44100.0, bandwidthKHz);
    };

    const AudioBuffer ingested =
        VarClockChain::ingest(input.channel(0), 44100.0, bandwidthKHz);

    BENCHMARK("playback 1 s (ZOH @4x, tracking BW6, decimate to 44.1k)")
    {
        return VarClockChain::playback(ingested.channel(0), fs, 1.0, 44100.0);
    };

    BENCHMARK("playback 1 s, pitch-down 12 st (clockRatio 0.5)")
    {
        return VarClockChain::playback(ingested.channel(0), fs, 0.5, 44100.0);
    };
}
