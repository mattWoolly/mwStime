// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ui_screenshot — the headless UI golden-screenshot driver + comparer (task
// 047b; docs/design/ui-design.md §4, docs/design/testing-strategy.md §7 Wave 3,
// ADR-005). Modes:
//
//   --list
//       Print every defined capture state id (one per line). Used by the bless
//       target so the script needs no state table of its own (same pattern as
//       the golden harness reading cases from cases.json — task 026).
//
//   --capture <id> --out <file.png>
//       Render the named state offscreen with the JUCE software renderer and
//       write a PNG. Deterministic (fixed fake LCD data, no animations).
//
//   --compare --candidate <a.png> --blessed <b.png>
//             --policy exact|tolerance [--max-delta N] [--pct-threshold P]
//       Gate a candidate against a blessed PNG. `exact` requires byte-identical
//       decoded pixels (the macOS-arm64 reference-platform gate). `tolerance`
//       allows per-channel |delta| <= max-delta on each pixel and fails only if
//       the fraction of differing pixels exceeds pct-threshold (the off-reference
//       Linux/Windows path — cross-platform AA is not deterministic, ADR-005).
//       On mismatch it prints diagnostics (max channel delta, differing-pixel %,
//       first divergent pixel) so failures are debuggable from CTest logs.
//
//   --is-reference-platform
//       Exit 0 on the reference platform (macOS arm64), 10 otherwise. The
//       per-state runner uses this to pick exact vs tolerance automatically
//       (scope: "on non-reference platforms the exact gate downgrades to the
//       tolerance compare automatically; never silently skips on the reference
//       platform").

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "CaptureStates.h"

namespace {

/// macOS arm64 is the single reference platform that gates bit-exactly
/// (ui-design §4, ADR-005). Detected at compile time from the toolchain macros.
constexpr bool kIsReferencePlatform =
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    true;
#else
    false;
#endif

[[nodiscard]] std::string argValue(int argc, char** argv, const std::string& flag,
                                   const std::string& fallback = {})
{
    for (int i = 1; i + 1 < argc; ++i)
        if (flag == argv[i])
            return argv[i + 1];
    return fallback;
}

[[nodiscard]] bool hasFlag(int argc, char** argv, const std::string& flag)
{
    for (int i = 1; i < argc; ++i)
        if (flag == argv[i])
            return true;
    return false;
}

int doList()
{
    for (const auto& s : mws::uiscreenshot::captureStates())
        std::cout << s.id << '\n';
    return 0;
}

int doCapture(int argc, char** argv)
{
    const std::string id = argValue(argc, argv, "--capture");
    const std::string out = argValue(argc, argv, "--out");
    if (id.empty() || out.empty())
    {
        std::cerr << "ui_screenshot: --capture <id> --out <file.png> required\n";
        return 2;
    }
    if (! mws::uiscreenshot::isKnownState(id))
    {
        std::cerr << "ui_screenshot: unknown state '" << id << "'\n";
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::Image image = mws::uiscreenshot::renderState(id);

    juce::File outFile(juce::File::getCurrentWorkingDirectory().getChildFile(out));
    if (juce::File::isAbsolutePath(out))
        outFile = juce::File(out);
    outFile.deleteFile();
    if (auto stream = outFile.createOutputStream())
    {
        juce::PNGImageFormat png;
        if (! png.writeImageToStream(image, *stream))
        {
            std::cerr << "ui_screenshot: PNG encode failed for " << out << "\n";
            return 1;
        }
    }
    else
    {
        std::cerr << "ui_screenshot: cannot open output " << out << "\n";
        return 1;
    }
    std::cout << "ui_screenshot: wrote " << outFile.getFullPathName() << " ("
              << image.getWidth() << "x" << image.getHeight() << ")\n";
    return 0;
}

/// Load a PNG into an ARGB software image (so the pixel layout is identical for
/// both sides of the compare regardless of how the file was encoded).
juce::Image loadPng(const juce::String& path)
{
    juce::File f(path);
    juce::Image raw = juce::ImageFileFormat::loadFrom(f);
    if (! raw.isValid())
        return raw;
    juce::Image img(juce::Image::ARGB, raw.getWidth(), raw.getHeight(), true,
                    juce::SoftwareImageType());
    juce::Graphics g(img);
    g.drawImageAt(raw, 0, 0);
    return img;
}

int doCompare(int argc, char** argv)
{
    const juce::String candPath = argValue(argc, argv, "--candidate");
    const juce::String blessedPath = argValue(argc, argv, "--blessed");
    const std::string policy = argValue(argc, argv, "--policy", "exact");
    const int maxDelta = std::atoi(argValue(argc, argv, "--max-delta", "2").c_str());
    const double pctThreshold =
        std::atof(argValue(argc, argv, "--pct-threshold", "0.5").c_str());

    if (candPath.isEmpty() || blessedPath.isEmpty())
    {
        std::cerr << "ui_screenshot: --compare needs --candidate and --blessed\n";
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::Image cand = loadPng(candPath);
    const juce::Image blessed = loadPng(blessedPath);

    if (! cand.isValid())
    {
        std::cerr << "ui_screenshot: cannot load candidate PNG: " << candPath << "\n";
        return 1;
    }
    if (! blessed.isValid())
    {
        std::cerr << "ui_screenshot: cannot load blessed PNG: " << blessedPath << "\n";
        return 1;
    }
    if (cand.getWidth() != blessed.getWidth() || cand.getHeight() != blessed.getHeight())
    {
        std::cerr << "ui_screenshot: DIMENSION MISMATCH candidate "
                  << cand.getWidth() << "x" << cand.getHeight() << " vs blessed "
                  << blessed.getWidth() << "x" << blessed.getHeight() << "\n";
        return 1;
    }

    const int w = cand.getWidth();
    const int h = cand.getHeight();
    int maxChannelDelta = 0;
    long long differingPixels = 0;
    int firstX = -1, firstY = -1;

    juce::Image::BitmapData ca(cand, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData ba(blessed, juce::Image::BitmapData::readOnly);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const juce::Colour pc = ca.getPixelColour(x, y);
            const juce::Colour pb = ba.getPixelColour(x, y);
            const int dr = std::abs((int) pc.getRed() - (int) pb.getRed());
            const int dg = std::abs((int) pc.getGreen() - (int) pb.getGreen());
            const int db = std::abs((int) pc.getBlue() - (int) pb.getBlue());
            const int da = std::abs((int) pc.getAlpha() - (int) pb.getAlpha());
            const int pixelMax = std::max({ dr, dg, db, da });
            if (pixelMax > maxChannelDelta)
                maxChannelDelta = pixelMax;

            const bool differs = (policy == "exact") ? (pixelMax > 0)
                                                      : (pixelMax > maxDelta);
            if (differs)
            {
                ++differingPixels;
                if (firstX < 0)
                {
                    firstX = x;
                    firstY = y;
                }
            }
        }
    }

    const double totalPixels = (double) w * (double) h;
    const double diffPct = 100.0 * (double) differingPixels / totalPixels;

    bool pass = false;
    if (policy == "exact")
        pass = (differingPixels == 0);
    else
        pass = (diffPct <= pctThreshold);

    if (! pass)
    {
        std::cerr << "ui_screenshot: SCREENSHOT MISMATCH (policy=" << policy << ")\n"
                  << "  image size       : " << w << "x" << h << " (" << (long long) totalPixels
                  << " px)\n"
                  << "  max channel delta: " << maxChannelDelta << "\n"
                  << "  differing pixels : " << differingPixels << " (" << diffPct << "%)\n";
        if (policy != "exact")
            std::cerr << "  allowed          : per-pixel |delta| <= " << maxDelta
                      << ", differing <= " << pctThreshold << "%\n";
        if (firstX >= 0)
            std::cerr << "  first divergence : (" << firstX << ", " << firstY << ")\n";
        return 1;
    }

    std::cout << "ui_screenshot: MATCH (policy=" << policy << ", max channel delta "
              << maxChannelDelta << ", differing " << diffPct << "%)\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (hasFlag(argc, argv, "--is-reference-platform"))
        return kIsReferencePlatform ? 0 : 10;
    if (hasFlag(argc, argv, "--list"))
        return doList();
    if (hasFlag(argc, argv, "--compare"))
        return doCompare(argc, argv);
    if (hasFlag(argc, argv, "--capture"))
        return doCapture(argc, argv);

    std::cerr << "usage: ui_screenshot --list\n"
              << "       ui_screenshot --capture <id> --out <file.png>\n"
              << "       ui_screenshot --compare --candidate <a.png> --blessed <b.png>\n"
              << "                     --policy exact|tolerance [--max-delta N] [--pct-threshold P]\n"
              << "       ui_screenshot --is-reference-platform\n";
    return 2;
}
