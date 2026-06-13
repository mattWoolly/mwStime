// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// calibrate — the SpliceCal / D-TIME hardware-capture calibration tool
// (task plan/backlog/026b; docs/qa/hardware-capture-plan.md). JUCE-free; links
// only mwstime_core + the shared mwstime_render_lib case schema.
//
// Reads a calibration MANIFEST (the tests/golden/cases.json schema + a per-case
// `set` tag), the committed original synthetic INPUTS, and an arbitrary LOCAL
// CAPTURES directory (real S950/S1100 renders — NEVER committed; no third-party
// audio in the repo). Per case it measures the splice observables (flutter rate
// / stutter schedule / schedule-derived output length), FITS overlapF by
// re-rendering the engine across a grid and minimizing the distance to the
// capture (Calibration cases) or PREDICTS at a frozen overlapF (Validation
// cases — the no-overfitting rule), checks the S950 D-TIME mapping, and prints a
// proposed SpliceCal plus a report.
//
// Usage:
//   calibrate --manifest <m.json> --inputs-dir <dir> --captures <dir>
//             [--set calibration|validation|all] [--overlap-f <F>]
//             [--grid-lo <F> --grid-hi <F> --grid-step <F>]
//
// Exit 0 = all selected cases fit/predicted within tolerance (and every checked
//          S950 D-TIME mapping is consistent);
// Exit 1 = a selected case missed tolerance or a D-TIME check failed (the gate
//          FAILed — see docs/qa/hardware-capture-plan.md §5/§6);
// Exit 2 = usage / IO / manifest error.

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Calibrate.h"
#include "Manifest.h"

namespace {

using mwscal::CalCase;
using mwscal::CaseFit;
using mwscal::CaseSet;
using mwscal::FitConfig;

enum class SetFilter { Calibration, Validation, All };

struct Options {
    std::string manifest;
    std::string inputsDir;
    std::string capturesDir;
    SetFilter setFilter = SetFilter::All;
    FitConfig cfg{};
    bool ok = true;
    bool help = false;
    std::string error;
};

void printUsage(std::ostream& os)
{
    os <<
"usage: calibrate --manifest <m.json> --inputs-dir <dir> --captures <dir>\n"
"                 [--set calibration|validation|all] [--overlap-f <F>]\n"
"                 [--grid-lo <F>] [--grid-hi <F>] [--grid-step <F>]\n"
"                 [--consistency-tol <x>]\n"
"\n"
"  --manifest        calibration manifest (cases.json schema + per-case `set`)\n"
"  --inputs-dir      committed original synthetic corpus (tests/golden/inputs)\n"
"  --captures        LOCAL capture dir (real renders; NOT committed) of <id>.wav\n"
"  --set             which cases to run (default: all)\n"
"  --overlap-f       frozen overlapF for Validation cases (the fitted value);\n"
"                    PREDICT-only, no search (the no-overfitting rule)\n"
"  --grid-lo/-hi/-step  the overlapF search grid (default 0.05..0.60 step 0.01)\n"
"  --consistency-tol RMS residual tolerance for D-TIME / validation consistency\n"
"\n"
"Exit 0 = all selected cases within tolerance (and D-TIME mappings consistent);\n"
"1 = a case missed tolerance / a D-TIME check failed; 2 = usage / IO error.\n";
}

double parseDouble(const std::string& s, bool& ok)
{
    try { std::size_t pos = 0; const double v = std::stod(s, &pos);
          ok = (pos == s.size()); return v; }
    catch (...) { ok = false; return 0.0; }
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
        auto num = [&](const char* name, float& dst) {
            const std::string v = value(name);
            if (!o.ok) return;
            bool good = true;
            const double parsed = parseDouble(v, good);
            if (!good) { o.ok = false; o.error = std::string("invalid ") + name + ": '" + v + "'"; }
            else dst = static_cast<float>(parsed);
        };

        if (flag == "--manifest")          o.manifest = value("--manifest");
        else if (flag == "--inputs-dir")   o.inputsDir = value("--inputs-dir");
        else if (flag == "--captures")     o.capturesDir = value("--captures");
        else if (flag == "--set")
        {
            const std::string s = value("--set");
            if (!o.ok) {}
            else if (s == "calibration") o.setFilter = SetFilter::Calibration;
            else if (s == "validation")  o.setFilter = SetFilter::Validation;
            else if (s == "all")         o.setFilter = SetFilter::All;
            else { o.ok = false; o.error = "invalid --set: '" + s + "'"; }
        }
        else if (flag == "--overlap-f")    num("--overlap-f", o.cfg.frozenOverlapF);
        else if (flag == "--grid-lo")      num("--grid-lo", o.cfg.gridLo);
        else if (flag == "--grid-hi")      num("--grid-hi", o.cfg.gridHi);
        else if (flag == "--grid-step")    num("--grid-step", o.cfg.gridStep);
        else if (flag == "--consistency-tol")
        {
            const std::string v = value("--consistency-tol");
            if (o.ok) { bool good = true; const double x = parseDouble(v, good);
                        if (!good) { o.ok = false; o.error = "invalid --consistency-tol: '" + v + "'"; }
                        else o.cfg.consistencyTol = x; }
        }
        else if (flag == "--help" || flag == "-h") { o.help = true; }
        else { o.ok = false; o.error = "unknown flag: '" + flag + "'"; }
        if (!o.ok) return o;
    }
    if (!o.help && (o.manifest.empty() || o.inputsDir.empty() || o.capturesDir.empty()))
    {
        o.ok = false;
        o.error = "--manifest, --inputs-dir and --captures are all required";
    }
    o.cfg.inputsDir = o.inputsDir;
    o.cfg.capturesDir = o.capturesDir;
    return o;
}

bool selected(CaseSet set, SetFilter filter)
{
    switch (filter)
    {
        case SetFilter::All:          return true;
        case SetFilter::Calibration:  return set == CaseSet::Calibration;
        case SetFilter::Validation:   return set == CaseSet::Validation;
    }
    return true;
}

const char* setName(CaseSet s)
{
    return s == CaseSet::Calibration ? "calibration" : "validation";
}

std::string readTextFile(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open manifest: " + path; return {}; }
    std::ostringstream buf; buf << in.rdbuf();
    return buf.str();
}

void printFit(const CaseFit& f)
{
    std::cout << "case " << f.id << " [" << setName(f.set) << "]\n";
    if (!f.ok)
    {
        std::cout << "  ERROR: " << f.error << "\n";
        return;
    }
    std::cout << std::fixed;
    std::cout << "  proposed SpliceCal : overlapF " << std::setprecision(3) << f.overlapF
              << ", shape Linear, rounding RoundNearest\n";
    std::cout << "  flutter rate       : " << std::setprecision(2) << f.obs.flutterRateHz
              << " Hz (splice-comb spacing = modelRate/hop_out)\n";
    std::cout << "  stutter schedule   : hop_out " << f.obs.hopOut
              << ", hop_in " << f.obs.hopIn
              << " (C " << f.obs.cycleLen << " @ " << std::setprecision(1)
              << f.obs.modelRateHz << " Hz)\n";
    std::cout << "  schedule length    : " << f.obs.scheduleLen
              << " (capture " << f.obs.captureLen << ")\n";
    std::cout << "  residual           : " << std::scientific << std::setprecision(3)
              << f.residual << std::fixed << "\n";
    if (f.dTimeChecked)
        std::cout << "  D-TIME mapping     : "
                  << (f.dTimeConsistent ? "CONSISTENT" : "INCONSISTENT")
                  << " (D-TIME ≡ cycle length in samples — dsp-engine §5 gate)\n";
}

} // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);
    if (opt.help)
    {
        printUsage(std::cout);
        return 0;
    }
    if (!opt.ok)
    {
        std::cerr << "calibrate: " << opt.error << "\n";
        printUsage(std::cerr);
        return 2;
    }

    std::string fileErr;
    const std::string text = readTextFile(opt.manifest, fileErr);
    if (!fileErr.empty())
    {
        std::cerr << "calibrate: " << fileErr << "\n";
        return 2;
    }

    const mwscal::ManifestLoadResult manifest = mwscal::loadManifest(text);
    if (!manifest.ok())
    {
        std::cerr << "calibrate: " << manifest.error << "\n";
        return 2;
    }

    std::cout << "calibrate: SpliceCal / D-TIME hardware-capture fit\n";
    std::cout << "  manifest : " << opt.manifest << "\n";
    std::cout << "  inputs   : " << opt.inputsDir << "\n";
    std::cout << "  captures : " << opt.capturesDir << " (local; not committed)\n\n";

    int selectedCount = 0;
    int failures = 0;     // tolerance / D-TIME failures (gate FAIL)
    int ioErrors = 0;     // missing captures / inputs (IO error)

    for (const CalCase& c : manifest.cases)
    {
        if (!selected(c.set, opt.setFilter))
            continue;
        ++selectedCount;

        const CaseFit fit = mwscal::fitCase(c, opt.cfg);
        printFit(fit);

        if (!fit.ok)
        {
            ++ioErrors;
        }
        else
        {
            // A fit/prediction misses tolerance when its residual is above the
            // consistency tolerance (an un-modelled splice), or a checked S950
            // D-TIME mapping is inconsistent (the v1-freeze gate FAILs).
            if (fit.residual > opt.cfg.consistencyTol)
                ++failures;
            if (fit.dTimeChecked && !fit.dTimeConsistent)
                ++failures;
        }
        std::cout << "\n";
    }

    if (selectedCount == 0)
    {
        std::cerr << "calibrate: no cases matched --set (nothing to do)\n";
        return 2;
    }

    std::cout << "calibrate: " << selectedCount << " case(s); " << failures
              << " over tolerance / inconsistent; " << ioErrors << " IO error(s)\n";

    if (ioErrors > 0)
        return 2;
    return failures > 0 ? 1 : 0;
}
