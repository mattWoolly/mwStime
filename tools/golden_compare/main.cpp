// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// golden_compare — the two-stage golden-render comparer with diagnostics
// (docs/design/testing-strategy.md §4 "Runner"; task plan/backlog/026-golden-harness.md).
//
// Stage 1 (gate): compare a candidate render against the blessed render.
//   --policy exact      : bit-exact (max abs diff == 0). Used for the CLASSIC
//                         stretch-only cases (transpose 0, character OFF), which
//                         are integer-only by construction and therefore
//                         cross-platform bit-exact (dsp-engine.md §3.2;
//                         architecture.md §2).
//   --policy tolerance  : |diff| <= --tol (default 1e-6) per sample. Used for the
//                         float-stage cases (filters/SRC/transpose/REVISED). On
//                         the REFERENCE platform (macOS arm64, where the goldens
//                         are blessed) these are bit-exact too, so the runner
//                         passes --tol 0; off-reference CI relaxes to 1e-6
//                         (testing-strategy.md §4 comparison policy).
//   A frame-count or channel-count mismatch is always a hard failure.
//
// Stage 2 (diagnostics): on ANY mismatch, print — to stdout so it lands in agent
// logs without DAW access (testing-strategy.md §4) —
//   * max abs diff and the sample index where it occurs,
//   * RMS diff,
//   * the first divergent sample (index + both values),
//   * the dominant spectral peak of the DIFFERENCE signal (the splice-comb
//     "metallic ring" signature; location in Hz + level in dB),
//   * a 1/3-octave spectral-difference table (band-summed |candidate|-|blessed|).
//
// JUCE-free; links only mwstime_core (WavIo). Exit 0 = match within policy;
// exit 1 = mismatch (diagnostics printed); exit 2 = usage / IO error.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iomanip>
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

struct Options {
    std::string candidate;
    std::string blessed;
    bool exact = false;      ///< --policy exact (bit-exact gate)
    double tol = 1e-6;       ///< --tol (tolerance policy; default 1e-6)
    bool ok = true;
    std::string error;
};

void printUsage()
{
    std::cerr <<
"usage: golden_compare --candidate <a.wav> --blessed <b.wav>\n"
"                      [--policy exact|tolerance] [--tol <maxAbs>]\n"
"\n"
"  --policy exact      bit-exact gate (max abs diff == 0) — CLASSIC stretch-only\n"
"  --policy tolerance  |diff| <= --tol per sample (default 1e-6) — float-stage\n"
"  --tol <maxAbs>      per-sample tolerance for the tolerance policy\n"
"\n"
"Exit 0 = match within policy; 1 = mismatch (diagnostics on stdout);\n"
"2 = usage / IO error.\n";
}

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string flag = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
            {
                o.ok = false;
                o.error = std::string("missing value for ") + name;
                return {};
            }
            return argv[++i];
        };

        if (flag == "--candidate")      o.candidate = value("--candidate");
        else if (flag == "--blessed")   o.blessed = value("--blessed");
        else if (flag == "--policy")
        {
            const std::string p = value("--policy");
            if (p == "exact")          o.exact = true;
            else if (p == "tolerance") o.exact = false;
            else { o.ok = false; o.error = "invalid --policy: '" + p + "'"; }
        }
        else if (flag == "--tol")
        {
            const std::string v = value("--tol");
            try { o.tol = std::stod(v); }
            catch (...) { o.ok = false; o.error = "invalid --tol: '" + v + "'"; }
        }
        else if (flag == "--help" || flag == "-h")
        {
            o.ok = false; // signal "print usage, exit 2" path
            o.error = "help";
        }
        else { o.ok = false; o.error = "unknown flag: '" + flag + "'"; }
        if (!o.ok)
            return o;
    }
    if (o.candidate.empty() || o.blessed.empty())
    {
        o.ok = false;
        o.error = "both --candidate and --blessed are required";
    }
    return o;
}

/// In-place iterative radix-2 FFT (forward). `data` length MUST be a power of 2.
void fft(std::vector<std::complex<double>>& data)
{
    const std::size_t n = data.size();
    if (n <= 1)
        return;
    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
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
                const std::complex<double> u = data[i + k];
                const std::complex<double> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

/// Largest power of two <= n (0 -> 0).
std::size_t floorPow2(std::size_t n)
{
    if (n == 0)
        return 0;
    std::size_t p = 1;
    while ((p << 1) != 0 && (p << 1) <= n)
        p <<= 1;
    return p;
}

/// Prints the dominant spectral peak of `diff` (splice-comb signature) and a
/// 1/3-octave |candidate|-|blessed| difference table. `rate` is the WAV's rate.
void printSpectralDiagnostics(const std::vector<double>& cand,
                              const std::vector<double>& bless,
                              const std::vector<double>& diff, double rate)
{
    const std::size_t fftLen = floorPow2(diff.size());
    if (fftLen < 4 || rate <= 0.0)
    {
        std::cout << "  spectral diag      : (signal too short for FFT)\n";
        return;
    }

    auto spectrumMag = [&](const std::vector<double>& sig) {
        std::vector<std::complex<double>> buf(fftLen);
        // Hann window for a clean magnitude spectrum.
        for (std::size_t i = 0; i < fftLen; ++i)
        {
            const double wnd = 0.5 - 0.5 * std::cos(2.0 * kPi
                                * static_cast<double>(i)
                                / static_cast<double>(fftLen - 1));
            buf[i] = std::complex<double>(sig[i] * wnd, 0.0);
        }
        fft(buf);
        std::vector<double> mag(fftLen / 2 + 1, 0.0);
        for (std::size_t k = 0; k <= fftLen / 2; ++k)
            mag[k] = std::abs(buf[k]);
        return mag;
    };

    const std::vector<double> magDiff = spectrumMag(diff);
    const std::vector<double> magCand = spectrumMag(cand);
    const std::vector<double> magBless = spectrumMag(bless);

    // Dominant peak of the difference spectrum (skip DC bin 0).
    std::size_t peakBin = 1;
    double peakMag = 0.0;
    for (std::size_t k = 1; k < magDiff.size(); ++k)
        if (magDiff[k] > peakMag)
        {
            peakMag = magDiff[k];
            peakBin = k;
        }
    const double binHz = rate / static_cast<double>(fftLen);
    const double peakHz = static_cast<double>(peakBin) * binHz;
    // Reference for dB: the larger of the two signal spectra at that bin (or
    // full-scale fallback) so the level is "how loud is the comb vs signal".
    const double refMag = std::max({ magCand[peakBin], magBless[peakBin], 1e-12 });
    const double peakDb = 20.0 * std::log10(std::max(peakMag, 1e-12) / refMag);
    std::cout << "  splice-comb peak   : " << std::fixed << std::setprecision(1)
              << peakHz << " Hz (bin " << peakBin << "), "
              << std::setprecision(2) << peakDb << " dB rel. signal\n";

    // 1/3-octave difference table: sum |magCand|-|magBless| per band, 20 Hz up.
    std::cout << "  1/3-octave diff (|cand|-|blessed| band energy):\n";
    const double third = std::pow(2.0, 1.0 / 3.0);
    double bandLo = 20.0;
    std::cout << std::scientific << std::setprecision(3);
    while (bandLo < rate / 2.0)
    {
        const double bandHi = bandLo * third;
        const auto kLo = static_cast<std::size_t>(std::ceil(bandLo / binHz));
        const auto kHi = std::min<std::size_t>(
            static_cast<std::size_t>(std::floor(bandHi / binHz)), fftLen / 2);
        double sumCand = 0.0;
        double sumBless = 0.0;
        for (std::size_t k = kLo; k <= kHi && k < magCand.size(); ++k)
        {
            sumCand += magCand[k];
            sumBless += magBless[k];
        }
        if (kHi >= kLo && kLo < magCand.size())
            std::cout << "    " << std::fixed << std::setprecision(0)
                      << std::setw(7) << bandLo << " Hz: "
                      << std::scientific << std::setprecision(3)
                      << (sumCand - sumBless) << "\n";
        bandLo = bandHi;
    }
}

} // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);
    if (!opt.ok)
    {
        if (opt.error != "help")
            std::cerr << "golden_compare: " << opt.error << "\n";
        printUsage();
        return 2;
    }

    const WavIo::ReadResult cand = WavIo::read(opt.candidate);
    if (!cand.ok())
    {
        std::cerr << "golden_compare: cannot read candidate '" << opt.candidate
                  << "': " << cand.error << "\n";
        return 2;
    }
    const WavIo::ReadResult bless = WavIo::read(opt.blessed);
    if (!bless.ok())
    {
        std::cerr << "golden_compare: cannot read blessed '" << opt.blessed
                  << "': " << bless.error << "\n";
        return 2;
    }

    const AudioBuffer& a = cand.buffer;
    const AudioBuffer& b = bless.buffer;

    // Shape mismatch is always a hard failure (with a clear message).
    if (a.numChannels() != b.numChannels() || a.numFrames() != b.numFrames())
    {
        std::cout << "golden_compare: MISMATCH (shape)\n"
                  << "  candidate : " << a.numChannels() << " ch x "
                  << a.numFrames() << " frames\n"
                  << "  blessed   : " << b.numChannels() << " ch x "
                  << b.numFrames() << " frames\n";
        return 1;
    }

    // Walk all samples (channel-major, matching AudioBuffer layout) and gather
    // the gate metrics + an interleaved-by-channel diff for spectral diag.
    const std::size_t nCh = a.numChannels();
    const std::size_t nFr = a.numFrames();

    double maxAbs = 0.0;
    std::size_t maxAbsIdx = 0;
    std::size_t maxAbsCh = 0;
    double sumSq = 0.0;
    bool foundFirst = false;
    std::size_t firstIdx = 0;
    std::size_t firstCh = 0;
    float firstA = 0.0f;
    float firstB = 0.0f;
    const double gateTol = opt.exact ? 0.0 : opt.tol;

    for (std::size_t ch = 0; ch < nCh; ++ch)
    {
        const auto va = a.channel(ch);
        const auto vb = b.channel(ch);
        for (std::size_t n = 0; n < nFr; ++n)
        {
            const double d = static_cast<double>(va[n]) - static_cast<double>(vb[n]);
            const double ad = std::abs(d);
            sumSq += d * d;
            if (ad > maxAbs)
            {
                maxAbs = ad;
                maxAbsIdx = n;
                maxAbsCh = ch;
            }
            if (!foundFirst && ad > gateTol)
            {
                foundFirst = true;
                firstIdx = n;
                firstCh = ch;
                firstA = va[n];
                firstB = vb[n];
            }
        }
    }

    const double rms = (nCh * nFr > 0)
        ? std::sqrt(sumSq / static_cast<double>(nCh * nFr)) : 0.0;
    const bool pass = maxAbs <= gateTol;

    if (pass)
    {
        std::cout << "golden_compare: MATCH ("
                  << (opt.exact ? "exact" : "tolerance")
                  << ", max abs diff " << std::scientific << std::setprecision(3)
                  << maxAbs << " <= " << gateTol << ")\n";
        return 0;
    }

    // --- Stage 2: diagnostics --------------------------------------------
    std::cout << "golden_compare: MISMATCH ("
              << (opt.exact ? "policy exact" : "policy tolerance")
              << ", tol " << std::scientific << std::setprecision(3) << gateTol
              << ")\n";
    std::cout << "  candidate          : " << opt.candidate << "\n";
    std::cout << "  blessed            : " << opt.blessed << "\n";
    std::cout << "  shape              : " << nCh << " ch x " << nFr << " frames @ "
              << std::fixed << std::setprecision(1) << a.sampleRate << " Hz\n";
    std::cout << "  max abs diff       : " << std::scientific << std::setprecision(6)
              << maxAbs << " at frame " << maxAbsIdx << " (ch " << maxAbsCh << ")\n";
    std::cout << "  RMS diff           : " << std::scientific << std::setprecision(6)
              << rms << "\n";
    if (foundFirst)
        std::cout << "  first divergence   : frame " << firstIdx << " (ch " << firstCh
                  << "): candidate " << std::scientific << std::setprecision(6)
                  << firstA << " vs blessed " << firstB << "\n";

    // Spectral diagnostics on channel 0 (the comb signature is per-channel).
    std::vector<double> dCand(nFr), dBless(nFr), dDiff(nFr);
    const auto va = a.channel(0);
    const auto vb = b.channel(0);
    for (std::size_t n = 0; n < nFr; ++n)
    {
        dCand[n] = static_cast<double>(va[n]);
        dBless[n] = static_cast<double>(vb[n]);
        dDiff[n] = dCand[n] - dBless[n];
    }
    printSpectralDiagnostics(dCand, dBless, dDiff, a.sampleRate);

    return 1;
}
