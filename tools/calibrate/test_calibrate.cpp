// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// calibrate self-test (task plan/backlog/026b acceptance criterion: "recovers
// planted SpliceCal values from synthetic captures"; docs/qa/hardware-capture-
// plan.md §4 tool self-test). Proves the fitter is correct BEFORE any real
// hardware capture exists.
//
// Method (the no-hardware proxy): render the corpus at a DELIBERATELY DIFFERENT
// planted SpliceCal — the planted renders stand in for "captures" — then run the
// fitter and assert it recovers the planted overlapF (Calibration), predicts the
// held-out captures at the frozen planted F (Validation, no search), and finds
// the S950 D-TIME mapping consistent. A synthetic capture is reproduced
// bit-exactly by the engine at its planted F, so the search has a perfect
// minimum (residual 0) exactly at the planted value.
//
// Test-case names begin with the tag word "calibrate" so `ctest -R calibrate`
// matches (plan/backlog/README.md test-selection rules); the [calibrate] tag is
// carried for label-style selection too. The binary path is injected by CMake
// (MWSTIME_CALIBRATE_BINARY) so the test also drives the real CLI exit paths.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Calibrate.h"
#include "Manifest.h"

#include "mws/core/Buffer.h"
#include "mws/core/WavIo.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"
#include "mws/stretch/CyclicEngine.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#ifndef MWSTIME_CALIBRATE_BINARY
#error "MWSTIME_CALIBRATE_BINARY must be defined by the build (calibrate path)"
#endif

namespace {

using mws::core::AudioBuffer;
using mws::core::WavIo;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::stretch::CyclicEngine;

constexpr double kRate = 44100.0;

/// The planted overlapF — deliberately different from the engine default 0.20
/// (dsp-engine.md §3.1) so a recovered value that just echoes the default would
/// fail the test.
constexpr float kPlantedOverlapF = 0.34f;

/// A deterministic sine (the cleanest splice-comb signature input).
AudioBuffer makeSine(double freqHz, std::size_t numFrames)
{
    AudioBuffer b(1, numFrames);
    b.sampleRate = kRate;
    auto v = b.channel(0);
    const double w = 2.0 * std::numbers::pi_v<double> * freqHz / kRate;
    for (std::size_t n = 0; n < numFrames; ++n)
        v[n] = static_cast<float>(std::sin(w * static_cast<double>(n)));
    return b;
}

/// A sparse impulse train (the stutter-schedule signature input).
AudioBuffer makeClicks(std::size_t numFrames, std::size_t spacing)
{
    AudioBuffer b(1, numFrames);
    b.sampleRate = kRate;
    auto v = b.channel(0);
    for (std::size_t n = 0; n < numFrames; n += spacing)
        v[n] = 0.9f;
    return b;
}

void writeTextFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Renders `src` through the full offline pipeline at the planted SpliceCal and
/// writes it as the capture `<id>.wav` (float32, lossless) — a synthetic
/// stand-in for a real hardware capture.
void writePlantedCapture(const std::string& dir, const std::string& id,
                         const AudioBuffer& src, const ParamSnapshot& params)
{
    CyclicEngine::SpliceCal planted;
    planted.overlapF = kPlantedOverlapF;
    const OfflineRenderer renderer(planted);
    const auto r = renderer.render(src, params);
    REQUIRE(r.ok());
    REQUIRE(WavIo::write(dir + "/" + id + ".wav", r.out, WavIo::BitDepth::Float32).ok());
}

int runCli(const std::string& args)
{
    const std::string cmd = std::string("\"") + MWSTIME_CALIBRATE_BINARY + "\" " + args;
    const int raw = std::system(cmd.c_str());
#if defined(_WIN32)
    return raw;
#else
    if (raw == -1) return -1;
    return WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
}

/// Builds the standard test corpus (committed-input stand-ins) + planted
/// captures + a manifest in the CTest working directory, and returns the
/// manifest text. inputsDir == capturesDir == "." (the working dir).
std::string buildFixture()
{
    // Inputs (stand-ins for the committed synthetic corpus). Write them, then
    // read them BACK and render the captures from the read-back buffers — the
    // fitter reads the same WAVs, so the capture and the fitter's candidate
    // share an identical (16-bit-quantized) source. (A real capture likewise
    // renders from the loaded sample, not from full-precision math.)
    REQUIRE(WavIo::write("cal_sine.wav", makeSine(440.0, 44100), WavIo::BitDepth::Int16).ok());
    REQUIRE(WavIo::write("cal_clicks.wav", makeClicks(44100, 4000), WavIo::BitDepth::Int16).ok());
    REQUIRE(WavIo::write("cal_saw.wav", makeSine(110.0, 44100), WavIo::BitDepth::Int16).ok());
    const AudioBuffer sine = WavIo::read("cal_sine.wav").buffer;
    const AudioBuffer clicks = WavIo::read("cal_clicks.wav").buffer;
    const AudioBuffer saw = WavIo::read("cal_saw.wav").buffer;

    // The manifest: disjoint calibration vs validation (by input file AND by
    // parameter point — docs/qa/hardware-capture-plan.md §3).
    const std::string manifest = R"({
  "version": 1,
  "cases": [
    {
      "id": "s1100_sine_300", "input": "cal_sine.wav", "model": "s1100", "set": "calibration",
      "params": { "cycleLen": 1000, "timeFactor": 300, "hopMode": "classic", "character": false },
      "bitDepth": "float"
    },
    {
      "id": "s1100_sine_cycle500_200", "input": "cal_sine.wav", "model": "s1100", "set": "calibration",
      "params": { "cycleLen": 500, "timeFactor": 200, "hopMode": "classic", "character": false },
      "bitDepth": "float"
    },
    {
      "id": "s1100_click_300", "input": "cal_clicks.wav", "model": "s1100", "set": "calibration",
      "params": { "cycleLen": 1000, "timeFactor": 300, "hopMode": "classic", "character": false },
      "bitDepth": "float"
    },
    {
      "id": "s950_sine_dtime1000_200", "input": "cal_sine.wav", "model": "s950", "set": "calibration",
      "params": { "cycleLen": 1000, "timeFactor": 200, "material": "pol2", "hopMode": "classic", "character": false },
      "bitDepth": "float"
    },
    {
      "id": "s950_sine_dtime500_200", "input": "cal_sine.wav", "model": "s950", "set": "calibration",
      "params": { "cycleLen": 500, "timeFactor": 200, "material": "pol2", "hopMode": "classic", "character": false },
      "bitDepth": "float"
    },
    {
      "id": "s1100_jungle_amen_300", "input": "cal_saw.wav", "model": "s1100", "set": "validation",
      "params": { "cycleLen": 1000, "timeFactor": 300, "hopMode": "classic", "character": false },
      "bitDepth": "float"
    },
    {
      "id": "s950_sine_dtime2000_200", "input": "cal_saw.wav", "model": "s950", "set": "validation",
      "params": { "cycleLen": 2000, "timeFactor": 200, "material": "pol2", "hopMode": "classic", "character": false },
      "bitDepth": "float"
    }
  ]
})";
    writeTextFile("cal_manifest.json", manifest);

    // Planted captures (rendered at kPlantedOverlapF) for every case.
    const mwscal::ManifestLoadResult loaded = mwscal::loadManifest(manifest);
    REQUIRE(loaded.ok());
    for (const mwscal::CalCase& c : loaded.cases)
    {
        const AudioBuffer& src =
            c.inputFile == "cal_sine.wav"   ? sine
            : c.inputFile == "cal_clicks.wav" ? clicks
                                              : saw;
        writePlantedCapture(".", c.id, src, c.params);
    }
    return manifest;
}

} // namespace

TEST_CASE("calibrate: fitter recovers the planted overlapF on calibration cases",
          "[calibrate]")
{
    const std::string manifest = buildFixture();
    const mwscal::ManifestLoadResult loaded = mwscal::loadManifest(manifest);
    REQUIRE(loaded.ok());

    mwscal::FitConfig cfg;
    cfg.inputsDir = ".";
    cfg.capturesDir = ".";
    // A grid that STRADDLES the planted value but does not land on it as an
    // endpoint or the default — recovering 0.34 from this grid is meaningful.
    cfg.gridLo = 0.10f;
    cfg.gridHi = 0.50f;
    cfg.gridStep = 0.01f;

    int calibrationCases = 0;
    for (const mwscal::CalCase& c : loaded.cases)
    {
        if (c.set != mwscal::CaseSet::Calibration)
            continue;
        ++calibrationCases;
        const mwscal::CaseFit fit = mwscal::fitCase(c, cfg);
        INFO("case " << c.id);
        REQUIRE(fit.ok);
        // Recovered overlapF == the planted value (within one grid step).
        REQUIRE_THAT(static_cast<double>(fit.overlapF),
                     Catch::Matchers::WithinAbs(kPlantedOverlapF, cfg.gridStep + 1e-4));
        // A synthetic capture is reproduced bit-exactly at its planted F.
        REQUIRE(fit.residual == 0.0);
        // The observables are the schedule geometry at the recovered F.
        REQUIRE(fit.obs.hopOut == mwscal::hopOutForCycle(fit.obs.cycleLen, fit.overlapF));
        REQUIRE(fit.obs.flutterRateHz > 0.0);
        REQUIRE(fit.obs.scheduleLen > 0);
    }
    REQUIRE(calibrationCases == 5); // the manifest's calibration set
}

TEST_CASE("calibrate: D-TIME mapping is consistent across disjoint D-TIME points",
          "[calibrate]")
{
    // The S950 D-TIME mapping (D-TIME ≡ cycle length in samples) is the
    // v1-freeze gate (dsp-engine.md §5). The fitter must find it CONSISTENT for
    // the S950 calibration captures (D-TIME 1000 and 500 — disjoint points).
    const std::string manifest = buildFixture();
    const mwscal::ManifestLoadResult loaded = mwscal::loadManifest(manifest);
    REQUIRE(loaded.ok());

    mwscal::FitConfig cfg;
    cfg.inputsDir = ".";
    cfg.capturesDir = ".";
    cfg.gridLo = 0.10f;
    cfg.gridHi = 0.50f;
    cfg.gridStep = 0.01f;

    int dtimeChecks = 0;
    for (const mwscal::CalCase& c : loaded.cases)
    {
        if (c.params.model != mws::model::ModelId::S950
            || c.set != mwscal::CaseSet::Calibration)
            continue;
        const mwscal::CaseFit fit = mwscal::fitCase(c, cfg);
        INFO("case " << c.id << " D-TIME " << c.params.cycleLen);
        REQUIRE(fit.ok);
        REQUIRE(fit.dTimeChecked);
        REQUIRE(fit.dTimeConsistent);
        // D-TIME maps straight to the cycle length the schedule used.
        REQUIRE(fit.obs.cycleLen == c.params.cycleLen);
        ++dtimeChecks;
    }
    REQUIRE(dtimeChecks == 2); // D-TIME 1000 and 500
}

TEST_CASE("calibrate: validation cases are PREDICTED at the frozen F (no overfitting)",
          "[calibrate]")
{
    // The held-out validation captures are predicted at the fitted (planted) F
    // WITHOUT searching — the no-overfitting rule (docs/qa/...-plan.md §3). With
    // the correct frozen F the prediction residual is 0; with a wrong frozen F
    // it is not (proving the prediction is genuine, not a free pass).
    const std::string manifest = buildFixture();
    const mwscal::ManifestLoadResult loaded = mwscal::loadManifest(manifest);
    REQUIRE(loaded.ok());

    mwscal::FitConfig good;
    good.inputsDir = ".";
    good.capturesDir = ".";
    good.frozenOverlapF = kPlantedOverlapF; // the value fitted on the calibration set

    mwscal::FitConfig wrong = good;
    wrong.frozenOverlapF = 0.10f; // a deliberately wrong frozen F

    int validationCases = 0;
    for (const mwscal::CalCase& c : loaded.cases)
    {
        if (c.set != mwscal::CaseSet::Validation)
            continue;
        ++validationCases;

        const mwscal::CaseFit ok = mwscal::fitCase(c, good);
        INFO("case " << c.id);
        REQUIRE(ok.ok);
        REQUIRE(ok.overlapF == kPlantedOverlapF); // frozen, not searched
        REQUIRE(ok.residual == 0.0);              // predicts the held-out capture

        const mwscal::CaseFit bad = mwscal::fitCase(c, wrong);
        REQUIRE(bad.ok);
        REQUIRE(bad.residual > 0.0); // a wrong frozen F does NOT predict it
    }
    REQUIRE(validationCases == 2);
}

TEST_CASE("calibrate: the CLI fits the calibration set and exits zero", "[calibrate]")
{
    buildFixture();
    const std::string args =
        "--manifest cal_manifest.json --inputs-dir . --captures . --set calibration";
    REQUIRE(runCli(args) == 0);
}

TEST_CASE("calibrate: the CLI predicts the validation set at the frozen F", "[calibrate]")
{
    buildFixture();
    std::string args =
        "--manifest cal_manifest.json --inputs-dir . --captures . --set validation"
        " --overlap-f 0.34";
    REQUIRE(runCli(args) == 0);
    // A wrong frozen F leaves the held-out captures unpredicted ⇒ exit 1 (gate FAIL).
    args = "--manifest cal_manifest.json --inputs-dir . --captures . --set validation"
           " --overlap-f 0.10";
    REQUIRE(runCli(args) == 1);
}

TEST_CASE("calibrate: bad arguments and missing files exit nonzero", "[calibrate]")
{
    REQUIRE(runCli("--manifest m.json --bogus 1") != 0);            // unknown flag
    REQUIRE(runCli("--inputs-dir . --captures .") != 0);           // missing --manifest
    REQUIRE(runCli("--manifest nope.json --inputs-dir . --captures .") == 2); // no file
    REQUIRE(runCli("--help") == 0);
}
