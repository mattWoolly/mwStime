// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// rendercli smoke test (task plan/backlog/025-mwstime-render-cli.md): render a
// generated sine through one S1000 case, assert exit 0, and confirm the output
// parses via WavIo with the schedule-derived length. Also exercises a couple of
// the bad-arg / refusal exit paths.
//
// Self-contained: the golden corpus (task 026) does not exist yet, so the test
// writes its own tiny input WAV and its own one-case cases.json into the CTest
// working directory, then drives the real mwstime_render binary (path injected
// by CMake as MWSTIME_RENDER_BINARY) — the same exec path the golden runner
// uses (testing-strategy.md §4).
//
// Test-case names begin with the tag word "rendercli" so `ctest -R rendercli`
// matches (plan/backlog/README.md test-selection rules); the [rendercli] tag is
// carried for label-style selection too.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/WavIo.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"

#include "Args.h"

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

#ifndef MWSTIME_RENDER_BINARY
#error "MWSTIME_RENDER_BINARY must be defined by the build (path to mwstime_render)"
#endif

namespace {

using mws::core::AudioBuffer;
using mws::core::WavIo;

constexpr double kRate = 44100.0;

/// A deterministic full-band-safe sine for the smoke render.
AudioBuffer makeSine(double freqHz, std::size_t numFrames)
{
    AudioBuffer buffer(1, numFrames);
    buffer.sampleRate = kRate;
    auto view = buffer.channel(0);
    const double w = 2.0 * std::numbers::pi_v<double> * freqHz / kRate;
    for (std::size_t n = 0; n < numFrames; ++n)
        view[n] = static_cast<float>(std::sin(w * static_cast<double>(n)));
    return buffer;
}

/// Runs the CLI with a single space-joined argument string; returns the exit
/// code (already normalized to the process exit status).
int runCli(const std::string& args)
{
    const std::string cmd = std::string("\"") + MWSTIME_RENDER_BINARY + "\" " + args;
    const int raw = std::system(cmd.c_str());
#if defined(_WIN32)
    return raw;
#else
    if (raw == -1)
        return -1;
    return WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
}

void writeTextFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("rendercli: case mode renders an S1000 sine to a parseable WAV", "[rendercli]")
{
    // Inputs dir + cases file live in the CTest working directory.
    const std::string inputsDir = ".";
    const std::string inputFile = "rendercli_sine440.wav";
    const std::string casesFile = "rendercli_cases.json";
    const std::string outFile = "rendercli_out.wav";

    // A 0.5 s sine — long enough that the §3.4 scheduler launches several grains.
    const std::size_t srcFrames = 22050;
    {
        const AudioBuffer sine = makeSine(440.0, srcFrames);
        REQUIRE(WavIo::write(inputFile, sine, WavIo::BitDepth::Int16).ok());
    }

    // One S1000 CLASSIC case, float output for a lossless length check.
    writeTextFile(casesFile, R"({
  "version": 1,
  "cases": [
    {
      "id": "s1000_smoke",
      "input": "rendercli_sine440.wav",
      "model": "s1000",
      "params": { "timeFactor": 200, "cycleLen": 1000, "hopMode": "classic",
                  "character": false },
      "bitDepth": "float"
    }
  ]
})");

    const std::string args = "--case s1000_smoke --cases " + casesFile
                             + " --inputs-dir " + inputsDir + " --out " + outFile;
    REQUIRE(runCli(args) == 0);

    // The output must parse via WavIo.
    const WavIo::ReadResult out = WavIo::read(outFile);
    REQUIRE(out.ok());
    REQUIRE(out.buffer.numChannels() == 1);

    // Schedule-derived length: re-run the same render in-process and compare
    // the frame count (the renderer is the single source of the §3.4 schedule;
    // the CLI must produce that exact length — not round(N*T)).
    mws::engine::ParamSnapshot params;
    params.model = mws::model::ModelId::S1000;
    params.timeFactor = 200.0;
    params.cycleLen = 1000;
    params.hopMode = mws::engine::HopMode::Classic;
    params.character = false;

    const mws::engine::OfflineRenderer renderer;
    const AudioBuffer src = WavIo::read(inputFile).buffer;
    const mws::engine::RenderResult expected = renderer.render(src, params);
    REQUIRE(expected.ok());
    REQUIRE(static_cast<std::int64_t>(out.buffer.numFrames())
            == static_cast<std::int64_t>(expected.out.numFrames()));
    REQUIRE(out.buffer.numFrames() != srcFrames); // actually stretched
}

TEST_CASE("rendercli: direct mode renders all four shipping models", "[rendercli]")
{
    const std::string inputFile = "rendercli_directsrc.wav";
    {
        const AudioBuffer sine = makeSine(330.0, 16384);
        REQUIRE(WavIo::write(inputFile, sine, WavIo::BitDepth::Int16).ok());
    }

    for (const char* model : { "s900", "s950", "s1000", "s1100" })
    {
        const std::string outFile = std::string("rendercli_direct_") + model + ".wav";
        const std::string args = "--in " + inputFile + " --out " + outFile
                                 + " --model " + model + " --time-factor 150"
                                 + " --cycle-len 800 --hop-mode classic";
        INFO("model " << model);
        REQUIRE(runCli(args) == 0);
        REQUIRE(WavIo::read(outFile).ok());
    }
}

TEST_CASE("rendercli: bad arguments exit nonzero", "[rendercli]")
{
    // Unknown flag.
    REQUIRE(runCli("--in x.wav --out y.wav --bogus 1") != 0);
    // Missing required --out in direct mode.
    REQUIRE(runCli("--in x.wav") != 0);
    // Invalid enum value.
    REQUIRE(runCli("--in x.wav --out y.wav --model s9999") != 0);
    // Nonexistent input.
    REQUIRE(runCli("--in does_not_exist_12345.wav --out y.wav") != 0);
}

TEST_CASE("rendercli: --help exits zero", "[rendercli]")
{
    REQUIRE(runCli("--help") == 0);
}

TEST_CASE("rendercli: helpText documents every flag with its hardware unit", "[rendercli]")
{
    // The acceptance criterion ("--help documents every flag with hardware
    // units") is checked against the in-process help string so it is robust to
    // stdout capture. Every render flag and its unit token must be present.
    const std::string help = mwsrender::helpText();
    for (const char* token : {
             "--model", "--time-factor", "% of length", "--cycle-len",
             "samples at", "--stretch-mode", "--hop-mode", "--transpose",
             "st, -24", "--qual", "--width", "--material", "--bandwidth",
             "kHz", "--fs", "--character", "--norm", "--out-trim", "dB",
             "--bit-depth", "--case", "--cases", "--inputs-dir", "--out" })
    {
        INFO("missing token: " << token);
        REQUIRE(help.find(token) != std::string::npos);
    }
}
