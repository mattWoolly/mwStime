// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// mwstime-render — JUCE-free CLI over OfflineRenderer + WavIo: wav in, wav
// out, all params (docs/design/architecture.md §2 tools layer; task
// plan/backlog/025-mwstime-render-cli.md). The driver for the golden-render
// regression suite (testing-strategy.md §4) and the agent debugging tool —
// DSP work never requires a host.
//
// Two modes:
//   direct:  mwstime-render --in in.wav --out out.wav [param flags]
//   case:    mwstime-render --case <id> [--cases tests/golden/cases.json]
//                            [--inputs-dir <dir>] --out out.wav
//
// cases.json schema (defined HERE; the corpus is populated by task 026):
//   {
//     "cases": [
//       {
//         "id":     "s1100_jungle300",          // unique case id (string)
//         "input":  "breakslice.wav",           // WAV in --inputs-dir (or an
//                                               //   absolute path)
//         "bits":   "float32",                  // optional output depth:
//                                               //   16|24|32|float32
//         "params": {                           // optional; keys are the CLI
//           "model": "s1100",                   //   long-flag names, values
//           "time-factor": 300,                 //   use the same syntax as
//           "cycle-len": 1000,                  //   the flags (JSON numbers
//           "hop-mode": "classic",              //   for numerics, strings
//           "character": "on",                  //   for enums/switches).
//           "norm": "off"                       //   Omitted keys take the
//         }                                     //   dsp-engine.md §2 defaults
//       }
//     ]
//   }
//
// Exit codes: 0 success; 1 runtime refusal/failure (NotEnoughMemory, file
// IO); 2 bad arguments / bad cases file. Determinism contract: an identical
// invocation produces a bit-identical output file (OfflineRenderer is
// deterministic, architecture.md §6; WavIo conversion rules are pinned).

#include "MiniJson.h"

#include "mws/core/Buffer.h"
#include "mws/core/Version.h"
#include "mws/core/WavIo.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace json = mws::tools::json;
using mws::core::WavIo;
using mws::engine::ParamSnapshot;

constexpr std::string_view kDefaultCasesPath = "tests/golden/cases.json";

// ---------------------------------------------------------------------------
// help
// ---------------------------------------------------------------------------

void printHelp()
{
    std::printf(R"(mwstime-render -- Akai S-series timestretch renderer (mwstime-core, JUCE-free)

Renders a WAV through the hardware-faithful offline pipeline (ingest
character -> per-model stretch -> transpose -> playback character ->
optional normalize) and prints the achieved RenderInfo. Deterministic:
an identical invocation produces a bit-identical output file.

Usage:
  mwstime-render --in <in.wav> --out <out.wav> [param flags]
  mwstime-render --case <id> [--cases <cases.json>] [--inputs-dir <dir>]
                 --out <out.wav> [--bits <depth>]
  mwstime-render --help

Modes / IO:
  --in <path>          input WAV: 16/24/32-bit int or 32-bit float PCM,
                       mono or stereo (direct mode)
  --out <path>         output WAV path (required in both modes)
  --case <id>          render one golden case by id (case mode)
  --cases <path>       cases file (default: tests/golden/cases.json)
  --inputs-dir <dir>   directory holding case input WAVs
                       (default: <cases-file dir>/inputs)
  --bits <16|24|32|float32>
                       output WAV bit depth (default: float32, lossless --
                       the golden-compare depth; a case's "bits" field
                       overrides this in case mode)

Parameters (docs/design/dsp-engine.md section 2 -- flags mirror the LCD
parameter table; defaults are the table defaults; values outside the
superset range are rejected; the active model additionally clamps to its
own hardware range at the engine and the achieved figures are printed):
  --model <s900|s950|s1000|s1100>
                       MODEL (default: s1000). s3000 is a reserved v1.1
                       slot and is refused.
  --time-factor <pct>  TIME FACTOR, percent of original length,
                       25.00-2000.00 (default: 100). S950 engine-clamps
                       the high end to 999. Integer-quantized by CLASSIC
                       timing (the achieved % is printed).
  --cycle-len <samples>
                       CYCLE LENGTH (S1000/S1100) / D-TIME (S950), in
                       samples at the model rate, 20-2000 (default: 1000)
  --stretch-mode <cyclic|intell>
                       STRETCH MODE (default: cyclic). INTELL is deferred
                       to v1.1 and renders as CYCLIC at v1.
  --hop-mode <classic|revised>
                       TIMING (default: classic, the hardware-faithful
                       integer hop; revised = fractional, sample-exact)
  --transpose <st>     TRANSPOSE, semitones, -24.00-+24.00 in 1-cent
                       steps (default: 0). On the S900 varispeed couples
                       time and pitch (the printed achieved % folds it in).
  --qual <1-99>        QUAL, INTELL decisions index (default: 10).
                       Inert at v1 (INTELL deferred); accepted so case
                       files round-trip it.
  --width <1-99>       WIDTH, INTELL crossfade index (default: 10).
                       Inert at v1; accepted for round-trip.
  --material <mon1|pol2>
                       S950 MON1/POL2 material switch (default: pol2)
  --bandwidth <kHz>    BANDWIDTH, kHz, 3.0-19.2 in 0.1 steps (S900/S950;
                       model rate = 2.5 x bandwidth; S900 engine-clamps
                       to 16.0; default: 19.2)
  --fs <44.1|22.05>    FS, S1000/S1100 fixed model rate, kHz
                       (default: 44.1)
  --character <on|off> CHARACTER, per-model hardware character chain
                       (default: on)
  --norm <on|off>      NORM, peak-normalize the render to the SOURCE peak
                       (default: off -- authentic)

Output: prints the RenderInfo (achieved length/ratio, mono summing, engine
version) to stdout. Refusals (e.g. the 10-minute NOT ENOUGH MEMORY cap) and
bad arguments exit nonzero with a message on stderr.
)");
}

// ---------------------------------------------------------------------------
// small parsing helpers
// ---------------------------------------------------------------------------

/// Whole-string double parse; nullopt on any trailing garbage.
std::optional<double> parseDouble(std::string_view text)
{
    const std::string token(text);
    if (token.empty())
        return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size())
        return std::nullopt;
    return value;
}

/// Whole-string int parse via the double path (rejects fractions).
std::optional<int> parseInt(std::string_view text)
{
    const std::optional<double> value = parseDouble(text);
    if (!value || *value != static_cast<double>(static_cast<int>(*value)))
        return std::nullopt;
    return static_cast<int>(*value);
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::optional<bool> parseOnOff(std::string_view text)
{
    const std::string v = lower(text);
    if (v == "on")
        return true;
    if (v == "off")
        return false;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// the shared param-setting table (direct flags AND case-file "params" keys)
// ---------------------------------------------------------------------------

/// Applies one named parameter (CLI long-flag name, dsp-engine.md §2 row) to
/// the snapshot. Returns an error message, or empty on success. Numeric
/// values outside the host-facing superset range are rejected here; the
/// per-model engine clamp (ModelSpec::clamp) still applies inside.
std::string applyParam(ParamSnapshot& params, std::string_view name,
                       std::string_view value)
{
    namespace superset = mws::model::superset;
    using mws::engine::HopMode;
    using mws::engine::Material;
    using mws::engine::SampleRateSel;
    using mws::engine::StretchMode;
    using mws::model::ModelId;

    const auto bad = [&](std::string_view expected) {
        return "--" + std::string(name) + ": invalid value '"
               + std::string(value) + "' (expected " + std::string(expected)
               + ")";
    };

    if (name == "model")
    {
        const std::string v = lower(value);
        if (v == "s900")
            params.model = ModelId::S900;
        else if (v == "s950")
            params.model = ModelId::S950;
        else if (v == "s1000")
            params.model = ModelId::S1000;
        else if (v == "s1100")
            params.model = ModelId::S1100;
        else if (v == "s3000")
            return "--model: s3000 is a reserved v1.1 slot (ADR-004) and "
                   "cannot render";
        else
            return bad("s900|s950|s1000|s1100");
        return {};
    }
    if (name == "time-factor")
    {
        const auto v = parseDouble(value);
        if (!v || *v < superset::kTimeFactorMin || *v > superset::kTimeFactorMax)
            return bad("25.00-2000.00 %");
        params.timeFactor = *v;
        return {};
    }
    if (name == "cycle-len")
    {
        const auto v = parseInt(value);
        if (!v || *v < superset::kCycleLenMin || *v > superset::kCycleLenMax)
            return bad("20-2000 samples");
        params.cycleLen = *v;
        return {};
    }
    if (name == "stretch-mode")
    {
        const std::string v = lower(value);
        if (v == "cyclic")
            params.stretchMode = StretchMode::Cyclic;
        else if (v == "intell")
            params.stretchMode = StretchMode::Intell;
        else
            return bad("cyclic|intell");
        return {};
    }
    if (name == "hop-mode")
    {
        const std::string v = lower(value);
        if (v == "classic")
            params.hopMode = HopMode::Classic;
        else if (v == "revised")
            params.hopMode = HopMode::Revised;
        else
            return bad("classic|revised");
        return {};
    }
    if (name == "transpose")
    {
        const auto v = parseDouble(value);
        if (!v || *v < superset::kTransposeMin || *v > superset::kTransposeMax)
            return bad("-24.00-+24.00 semitones");
        params.transpose = *v;
        return {};
    }
    if (name == "qual")
    {
        const auto v = parseInt(value);
        if (!v || *v < superset::kQualMin || *v > superset::kQualMax)
            return bad("1-99");
        params.qual = *v;
        return {};
    }
    if (name == "width")
    {
        const auto v = parseInt(value);
        if (!v || *v < superset::kWidthMin || *v > superset::kWidthMax)
            return bad("1-99");
        params.width = *v;
        return {};
    }
    if (name == "material")
    {
        const std::string v = lower(value);
        if (v == "mon1")
            params.material = Material::Mon1;
        else if (v == "pol2")
            params.material = Material::Pol2;
        else
            return bad("mon1|pol2");
        return {};
    }
    if (name == "bandwidth")
    {
        const auto v = parseDouble(value);
        if (!v || *v < superset::kBandwidthMinKHz
            || *v > superset::kBandwidthMaxKHz)
            return bad("3.0-19.2 kHz");
        params.bandwidth = *v;
        return {};
    }
    if (name == "fs")
    {
        const std::string v = lower(value);
        if (v == "44.1" || v == "44100")
            params.sampleRateSel = SampleRateSel::Fs44100;
        else if (v == "22.05" || v == "22050")
            params.sampleRateSel = SampleRateSel::Fs22050;
        else
            return bad("44.1|22.05 kHz");
        return {};
    }
    if (name == "character")
    {
        const auto v = parseOnOff(value);
        if (!v)
            return bad("on|off");
        params.character = *v;
        return {};
    }
    if (name == "norm")
    {
        const auto v = parseOnOff(value);
        if (!v)
            return bad("on|off");
        params.norm = *v;
        return {};
    }
    return "unknown parameter '--" + std::string(name) + "' (see --help)";
}

std::optional<WavIo::BitDepth> parseBits(std::string_view value)
{
    const std::string v = lower(value);
    if (v == "16")
        return WavIo::BitDepth::Int16;
    if (v == "24")
        return WavIo::BitDepth::Int24;
    if (v == "32")
        return WavIo::BitDepth::Int32;
    if (v == "float32" || v == "f32" || v == "float")
        return WavIo::BitDepth::Float32;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// argv parsing
// ---------------------------------------------------------------------------

struct CliOptions {
    std::string inPath;     ///< direct mode
    std::string caseId;     ///< case mode
    std::string casesPath{kDefaultCasesPath};
    std::string inputsDir;  ///< empty => <cases dir>/inputs
    std::string outPath;
    WavIo::BitDepth bits = WavIo::BitDepth::Float32;
    ParamSnapshot params;   ///< pluginMode is forced to SAMPLE by the renderer
    bool help = false;
};

/// Parses argv. Returns an error message, or empty on success.
std::string parseArgs(int argc, char** argv, CliOptions& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            options.help = true;
            return {};
        }
        if (!arg.starts_with("--"))
            return "unexpected argument '" + std::string(arg)
                   + "' (flags start with --; see --help)";

        const std::string_view name = arg.substr(2);
        if (i + 1 >= argc)
            return "--" + std::string(name) + ": missing value";
        const std::string_view value = argv[++i];

        if (name == "in")
            options.inPath = value;
        else if (name == "out")
            options.outPath = value;
        else if (name == "case")
            options.caseId = value;
        else if (name == "cases")
            options.casesPath = value;
        else if (name == "inputs-dir")
            options.inputsDir = value;
        else if (name == "bits")
        {
            const auto bits = parseBits(value);
            if (!bits)
                return "--bits: invalid value '" + std::string(value)
                       + "' (expected 16|24|32|float32)";
            options.bits = *bits;
        }
        else
        {
            if (const std::string err = applyParam(options.params, name, value);
                !err.empty())
                return err;
        }
    }

    if (options.help)
        return {};
    if (options.inPath.empty() && options.caseId.empty())
        return "one of --in (direct mode) or --case (case mode) is required "
               "(see --help)";
    if (!options.inPath.empty() && !options.caseId.empty())
        return "--in and --case are mutually exclusive";
    if (options.outPath.empty())
        return "--out is required";
    return {};
}

// ---------------------------------------------------------------------------
// case mode
// ---------------------------------------------------------------------------

/// JSON scalar -> the flag-value syntax applyParam expects.
std::string jsonScalarToString(const json::Value& value)
{
    switch (value.type)
    {
        case json::Value::Type::String: return value.string;
        case json::Value::Type::Bool:   return value.boolean ? "on" : "off";
        case json::Value::Type::Number:
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.10g", value.number);
            return buffer;
        }
        default: return {};
    }
}

/// Resolves --case against the cases file: fills options.inPath (from the
/// case's "input" + --inputs-dir), params, and bits. Returns an error
/// message, or empty on success.
std::string resolveCase(CliOptions& options)
{
    namespace fs = std::filesystem;

    std::ifstream file(options.casesPath, std::ios::binary);
    if (!file)
        return "cannot open cases file '" + options.casesPath + "'";
    std::ostringstream contents;
    contents << file.rdbuf();

    const json::ParseResult parsed = json::parse(contents.str());
    if (!parsed.ok())
        return "cases file '" + options.casesPath + "': " + parsed.error;

    // Accept the documented {"cases": [...]} envelope or a bare top-level
    // array (tolerant-reader convenience for ad-hoc agent case files).
    const json::Value* cases = parsed.value.find("cases");
    if (cases == nullptr && parsed.value.isArray())
        cases = &parsed.value;
    if (cases == nullptr || !cases->isArray())
        return "cases file '" + options.casesPath
               + "': expected {\"cases\": [...]} (or a top-level array)";

    const json::Value* found = nullptr;
    for (const json::Value& entry : cases->array)
    {
        const json::Value* id = entry.find("id");
        if (id != nullptr && id->isString() && id->string == options.caseId)
        {
            found = &entry;
            break;
        }
    }
    if (found == nullptr)
        return "case '" + options.caseId + "' not found in '"
               + options.casesPath + "'";

    const json::Value* input = found->find("input");
    if (input == nullptr || !input->isString() || input->string.empty())
        return "case '" + options.caseId + "': missing \"input\" (string)";

    const fs::path inputsDir =
        !options.inputsDir.empty()
            ? fs::path(options.inputsDir)
            : fs::path(options.casesPath).parent_path() / "inputs";
    const fs::path inputPath(input->string);
    options.inPath =
        (inputPath.is_absolute() ? inputPath : inputsDir / inputPath).string();

    if (const json::Value* bits = found->find("bits"); bits != nullptr)
    {
        if (!bits->isString())
            return "case '" + options.caseId + "': \"bits\" must be a string";
        const auto depth = parseBits(bits->string);
        if (!depth)
            return "case '" + options.caseId + "': invalid \"bits\" '"
                   + bits->string + "' (expected 16|24|32|float32)";
        options.bits = *depth;
    }

    if (const json::Value* params = found->find("params"); params != nullptr)
    {
        if (!params->isObject())
            return "case '" + options.caseId + "': \"params\" must be an object";
        for (const auto& [key, value] : params->object)
        {
            const std::string text = jsonScalarToString(value);
            if (text.empty() && !value.isString())
                return "case '" + options.caseId + "': param \"" + key
                       + "\" must be a string, number, or boolean";
            if (const std::string err =
                    applyParam(options.params, key, text);
                !err.empty())
                return "case '" + options.caseId + "': " + err;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// render + report
// ---------------------------------------------------------------------------

const char* renderErrorMessage(mws::engine::RenderError error)
{
    using mws::engine::RenderError;
    switch (error)
    {
        case RenderError::NotEnoughMemory:
            // The hardware's own idiom [MAN S3000 p.47]; the predicted output
            // exceeds the 10-minute model-rate cap (architecture.md 5.1).
            return "** NOT ENOUGH MEMORY ** predicted output exceeds the "
                   "10-minute model-rate render cap";
        case RenderError::Aborted:
            return "render aborted";
        case RenderError::UnsupportedModel:
            return "model is a reserved slot at v1 (ADR-004)";
        case RenderError::None:
            break;
    }
    return "unknown render error";
}

int runRender(const CliOptions& options)
{
    const WavIo::ReadResult in = WavIo::read(options.inPath);
    if (!in.ok())
    {
        std::fprintf(stderr, "mwstime-render: cannot read '%s': %s\n",
                     options.inPath.c_str(), in.error.c_str());
        return 1;
    }

    const mws::engine::OfflineRenderer renderer;
    const mws::engine::RenderResult result =
        renderer.render(in.buffer, options.params);
    if (!result.ok())
    {
        std::fprintf(stderr, "mwstime-render: %s\n",
                     renderErrorMessage(result.error));
        return 1;
    }

    const WavIo::WriteResult written =
        WavIo::write(options.outPath, result.out, options.bits);
    if (!written.ok())
    {
        std::fprintf(stderr, "mwstime-render: cannot write '%s': %s\n",
                     options.outPath.c_str(), written.error.c_str());
        return 1;
    }

    // RenderInfo report (task 025: achieved length/ratio, monoSummed,
    // engine version). Stable key: value lines — agent/script parseable.
    std::printf("rendered: %s\n", options.outPath.c_str());
    std::printf("model: %.*s\n",
                static_cast<int>(toString(options.params.model).size()),
                toString(options.params.model).data());
    std::printf("outputFrames: %lld\n",
                static_cast<long long>(result.info.outputFrames));
    std::printf("outputSampleRate: %.6g\n", result.info.outputSampleRate);
    std::printf("achievedTimeFactorPct: %.2f\n",
                result.info.achievedTimeFactorPct);
    std::printf("monoSummed: %s\n", result.info.monoSummed ? "yes" : "no");
    std::printf("engineVersionHash: %016llx\n",
                static_cast<unsigned long long>(result.info.engineVersionHash));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    CliOptions options;
    if (const std::string err = parseArgs(argc, argv, options); !err.empty())
    {
        std::fprintf(stderr, "mwstime-render: %s\n", err.c_str());
        return 2;
    }
    if (options.help)
    {
        printHelp();
        return 0;
    }

    if (!options.caseId.empty())
    {
        if (const std::string err = resolveCase(options); !err.empty())
        {
            std::fprintf(stderr, "mwstime-render: %s\n", err.c_str());
            return 2;
        }
    }

    return runRender(options);
}
