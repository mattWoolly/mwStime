// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// gen_golden_inputs — the DETERMINISTIC generator for the committed golden
// input corpus (docs/design/testing-strategy.md §4 inputs list;
// task plan/backlog/026-golden-harness.md scope).
//
// LEGAL RULE (task 026 / testing-strategy.md §4): every input is generated from
// synthesis primitives here — there is NO copyrighted audio in the repo (no Amen
// break, no sampled master). `breakslice.wav` is an ORIGINAL synthetic 1-bar
// drum loop built from a fixed seed; it is not a recording of anything.
//
// All synthesis is done in double precision and written through
// mws::core::WavIo so the committed WAVs are byte-identical on any platform:
//   - no std::random distributions (those are not portable across libstdc++ /
//     libc++); the noise sources use a hand-rolled SplitMix64 + a fixed
//     [-1, 1) integer->double mapping,
//   - all maths uses <cmath> in double then narrows to float32 exactly the way
//     WavIo will quantize to 16-bit, so a same-seed re-run reproduces the file
//     bit-for-bit (acceptance criterion: "regenerable byte-identical").
//
// Outputs (tests/golden/inputs/, all <= 2 s; 44.1 kHz 16-bit mono unless noted):
//   sine440.wav        440 Hz sine                       (clean tone)
//   saw100.wav         100 Hz band-limited-ish saw       (auto-cycle lag 441)
//   clicktrain_2hz.wav 2 Hz impulse train                (transient smear/flam)
//   noiseburst.wav     gated white-noise burst           (broadband, seeded)
//   sweep20-20k.wav    20 Hz -> 20 kHz log sweep         (filter/alias signature)
//   breakslice.wav     original synth 1-bar drum loop    (seeded; NOT a sample)
//   breakslice_22k.wav the same loop synthesised at 22.05 kHz (rate matters —
//                      cycle length is in samples [AKZ §2.1 note])
//   stereo_pan.wav     2-ch, L/R decorrelated + a pan    (coherence input)
//
// Usage:  gen_golden_inputs <output-dir>
//   Writes (overwrites) every input WAV into <output-dir>. Exit 0 on success,
//   nonzero with a stderr message on the first write failure.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include "mws/core/Buffer.h"
#include "mws/core/WavIo.h"

namespace {

using mws::core::AudioBuffer;
using mws::core::WavIo;

constexpr double kPi = std::numbers::pi_v<double>;
constexpr double kRate44k = 44100.0;
constexpr double kRate22k = 22050.0;

// --- deterministic noise --------------------------------------------------
// SplitMix64 (public-domain reference constants); a hand-rolled PRNG so the
// corpus is byte-identical regardless of the standard-library RNG (std random
// distributions are NOT portable across implementations).
class SplitMix64
{
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /// Uniform double in [-1, 1) from the top 53 bits (no std distribution).
    double nextBipolar() noexcept
    {
        const std::uint64_t bits = next() >> 11;            // 53-bit mantissa
        const double unit = static_cast<double>(bits)
                            / static_cast<double>(1ull << 53); // [0, 1)
        return unit * 2.0 - 1.0;                            // [-1, 1)
    }

private:
    std::uint64_t state_;
};

/// Writes `samples` (one channel) at `rate` to `dir/name` as 16-bit mono.
/// Returns false (after printing to stderr) on a write failure.
bool writeMono16(const std::string& dir, const std::string& name,
                 const std::vector<double>& samples, double rate)
{
    AudioBuffer buf(1, samples.size());
    buf.sampleRate = rate;
    auto ch = buf.channel(0);
    for (std::size_t n = 0; n < samples.size(); ++n)
        ch[n] = static_cast<float>(samples[n]);

    const std::string path = dir + "/" + name;
    const WavIo::WriteResult res = WavIo::write(path, buf, WavIo::BitDepth::Int16);
    if (!res.ok())
    {
        std::cerr << "gen_golden_inputs: cannot write " << path << ": "
                  << res.error << "\n";
        return false;
    }
    std::cout << "  wrote " << path << " (" << samples.size() << " frames @ "
              << rate << " Hz)\n";
    return true;
}

/// Writes a 2-channel buffer as 16-bit stereo.
bool writeStereo16(const std::string& dir, const std::string& name,
                   const std::vector<double>& left,
                   const std::vector<double>& right, double rate)
{
    const std::size_t frames = left.size();
    AudioBuffer buf(2, frames);
    buf.sampleRate = rate;
    auto l = buf.channel(0);
    auto r = buf.channel(1);
    for (std::size_t n = 0; n < frames; ++n)
    {
        l[n] = static_cast<float>(left[n]);
        r[n] = static_cast<float>(right[n]);
    }

    const std::string path = dir + "/" + name;
    const WavIo::WriteResult res = WavIo::write(path, buf, WavIo::BitDepth::Int16);
    if (!res.ok())
    {
        std::cerr << "gen_golden_inputs: cannot write " << path << ": "
                  << res.error << "\n";
        return false;
    }
    std::cout << "  wrote " << path << " (" << frames << " frames x2 @ "
              << rate << " Hz)\n";
    return true;
}

// --- synthesis primitives --------------------------------------------------

/// Pure sine at `freq` Hz, amplitude `amp`, `frames` long at `rate`.
std::vector<double> makeSine(double freq, double amp, std::size_t frames,
                             double rate)
{
    std::vector<double> out(frames, 0.0);
    const double w = 2.0 * kPi * freq / rate;
    for (std::size_t n = 0; n < frames; ++n)
        out[n] = amp * std::sin(w * static_cast<double>(n));
    return out;
}

/// A naive (full-spectrum) sawtooth — deliberately rich so the auto-cycle
/// detector has a strong period (lag 441 at 100 Hz / 44.1 kHz) and the
/// character chains have high-band content to band-limit.
std::vector<double> makeSaw(double freq, double amp, std::size_t frames,
                            double rate)
{
    std::vector<double> out(frames, 0.0);
    const double period = rate / freq;
    for (std::size_t n = 0; n < frames; ++n)
    {
        const double phase = std::fmod(static_cast<double>(n), period) / period; // [0,1)
        out[n] = amp * (2.0 * phase - 1.0);                                       // -1..+1 ramp
    }
    return out;
}

/// An impulse train: a single full-scale sample every `1/freq` seconds, the
/// rest silence — maximally exposes a stretch engine's transient smear / flam.
std::vector<double> makeClickTrain(double freq, double amp, std::size_t frames,
                                   double rate)
{
    std::vector<double> out(frames, 0.0);
    const double period = rate / freq;
    // Place clicks at rounded integer positions so the spacing is deterministic.
    for (double pos = 0.0; pos < static_cast<double>(frames); pos += period)
    {
        const auto idx = static_cast<std::size_t>(std::llround(pos));
        if (idx < frames)
            out[idx] = amp;
    }
    return out;
}

/// A gated white-noise burst: full-scale noise that decays linearly over the
/// last quarter, seeded so it is reproducible.
std::vector<double> makeNoiseBurst(double amp, std::size_t frames, double rate,
                                   std::uint64_t seed)
{
    (void) rate;
    std::vector<double> out(frames, 0.0);
    SplitMix64 rng(seed);
    const auto fadeStart = (frames * 3) / 4;
    for (std::size_t n = 0; n < frames; ++n)
    {
        double g = 1.0;
        if (n >= fadeStart && frames > fadeStart)
            g = 1.0 - static_cast<double>(n - fadeStart)
                          / static_cast<double>(frames - fadeStart);
        out[n] = amp * g * rng.nextBipolar();
    }
    return out;
}

/// Exponential (log) frequency sweep from f0 to f1 over the whole buffer —
/// the classic filter/alias signature input.
std::vector<double> makeLogSweep(double f0, double f1, double amp,
                                 std::size_t frames, double rate)
{
    std::vector<double> out(frames, 0.0);
    const double T = static_cast<double>(frames) / rate; // seconds
    const double k = std::log(f1 / f0);
    for (std::size_t n = 0; n < frames; ++n)
    {
        const double t = static_cast<double>(n) / rate;
        // Instantaneous-phase integral of f0 * exp(k t / T):
        const double phase = 2.0 * kPi * f0 * T / k * (std::exp(k * t / T) - 1.0);
        out[n] = amp * std::sin(phase);
    }
    return out;
}

/// One percussive hit: a pitched body (decaying sine) + a noise transient,
/// shaped by an exponential decay envelope. Original synthesis — not a sample.
void addHit(std::vector<double>& buf, std::size_t start, double bodyFreq,
            double decaySec, double noiseAmt, double amp, double rate,
            SplitMix64& rng)
{
    const auto len = static_cast<std::size_t>(decaySec * rate);
    const double w = 2.0 * kPi * bodyFreq / rate;
    for (std::size_t i = 0; i < len; ++i)
    {
        const std::size_t n = start + i;
        if (n >= buf.size())
            break;
        const double t = static_cast<double>(i) / rate;
        const double env = std::exp(-t / (decaySec * 0.35));
        const double body = std::sin(w * static_cast<double>(i));
        const double noise = rng.nextBipolar();
        buf[n] += amp * env * ((1.0 - noiseAmt) * body + noiseAmt * noise);
    }
}

/// An ORIGINAL synthetic 1-bar drum loop (kick / snare / hats) at `rate`.
/// Built from addHit() with a fixed seed → byte-identical. This is NOT a
/// recording of any break; the "breakslice" name is just the role it plays.
/// A 1-bar loop at 120 BPM is 2.0 s exactly (the input cap), so we use 174 BPM
/// (a jungle tempo) → 1 bar ≈ 1.379 s, comfortably under 2 s and the
/// thematically correct tempo for the timestretch use-case [DRR F10].
std::vector<double> makeBreakSlice(double rate, std::uint64_t seed)
{
    constexpr double kBpm = 174.0;
    const double barSec = 4.0 * 60.0 / kBpm;                 // 4 beats per bar
    const auto frames = static_cast<std::size_t>(barSec * rate);
    std::vector<double> out(frames, 0.0);
    SplitMix64 rng(seed);

    const double sixteenth = barSec / 16.0; // 16 sixteenth-notes per bar
    auto pos = [&](int step) {
        return static_cast<std::size_t>(static_cast<double>(step) * sixteenth * rate);
    };

    // A simple amen-flavoured pattern WITHOUT copying the Amen — generic
    // kick/snare/hat placement on a 16-step grid. Body freqs are synthetic.
    // hats on every off-beat 8th, kick on 1 & the "and" of 2, snares on 2 & 4.
    for (int step = 0; step < 16; ++step)
        addHit(out, pos(step), 9000.0, 0.04, 0.85, 0.18, rate, rng); // closed hat
    addHit(out, pos(0),  55.0, 0.30, 0.10, 0.95, rate, rng);          // kick
    addHit(out, pos(6),  55.0, 0.30, 0.10, 0.80, rate, rng);          // kick (& of 2)
    addHit(out, pos(10), 60.0, 0.28, 0.10, 0.70, rate, rng);          // kick
    addHit(out, pos(4),  190.0, 0.22, 0.55, 0.90, rate, rng);         // snare (beat 2)
    addHit(out, pos(12), 190.0, 0.22, 0.55, 0.90, rate, rng);         // snare (beat 4)
    addHit(out, pos(14), 200.0, 0.16, 0.60, 0.45, rate, rng);         // ghost snare

    // Normalise to a safe peak (0.95) so 16-bit quantisation is deterministic
    // and no clipping happens at write time.
    double peak = 0.0;
    for (double s : out)
        peak = std::max(peak, std::abs(s));
    if (peak > 0.0)
    {
        const double g = 0.95 / peak;
        for (double& s : out)
            s *= g;
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: gen_golden_inputs <output-dir>\n";
        return 2;
    }
    const std::string dir = argv[1];
    std::cout << "gen_golden_inputs: writing golden input corpus to " << dir << "\n";

    // Durations: every input <= 2 s (testing-strategy.md §4). 1 s of 44.1 kHz
    // is 44100 frames; the saw period of 441 makes the auto-cycle target exact.
    constexpr std::size_t kOneSec44k = 44100;
    constexpr std::size_t kHalfSec44k = 22050;

    bool ok = true;
    ok = ok && writeMono16(dir, "sine440.wav",
                           makeSine(440.0, 0.8, kOneSec44k, kRate44k), kRate44k);
    ok = ok && writeMono16(dir, "saw100.wav",
                           makeSaw(100.0, 0.8, kOneSec44k, kRate44k), kRate44k);
    ok = ok && writeMono16(dir, "clicktrain_2hz.wav",
                           makeClickTrain(2.0, 0.95, kOneSec44k * 2, kRate44k),
                           kRate44k); // 2 s -> a few clicks
    ok = ok && writeMono16(dir, "noiseburst.wav",
                           makeNoiseBurst(0.8, kHalfSec44k, kRate44k, 0xB12715A11ull),
                           kRate44k);
    ok = ok && writeMono16(dir, "sweep20-20k.wav",
                           makeLogSweep(20.0, 20000.0, 0.8, kOneSec44k * 2,
                                        kRate44k),
                           kRate44k); // 2 s sweep
    ok = ok && writeMono16(dir, "breakslice.wav",
                           makeBreakSlice(kRate44k, 0x5EED1B2EA1500EEDull),
                           kRate44k);
    ok = ok && writeMono16(dir, "breakslice_22k.wav",
                           makeBreakSlice(kRate22k, 0x5EED1B2EA1500EEDull),
                           kRate22k);
    {
        // Stereo: decorrelated L/R hits (a different break seed per side) so a
        // coherence test on identical settings can confirm per-channel
        // phase-coherence while the two sides differ (testing-strategy.md §3.5).
        std::vector<double> left = makeBreakSlice(kRate44k, 0x10C0FFEE5EA12345ull);
        std::vector<double> right = makeBreakSlice(kRate44k, 0x2BADF00DCAFEBABEull);
        ok = ok && writeStereo16(dir, "stereo_pan.wav", left, right, kRate44k);
    }

    if (!ok)
    {
        std::cerr << "gen_golden_inputs: FAILED\n";
        return 1;
    }
    std::cout << "gen_golden_inputs: done\n";
    return 0;
}
