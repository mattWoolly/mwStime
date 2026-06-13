// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// mwstime-render — the JUCE-free command-line renderer (wav in -> wav out, all
// params) over mws::engine::OfflineRenderer + mws::core::WavIo
// (docs/design/architecture.md §2, §8; docs/design/testing-strategy.md §4;
// task plan/backlog/025-mwstime-render-cli.md).
//
// Two modes:
//   direct: --in/--out plus per-parameter flags (every render-relevant
//           dsp-engine.md §2 parameter has a flag; defaults per the §2 table),
//   case:   --case <id> --cases cases.json --inputs-dir <dir> --out <out.wav>
//           (schema in Cases.h; corpus populated by task 026).
//
// On success it prints the OfflineRenderer RenderInfo (achieved length/ratio,
// monoSummed, engine version) to stdout and writes the output WAV; on a refused
// render (NotEnoughMemory / unsupported model) or a bad argument it prints to
// stderr and exits nonzero. Identical invocations produce a bit-identical
// output file (the OfflineRenderer determinism rule, architecture.md §6).

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "mws/core/Version.h"
#include "mws/core/WavIo.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"

#include "Args.h"
#include "Cases.h"

namespace {

using mwsrender::CliArgs;

mws::core::WavIo::BitDepth toWavDepth(CliArgs::BitDepth depth)
{
    switch (depth)
    {
        case CliArgs::BitDepth::Int16:   return mws::core::WavIo::BitDepth::Int16;
        case CliArgs::BitDepth::Int24:   return mws::core::WavIo::BitDepth::Int24;
        case CliArgs::BitDepth::Int32:   return mws::core::WavIo::BitDepth::Int32;
        case CliArgs::BitDepth::Float32: return mws::core::WavIo::BitDepth::Float32;
    }
    return mws::core::WavIo::BitDepth::Int16;
}

const char* renderErrorMessage(mws::engine::RenderError error)
{
    switch (error)
    {
        case mws::engine::RenderError::None:             return "ok";
        case mws::engine::RenderError::NotEnoughMemory:  return "** NOT ENOUGH MEMORY **";
        case mws::engine::RenderError::Aborted:          return "render aborted";
        case mws::engine::RenderError::UnsupportedModel: return "model not supported at v1 (reserved slot)";
    }
    return "unknown render error";
}

/// Reads a whole text file (the cases file); `error` non-empty on failure.
std::string readTextFile(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open cases file: " + path;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Joins a directory and filename with a single '/' (avoids std::filesystem to
/// keep the diagnostic message stable across platforms).
std::string joinPath(const std::string& dir, const std::string& file)
{
    if (dir.empty())
        return file;
    if (dir.back() == '/' || dir.back() == '\\')
        return dir + file;
    return dir + "/" + file;
}

void printRenderInfo(const mws::engine::RenderInfo& info,
                     const mws::engine::ParamSnapshot& clamped,
                     const std::string& outPath)
{
    std::cout << "mwstime-render: render complete\n";
    std::cout << "  model            : " << mws::model::toString(clamped.model) << '\n';
    std::cout << "  output           : " << outPath << '\n';
    std::cout << "  outputFrames     : " << info.outputFrames << '\n';
    std::cout << "  outputSampleRate : " << info.outputSampleRate << " Hz\n";
    std::cout << "  achievedTimeFactor: " << info.achievedTimeFactorPct << " %\n";
    std::cout << "  monoSummed       : " << (info.monoSummed ? "yes" : "no") << '\n';
    std::cout << "  engineVersion    : " << mws::core::engineVersion()
              << " (hash 0x" << std::hex << info.engineVersionHash << std::dec << ")\n";
}

} // namespace

int main(int argc, char** argv)
{
    const mwsrender::ArgsParseResult parsed = mwsrender::parseArgs(argc, argv);
    if (!parsed.ok())
    {
        std::cerr << "mwstime-render: " << parsed.error << "\n";
        std::cerr << "Try 'mwstime-render --help'.\n";
        return 2;
    }

    const CliArgs& args = parsed.args;

    if (args.mode == mwsrender::Mode::Help)
    {
        std::cout << mwsrender::helpText();
        return 0;
    }

    if (args.mode == mwsrender::Mode::Version)
    {
        // One machine-readable line: "<version> 0x<hash>". The bless target
        // (tools/bless_goldens.sh) parses this to stamp MANIFEST.json so the
        // renderer remains the single authoritative source of the engine
        // version (task 026; architecture.md §6 render metadata).
        std::cout << mws::core::engineVersion() << " 0x" << std::hex
                  << mws::core::engineVersionHash() << std::dec << "\n";
        return 0;
    }

    // Resolve the input WAV path, parameter snapshot, and output bit depth for
    // the chosen mode.
    std::string inputPath;
    mws::engine::ParamSnapshot params;
    CliArgs::BitDepth bitDepth = args.bitDepth;

    if (args.mode == mwsrender::Mode::Case)
    {
        std::string fileError;
        const std::string text = readTextFile(args.casesPath, fileError);
        if (!fileError.empty())
        {
            std::cerr << "mwstime-render: " << fileError << "\n";
            return 2;
        }
        const mwsrender::CaseLoadResult loaded = mwsrender::loadCase(text, args.caseId);
        if (!loaded.ok())
        {
            std::cerr << "mwstime-render: " << loaded.error << "\n";
            return 2;
        }
        inputPath = joinPath(args.inputsDir, loaded.def.inputFile);
        params = loaded.def.params;
        // A --bit-depth flag overrides the case's; otherwise use the case's
        // declared depth (and fall back to the §2 default of 16-bit).
        bitDepth = loaded.def.bitDepthExplicit ? loaded.def.bitDepth : args.bitDepth;
    }
    else
    {
        inputPath = args.inPath;
        params = args.params;
    }

    // Read the source WAV.
    const mws::core::WavIo::ReadResult source = mws::core::WavIo::read(inputPath);
    if (!source.ok())
    {
        std::cerr << "mwstime-render: cannot read input '" << inputPath
                  << "': " << source.error << "\n";
        return 2;
    }

    // Render. OfflineRenderer forces SAMPLE mode and clamps params via
    // ModelSpec::clamp internally; we clamp a copy here only to echo the
    // achieved model for the RenderInfo block.
    const mws::model::ModelSpec& spec = mws::model::ModelSpec::get(params.model);
    const mws::engine::ParamSnapshot clamped = spec.clamp(params);

    const mws::engine::OfflineRenderer renderer;
    const mws::engine::RenderResult result = renderer.render(source.buffer, params);
    if (!result.ok())
    {
        std::cerr << "mwstime-render: render refused: "
                  << renderErrorMessage(result.error) << "\n";
        return 1;
    }

    // Write the output WAV.
    const mws::core::WavIo::WriteResult written =
        mws::core::WavIo::write(args.outPath, result.out, toWavDepth(bitDepth));
    if (!written.ok())
    {
        std::cerr << "mwstime-render: cannot write output '" << args.outPath
                  << "': " << written.error << "\n";
        return 2;
    }

    printRenderInfo(result.info, clamped, args.outPath);
    return 0;
}
