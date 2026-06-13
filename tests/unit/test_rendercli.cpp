// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// mwstime-render CLI smoke tests (plan/backlog/025-mwstime-render-cli.md;
// testing-strategy.md §4 runner contract). Contracts under test:
//   - direct mode: a generated sine through one S1000 CLASSIC case exits 0
//     and the output WAV parses via WavIo with the SCHEDULE-DERIVED length
//     (OfflineRenderer::predictedOutputFrames is the oracle — never
//     round(N*T), dsp-engine.md §3.4),
//   - direct mode succeeds for all four shipping models (acceptance
//     criterion),
//   - case mode resolves a cases.json case (schema defined in task 025,
//     corpus populated by 026) and is deterministic: two identical
//     invocations produce bit-identical files,
//   - --help exits 0; bad args exit nonzero.
//
// The binary path is injected by tests/CMakeLists.txt as MWS_RENDER_CLI_PATH
// ($<TARGET_FILE:mwstime-render>). Test-case names begin with the tag word
// so `ctest -R rendercli` matches (plan/backlog/README.md test-selection
// rules).

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/WavIo.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using mws::core::AudioBuffer;
using mws::core::WavIo;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::model::ModelId;

constexpr double kRate = 44100.0;

/// Fresh per-test scratch directory under the system temp dir, removed on
/// scope exit.
struct ScratchDir
{
    ScratchDir()
    {
        const auto unique = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path()
               / ("mwstime-rendercli-" + unique);
        fs::create_directories(path);
    }

    ~ScratchDir()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    fs::path path;
};

/// 0.5 s mono 440 Hz sine at 44.1 kHz, written as a 16-bit WAV (the golden
/// input convention, testing-strategy.md §4).
fs::path writeSineWav(const fs::path& dir, std::size_t numFrames = 22050)
{
    AudioBuffer buffer(1, numFrames);
    buffer.sampleRate = kRate;
    auto view = buffer.channel(0);
    const double w = 2.0 * std::numbers::pi_v<double> * 440.0 / kRate;
    for (std::size_t n = 0; n < numFrames; ++n)
        view[n] = 0.5f
                  * static_cast<float>(std::sin(w * static_cast<double>(n)));

    const fs::path path = dir / "sine440.wav";
    const auto written = WavIo::write(path, buffer, WavIo::BitDepth::Int16);
    REQUIRE(written.ok());
    return path;
}

/// Runs the CLI with `args`, capturing stdout/stderr into files in `dir`.
/// Returns the raw exit status of std::system (0 == success).
int runCli(const fs::path& dir, const std::string& args)
{
    const std::string command = std::string("\"") + MWS_RENDER_CLI_PATH
                                + "\" " + args + " > \""
                                + (dir / "stdout.txt").string() + "\" 2> \""
                                + (dir / "stderr.txt").string() + "\"";
    return std::system(command.c_str()); // NOLINT(cert-env33-c) test driver
}

std::string slurp(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST_CASE("rendercli: S1000 sine render exits 0 with the schedule-derived "
          "length",
          "[rendercli]")
{
    ScratchDir scratch;
    const fs::path in = writeSineWav(scratch.path);
    const fs::path out = scratch.path / "out.wav";

    const int rc = runCli(scratch.path,
                          "--in \"" + in.string() + "\" --out \""
                              + out.string()
                              + "\" --model s1000 --time-factor 300"
                                " --cycle-len 1000 --hop-mode classic"
                                " --transpose 0 --character on --norm off"
                                " --fs 44.1");
    INFO(slurp(scratch.path / "stderr.txt"));
    REQUIRE(rc == 0);

    // The output parses via WavIo...
    const auto rendered = WavIo::read(out);
    REQUIRE(rendered.ok());
    CHECK(rendered.buffer.sampleRate == kRate);
    CHECK(rendered.buffer.numChannels() == 1);

    // ...with the schedule-derived length (predictedOutputFrames is the
    // single oracle, itself pinned against the §3.4 schedule in task 020's
    // tests — never round(N*T)).
    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.timeFactor = 300.0;
    params.cycleLen = 1000;
    const std::int64_t expected =
        OfflineRenderer{}.predictedOutputFrames(22050, kRate, params);
    REQUIRE(expected > 0);
    CHECK(static_cast<std::int64_t>(rendered.buffer.numFrames()) == expected);

    // RenderInfo lands on stdout.
    const std::string stdoutText = slurp(scratch.path / "stdout.txt");
    CHECK(stdoutText.find("outputFrames: ") != std::string::npos);
    CHECK(stdoutText.find("achievedTimeFactorPct: ") != std::string::npos);
    CHECK(stdoutText.find("monoSummed: ") != std::string::npos);
    CHECK(stdoutText.find("engineVersionHash: ") != std::string::npos);
}

TEST_CASE("rendercli: direct mode succeeds for all four shipping models",
          "[rendercli]")
{
    ScratchDir scratch;
    const fs::path in = writeSineWav(scratch.path, 4410);

    for (const char* model : { "s900", "s950", "s1000", "s1100" })
    {
        DYNAMIC_SECTION("model " << model)
        {
            const fs::path out =
                scratch.path / (std::string(model) + ".wav");
            const int rc = runCli(scratch.path,
                                  "--in \"" + in.string() + "\" --out \""
                                      + out.string() + "\" --model " + model
                                      + " --time-factor 200");
            INFO(slurp(scratch.path / "stderr.txt"));
            REQUIRE(rc == 0);

            const auto rendered = WavIo::read(out);
            REQUIRE(rendered.ok());
            CHECK(rendered.buffer.numFrames() > 0);
        }
    }
}

TEST_CASE("rendercli: case mode resolves cases.json and renders "
          "deterministically",
          "[rendercli]")
{
    ScratchDir scratch;
    const fs::path inputsDir = scratch.path / "inputs";
    fs::create_directories(inputsDir);
    writeSineWav(inputsDir);

    const fs::path casesPath = scratch.path / "cases.json";
    {
        std::ofstream cases(casesPath, std::ios::binary);
        cases << R"({
  "cases": [
    {
      "id": "s1000_smoke300",
      "input": "sine440.wav",
      "bits": "float32",
      "params": {
        "model": "s1000",
        "time-factor": 300,
        "cycle-len": 1000,
        "hop-mode": "classic",
        "transpose": 0,
        "character": "on",
        "norm": "off"
      }
    }
  ]
})";
    }

    const fs::path outA = scratch.path / "a.wav";
    const fs::path outB = scratch.path / "b.wav";
    for (const fs::path& out : { outA, outB })
    {
        const int rc = runCli(scratch.path,
                              "--case s1000_smoke300 --cases \""
                                  + casesPath.string() + "\" --inputs-dir \""
                                  + inputsDir.string() + "\" --out \""
                                  + out.string() + "\"");
        INFO(slurp(scratch.path / "stderr.txt"));
        REQUIRE(rc == 0);
    }

    // Identical invocation => bit-identical output file (task 025
    // determinism contract).
    const std::string bytesA = slurp(outA);
    const std::string bytesB = slurp(outB);
    REQUIRE(!bytesA.empty());
    CHECK(bytesA == bytesB);

    // And the case render matches the direct-mode schedule-derived length.
    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.timeFactor = 300.0;
    params.cycleLen = 1000;
    const auto rendered = WavIo::read(outA);
    REQUIRE(rendered.ok());
    CHECK(static_cast<std::int64_t>(rendered.buffer.numFrames())
          == OfflineRenderer{}.predictedOutputFrames(22050, kRate, params));
}

TEST_CASE("rendercli: --help exits 0; bad args exit nonzero", "[rendercli]")
{
    ScratchDir scratch;

    CHECK(runCli(scratch.path, "--help") == 0);

    // Unknown flag.
    CHECK(runCli(scratch.path, "--frobnicate 12") != 0);
    // Missing --out.
    CHECK(runCli(scratch.path, "--in nope.wav") != 0);
    // Out-of-superset value.
    CHECK(runCli(scratch.path,
                 "--in nope.wav --out nope-out.wav --time-factor 9999")
          != 0);
    // Reserved model slot (ADR-004).
    CHECK(runCli(scratch.path,
                 "--in nope.wav --out nope-out.wav --model s3000")
          != 0);
}
