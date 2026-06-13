// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CLI argument parsing (Args.h). Flags mirror the docs/design/dsp-engine.md §2
// parameter table; defaults are the §2 table defaults (which are also the
// ParamSnapshot field defaults, so an unset flag leaves the field untouched).

#include "Args.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace mwsrender {

namespace {

using mws::engine::HopMode;
using mws::engine::Material;
using mws::engine::SampleRateSel;
using mws::engine::StretchMode;
using mws::model::ModelId;

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Parses a finite double; on failure returns false and leaves `out` untouched.
bool parseDouble(const std::string& s, double& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size())
        return false;
    out = value;
    return true;
}

bool parseInt(const std::string& s, int& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    const long value = std::strtol(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size())
        return false;
    out = static_cast<int>(value);
    return true;
}

} // namespace

std::optional<ModelId> parseModel(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "s900")  return ModelId::S900;
    if (v == "s950")  return ModelId::S950;
    if (v == "s1000") return ModelId::S1000;
    if (v == "s1100") return ModelId::S1100;
    if (v == "s3000") return ModelId::S3000; // reserved; renderer refuses it
    return std::nullopt;
}

std::optional<HopMode> parseHopMode(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "classic") return HopMode::Classic;
    if (v == "revised") return HopMode::Revised;
    return std::nullopt;
}

std::optional<StretchMode> parseStretchMode(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "cyclic") return StretchMode::Cyclic;
    if (v == "intell") return StretchMode::Intell;
    return std::nullopt;
}

std::optional<Material> parseMaterial(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "mon1") return Material::Mon1;
    if (v == "pol2") return Material::Pol2;
    return std::nullopt;
}

std::optional<SampleRateSel> parseFs(const std::string& s)
{
    if (s == "44.1" || s == "44100" || s == "44.10") return SampleRateSel::Fs44100;
    if (s == "22.05" || s == "22050") return SampleRateSel::Fs22050;
    return std::nullopt;
}

std::optional<bool> parseOnOff(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "on" || v == "true" || v == "1")  return true;
    if (v == "off" || v == "false" || v == "0") return false;
    return std::nullopt;
}

std::string helpText()
{
    return
R"(mwstime-render — Akai S-series timestretch renderer (wav in -> wav out).

JUCE-free CLI over OfflineRenderer + WavIo; the golden-test driver and the
agent DSP-debugging tool (DSP work never requires a host).

USAGE
  mwstime-render --in <in.wav> --out <out.wav> [render flags]   (direct mode)
  mwstime-render --case <id> --cases <cases.json> --inputs-dir <dir>
                 --out <out.wav>                                 (case mode)
  mwstime-render --engine-version                                (prints
                 "<engineVersion> 0x<hash>" and exits — the bless target reads
                 it to stamp tests/golden/blessed/MANIFEST.json)
  mwstime-render --help

DIRECT-MODE RENDER FLAGS (defaults per dsp-engine.md §2; ranges clamped at the
engine by the active model, so a flag value outside a model's range is coerced
to that model's hardware-displayed value):
  --model       S900|S950|S1000|S1100   model (default S1000)
  --time-factor <pct>                    TIME FACTOR, % of length, 25..2000
                                         (default 100; S950 clamps to 999)
  --cycle-len   <samples>                CYCLE LENGTH / S950 D-TIME, samples at
                                         model rate, 20..2000 (default 1000)
  --stretch-mode CYCLIC|INTELL           S1000/S1100 stretch mode (default
                                         CYCLIC; INTELL is greyed until v1.1)
  --hop-mode    CLASSIC|REVISED          TIMING (default CLASSIC = hw-faithful)
  --transpose   <semitones>              TRANSPOSE, st, -24..+24 (default 0)
  --qual        <1..99>                  QUAL (INTELL only; inert at v1)
  --width       <1..99>                  WIDTH (INTELL only; inert at v1)
  --material    MON1|POL2                S950 material switch (default POL2)
  --bandwidth   <kHz>                    BANDWIDTH, S900/S950, 3.0..19.2 kHz
                                         (default 19.2; model rate = 2.5 x BW)
  --fs          44.1|22.05               FS, S1000/S1100 model rate, kHz
                                         (default 44.1)
  --character   on|off                   CHARACTER chain (default on)
  --norm        on|off                   NORM peak-normalize to source peak
                                         (default off = authentic)
  --out-trim    <dB>                     OUTPUT trim, dB, -24..+12 (default 0)
  --bit-depth   16|24|32|float           output WAV depth (default 16)

CASE MODE (schema in this tool; corpus populated by task 026):
  --case        <id>     case id to look up in the cases file
  --cases       <path>   cases.json path (default tests/golden/cases.json)
  --inputs-dir  <dir>    directory holding the case's input WAV
  --out         <path>   output WAV path
  --bit-depth   ...      overrides the case's depth if given

OUTPUT
  On success: prints a RenderInfo block to stdout (achieved output length and
  ratio, monoSummed, engine version) and exits 0. On a refused render
  (NotEnoughMemory) or a bad argument it prints a message to stderr and exits
  nonzero. Identical invocations produce bit-identical output files.
)";
}

ArgsParseResult parseArgs(int argc, const char* const* argv)
{
    ArgsParseResult result;
    CliArgs& a = result.args;
    a.casesPath = "tests/golden/cases.json";

    auto failArg = [&](const std::string& message) {
        result.error = message;
        return result;
    };

    // First pass: scan for help / version, then walk flag/value pairs.
    for (int i = 1; i < argc; ++i)
    {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h")
        {
            a.mode = Mode::Help;
            return result; // help wins regardless of other flags
        }
        if (flag == "--engine-version" || flag == "--version")
        {
            a.mode = Mode::Version;
            return result; // version query short-circuits like help
        }
    }

    bool sawCase = false;
    bool sawIo = false;

    auto needValue = [&](int& i, const std::string& flag, std::string& out) -> bool {
        if (i + 1 >= argc)
        {
            result.error = "missing value for " + flag;
            return false;
        }
        out = argv[++i];
        return true;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string flag = argv[i];
        std::string value;

        if (flag == "--in")
        {
            if (!needValue(i, flag, a.inPath)) return result;
            sawIo = true;
        }
        else if (flag == "--out")
        {
            if (!needValue(i, flag, a.outPath)) return result;
        }
        else if (flag == "--case")
        {
            if (!needValue(i, flag, a.caseId)) return result;
            sawCase = true;
        }
        else if (flag == "--cases")
        {
            if (!needValue(i, flag, a.casesPath)) return result;
        }
        else if (flag == "--inputs-dir")
        {
            if (!needValue(i, flag, a.inputsDir)) return result;
        }
        else if (flag == "--model")
        {
            if (!needValue(i, flag, value)) return result;
            const auto m = parseModel(value);
            if (!m) return failArg("invalid --model: '" + value + "' (S900|S950|S1000|S1100)");
            a.params.model = *m;
            sawIo = true;
        }
        else if (flag == "--time-factor")
        {
            if (!needValue(i, flag, value)) return result;
            double d{};
            if (!parseDouble(value, d)) return failArg("invalid --time-factor: '" + value + "'");
            a.params.timeFactor = d;
            sawIo = true;
        }
        else if (flag == "--cycle-len")
        {
            if (!needValue(i, flag, value)) return result;
            int n{};
            if (!parseInt(value, n)) return failArg("invalid --cycle-len: '" + value + "'");
            a.params.cycleLen = n;
            sawIo = true;
        }
        else if (flag == "--stretch-mode")
        {
            if (!needValue(i, flag, value)) return result;
            const auto sm = parseStretchMode(value);
            if (!sm) return failArg("invalid --stretch-mode: '" + value + "' (CYCLIC|INTELL)");
            a.params.stretchMode = *sm;
            sawIo = true;
        }
        else if (flag == "--hop-mode")
        {
            if (!needValue(i, flag, value)) return result;
            const auto hm = parseHopMode(value);
            if (!hm) return failArg("invalid --hop-mode: '" + value + "' (CLASSIC|REVISED)");
            a.params.hopMode = *hm;
            sawIo = true;
        }
        else if (flag == "--transpose")
        {
            if (!needValue(i, flag, value)) return result;
            double d{};
            if (!parseDouble(value, d)) return failArg("invalid --transpose: '" + value + "'");
            a.params.transpose = d;
            sawIo = true;
        }
        else if (flag == "--qual")
        {
            if (!needValue(i, flag, value)) return result;
            int n{};
            if (!parseInt(value, n)) return failArg("invalid --qual: '" + value + "'");
            a.params.qual = n;
            sawIo = true;
        }
        else if (flag == "--width")
        {
            if (!needValue(i, flag, value)) return result;
            int n{};
            if (!parseInt(value, n)) return failArg("invalid --width: '" + value + "'");
            a.params.width = n;
            sawIo = true;
        }
        else if (flag == "--material")
        {
            if (!needValue(i, flag, value)) return result;
            const auto mat = parseMaterial(value);
            if (!mat) return failArg("invalid --material: '" + value + "' (MON1|POL2)");
            a.params.material = *mat;
            sawIo = true;
        }
        else if (flag == "--bandwidth")
        {
            if (!needValue(i, flag, value)) return result;
            double d{};
            if (!parseDouble(value, d)) return failArg("invalid --bandwidth: '" + value + "'");
            a.params.bandwidth = d;
            sawIo = true;
        }
        else if (flag == "--fs")
        {
            if (!needValue(i, flag, value)) return result;
            const auto fs = parseFs(value);
            if (!fs) return failArg("invalid --fs: '" + value + "' (44.1|22.05)");
            a.params.sampleRateSel = *fs;
            sawIo = true;
        }
        else if (flag == "--character")
        {
            if (!needValue(i, flag, value)) return result;
            const auto b = parseOnOff(value);
            if (!b) return failArg("invalid --character: '" + value + "' (on|off)");
            a.params.character = *b;
            sawIo = true;
        }
        else if (flag == "--norm")
        {
            if (!needValue(i, flag, value)) return result;
            const auto b = parseOnOff(value);
            if (!b) return failArg("invalid --norm: '" + value + "' (on|off)");
            a.params.norm = *b;
            sawIo = true;
        }
        else if (flag == "--out-trim")
        {
            if (!needValue(i, flag, value)) return result;
            double d{};
            if (!parseDouble(value, d)) return failArg("invalid --out-trim: '" + value + "'");
            a.params.outTrim = d;
            sawIo = true;
        }
        else if (flag == "--bit-depth")
        {
            if (!needValue(i, flag, value)) return result;
            const std::string v = toLower(value);
            if (v == "16")      a.bitDepth = CliArgs::BitDepth::Int16;
            else if (v == "24") a.bitDepth = CliArgs::BitDepth::Int24;
            else if (v == "32") a.bitDepth = CliArgs::BitDepth::Int32;
            else if (v == "float" || v == "f32" || v == "float32")
                a.bitDepth = CliArgs::BitDepth::Float32;
            else return failArg("invalid --bit-depth: '" + value + "' (16|24|32|float)");
        }
        else
        {
            return failArg("unknown flag: '" + flag + "' (see --help)");
        }
    }

    // Mode resolution and required-flag checks.
    if (sawCase)
    {
        a.mode = Mode::Case;
        if (sawIo)
            return failArg("--case cannot be combined with direct-mode render flags");
        if (a.caseId.empty())
            return failArg("--case requires a case id");
        if (a.inputsDir.empty())
            return failArg("case mode requires --inputs-dir");
        if (a.outPath.empty())
            return failArg("case mode requires --out");
    }
    else
    {
        a.mode = Mode::Direct;
        if (a.inPath.empty())
            return failArg("direct mode requires --in (or use --case / --help)");
        if (a.outPath.empty())
            return failArg("direct mode requires --out");
    }

    return result;
}

} // namespace mwsrender
