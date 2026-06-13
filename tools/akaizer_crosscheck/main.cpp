// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// akaizer-crosscheck — the LOCAL-ONLY Akaizer secondary cross-check driver
// (task plan/backlog/026c-akaizer-crosscheck.md; docs/qa/akaizer-crosscheck.md;
// docs/design/testing-strategy.md §7 Wave 2, §8).
//
//   *** THIS TOOL IS NEVER RUN IN CI AND IS NOT A GOLDEN/REGRESSION GATE. ***
//
// Akaizer is closed payware (akaizer-analysis.md §1): its renders live only in a
// QA agent's scratch dir (research-cache/, gitignored, never committed) and the
// binary cannot be redistributed or fetched. Its "near-exact" fidelity claim was
// refuted 0-3 (deep-research-report.md Finding 6), so this is a *secondary*
// corroboration cross-check, NOT a calibration oracle — hardware captures are
// the oracle (task 026b). Output is a per-case DEVIATION REPORT, not a pass/fail.
//
// What it does, per case in cases.json:
//   1. Restrict to the certified Akaizer window T 120-2000% (akaizer-analysis.md
//      §2.2); cases outside it are SKIPPED with an explicit "no Akaizer oracle"
//      note (compression 25-119%, S950 D-TIME — no verified Akaizer behavior).
//   2. Derive the CLASSIC grain schedule analytically (dsp-engine.md §3.4):
//      hop_out = C·(1−F), hop_in = round(hop_out/T), grains, schedule length.
//      Predict the splice-comb / flutter frequency = modelRate / hop_out.
//   3. If BOTH a --mws-dir (mwstime renders) and an --akaizer-dir (locally
//      produced Akaizer CLASSIC renders) are given: read <id>.wav from each,
//      PEAK-NORMALIZE BOTH SIDES (dsp-engine.md §7.3 — Akaizer normalizes since
//      v1.3), and measure the dominant flutter frequency + output length of each.
//
// Modes:
//   --cases <cases.json> --inputs-dir <dir>          analytic report only
//   ... --mws-dir <dir> --akaizer-dir <dir>          measured cross-check
//   [--out <report.md>]                              defaults to stdout
//   [--overlap-f <F>]                                SpliceCal F (default 0.20)
//
// Links mwstime_render_lib (the cases.json loader + ModelSpec) and the
// header-only CrossCheck/Report math. JUCE-free.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mws/core/WavIo.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

#include "Cases.h" // mwstime_render_lib: loadCase
#include "CrossCheck.h"
#include "Report.h"

namespace {

struct Options {
    std::string casesPath = "tests/golden/cases.json";
    std::string inputsDir;
    std::string mwsDir;       ///< optional: directory of mwstime <id>.wav renders
    std::string akaizerDir;   ///< optional: directory of Akaizer <id>.wav renders
    std::string outPath;      ///< optional: report file (default stdout)
    double overlapF = mws::akz::kDefaultOverlapF;
    bool ok = true;
    std::string error;
};

void printUsage()
{
    std::cerr <<
"usage: akaizer-crosscheck --cases <cases.json> --inputs-dir <dir>\n"
"                          [--mws-dir <dir>] [--akaizer-dir <dir>]\n"
"                          [--out <report.md>] [--overlap-f <F>]\n"
"\n"
"  SECONDARY, LOCAL-ONLY cross-check (NEVER CI). Akaizer renders are produced\n"
"  by a QA agent with a locally licensed copy into a scratch dir (research-cache/,\n"
"  gitignored). Both sides are peak-normalized; only T 120-2000% cases have an\n"
"  Akaizer oracle. Output is a deviation report, not a pass/fail gate.\n"
"\n"
"  --cases      golden case matrix (default tests/golden/cases.json)\n"
"  --inputs-dir directory of the golden input WAVs (for input lengths)\n"
"  --mws-dir    directory of mwstime <id>.wav renders (optional)\n"
"  --akaizer-dir directory of Akaizer <id>.wav renders (optional)\n"
"  --out        write the Markdown report here (default stdout)\n"
"  --overlap-f  CyclicEngine SpliceCal overlap fraction (default 0.20)\n";
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
        if (flag == "--cases")            o.casesPath = value("--cases");
        else if (flag == "--inputs-dir") o.inputsDir = value("--inputs-dir");
        else if (flag == "--mws-dir")    o.mwsDir = value("--mws-dir");
        else if (flag == "--akaizer-dir") o.akaizerDir = value("--akaizer-dir");
        else if (flag == "--out")        o.outPath = value("--out");
        else if (flag == "--overlap-f")
        {
            const std::string v = value("--overlap-f");
            try { o.overlapF = std::stod(v); }
            catch (...) { o.ok = false; o.error = "invalid --overlap-f: '" + v + "'"; }
        }
        else if (flag == "--help" || flag == "-h") { o.ok = false; o.error = "help"; }
        else { o.ok = false; o.error = "unknown flag: '" + flag + "'"; }
        if (!o.ok)
            return o;
    }
    return o;
}

std::string readTextFile(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open file: " + path;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string joinPath(const std::string& dir, const std::string& file)
{
    if (dir.empty())
        return file;
    if (dir.back() == '/' || dir.back() == '\\')
        return dir + file;
    return dir + "/" + file;
}

/// Reads a WAV and peak-normalizes it in place (both sides MUST be normalized
/// before any comparison — dsp-engine.md §7.3). Returns false (with the buffer
/// untouched) when the file is absent/unreadable: a missing Akaizer render is
/// normal (the QA agent may render only a subset), not a hard error.
bool readNormalized(const std::string& path, mws::core::AudioBuffer& out)
{
    const mws::core::WavIo::ReadResult r = mws::core::WavIo::read(path);
    if (!r.ok())
        return false;
    out = r.buffer;
    mws::akz::peakNormalize(out);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);
    if (!opt.ok)
    {
        if (opt.error != "help")
            std::cerr << "akaizer-crosscheck: " << opt.error << "\n";
        printUsage();
        return opt.error == "help" ? 0 : 2;
    }

    std::string fileError;
    const std::string casesText = readTextFile(opt.casesPath, fileError);
    if (!fileError.empty())
    {
        std::cerr << "akaizer-crosscheck: " << fileError << "\n";
        return 2;
    }

    // Enumerate case ids from the JSON (top-level "cases" array). We reuse the
    // mwstime_render_lib loader per id to get the resolved ParamSnapshot, so we
    // do not re-implement the schema here.
    std::vector<std::string> caseIds;
    {
        // Minimal id scan: find every "id" string under "cases". The loader
        // validates the schema; we only need the id list to drive it.
        std::size_t pos = 0;
        const std::string key = "\"id\"";
        while ((pos = casesText.find(key, pos)) != std::string::npos)
        {
            const std::size_t colon = casesText.find(':', pos + key.size());
            if (colon == std::string::npos) break;
            const std::size_t q1 = casesText.find('"', colon + 1);
            if (q1 == std::string::npos) break;
            const std::size_t q2 = casesText.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            caseIds.push_back(casesText.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
    }
    if (caseIds.empty())
    {
        std::cerr << "akaizer-crosscheck: no cases found in " << opt.casesPath << "\n";
        return 2;
    }

    const bool measure = !opt.mwsDir.empty() && !opt.akaizerDir.empty();

    std::vector<mws::akz::CaseReport> reports;
    reports.reserve(caseIds.size());

    for (const std::string& id : caseIds)
    {
        const mwsrender::CaseLoadResult loaded = mwsrender::loadCase(casesText, id);
        if (!loaded.ok())
        {
            std::cerr << "akaizer-crosscheck: skipping '" << id << "': "
                      << loaded.error << "\n";
            continue;
        }
        const mws::engine::ParamSnapshot raw = loaded.def.params;
        const mws::model::ModelSpec& spec = mws::model::ModelSpec::get(raw.model);
        const mws::engine::ParamSnapshot clamped = spec.clamp(raw);

        // Input length at the model rate drives the schedule. We need the input
        // frame count; read just the header-bearing file (cheap whole-file read).
        long inputFrames = 0;
        if (!opt.inputsDir.empty())
        {
            const mws::core::WavIo::ReadResult in =
                mws::core::WavIo::read(joinPath(opt.inputsDir, loaded.def.inputFile));
            if (in.ok())
                inputFrames = static_cast<long>(in.buffer.numFrames());
        }

        const double modelRate = spec.modelRateHz(clamped);
        // Use the CLAMPED time factor: an S950 case authored at 2000% is really
        // 999% at the engine, and that is what an Akaizer comparison must use.
        mws::akz::CaseReport r = mws::akz::analyzeCase(
            id, clamped.timeFactor, clamped.cycleLen, inputFrames, modelRate,
            opt.overlapF);

        if (!r.skipped && measure)
        {
            mws::core::AudioBuffer mws, akz;
            const bool haveMws = readNormalized(joinPath(opt.mwsDir, id + ".wav"), mws);
            const bool haveAkz =
                readNormalized(joinPath(opt.akaizerDir, id + ".wav"), akz);
            if (haveMws && haveAkz)
            {
                r.haveMeasurements = true;
                r.mwsFlutterHz = mws::akz::measureDominantHz(mws);
                r.akaizerFlutterHz = mws::akz::measureDominantHz(akz);
                r.mwsOutputLen = static_cast<long>(mws.numFrames());
                r.akaizerOutputLen = static_cast<long>(akz.numFrames());
            }
        }
        reports.push_back(std::move(r));
    }

    std::string corner = "cases=" + opt.casesPath;
    if (measure)
        corner += ", mws-dir=" + opt.mwsDir + ", akaizer-dir=" + opt.akaizerDir
               + " (peak-normalized both sides)";
    else
        corner += " (analytic only — supply --mws-dir and --akaizer-dir to measure)";

    if (opt.outPath.empty())
    {
        mws::akz::writeMarkdownReport(std::cout, reports, corner);
    }
    else
    {
        std::ofstream out(opt.outPath, std::ios::binary);
        if (!out)
        {
            std::cerr << "akaizer-crosscheck: cannot write report '" << opt.outPath
                      << "'\n";
            return 2;
        }
        mws::akz::writeMarkdownReport(out, reports, corner);
        std::cout << "akaizer-crosscheck: wrote " << reports.size()
                  << " case rows to " << opt.outPath << "\n";
    }
    return 0;
}
