// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdPageModel (task 041) — page content builders. Page text mirrors the
// manuals' printed screens: the S1000-family multi-line TIME-STRETCH page
// (ui-design §1 mockup, [MAN §3, §5 example screen]), the S950 2-line
// ">14 STRETCH" page-14 layout [MAN §2 p.30], the S900 varispeed page with
// the ADR-003 notice (dsp-engine.md §6), and the FX-mode
// "TIME-STRETCH (REALTIME)" page (ui-design §6.4). NO JUCE includes here
// (headless rule).

#include "LcdPageModel.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string_view>

namespace mws::ui {
namespace {

using engine::HopMode;
using engine::ParamId;
using engine::ParamSnapshot;
using engine::PluginMode;
using engine::TempoSync;

// --- low-level row writing -------------------------------------------------

/// Writes `text` into `row` starting at `col`, clipping at kLcdCols.
void put(LcdRow& row, int col, std::string_view text,
         LcdCellStyle style = LcdCellStyle::Normal)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const int c = col + static_cast<int>(i);
        if (c < 0 || c >= kLcdCols)
            continue;
        row.text[static_cast<std::size_t>(c)] = text[i];
        row.styles[static_cast<std::size_t>(c)] = style;
    }
}

/// Writes `text` right-aligned so its last character lands in column
/// `col + width - 1` (hardware numeric-field look).
void putRight(LcdRow& row, int col, int width, std::string_view text,
              LcdCellStyle style = LcdCellStyle::Normal)
{
    const int len = static_cast<int>(text.size());
    put(row, col + std::max(0, width - len), text, style);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
std::string fmt(const char* format, ...)
{
    char buf[64]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

std::string truncated(std::string s, std::size_t maxLen)
{
    if (s.size() > maxLen)
        s.resize(maxLen);
    return s;
}

/// Akai sample names are 12 characters [MAN §3].
constexpr std::size_t kNameLen = 12;

std::string newSampleName(const LcdSampleInfo& sample)
{
    if (!sample.newName.empty())
        return truncated(sample.newName, kNameLen);
    if (!sample.name.empty())
        return truncated(sample.name + "*ST", kNameLen);
    return "-";
}

// --- enum -> LCD words -----------------------------------------------------

std::string_view toLcd(engine::StretchMode m)
{
    return m == engine::StretchMode::Intell ? "INTELL" : "CYCLIC";
}

std::string_view toLcd(HopMode m)
{
    return m == HopMode::Revised ? "REVISED" : "CLASSIC";
}

std::string_view toLcd(engine::Material m)
{
    return m == engine::Material::Mon1 ? "MON1" : "POL2";
}

std::string_view toLcd(engine::FxWindow w)
{
    switch (w)
    {
        case engine::FxWindow::QuarterBar: return "1/4 BAR";
        case engine::FxWindow::HalfBar:    return "1/2 BAR";
        case engine::FxWindow::OneBar:     return "1 BAR";
        case engine::FxWindow::TwoBars:    return "2 BARS";
        case engine::FxWindow::FourBars:   return "4 BARS";
        case engine::FxWindow::EightBars:  return "8 BARS";
        case engine::FxWindow::Free:       return "FREE";
    }
    return "?";
}

// --- message line (priority order; highest wins) ---------------------------

std::string activeMessage(const LcdRenderInfo& render)
{
    if (render.loadError != LcdLoadError::None)
        return LcdPageModel::messageForLoadError(render.loadError);
    if (render.notEnoughMemory)
        return LcdPageModel::kNotEnoughMemory;
    if (render.progressPercent >= 0)
        return LcdPageModel::formatProgress(render.progressPercent,
                                            render.remainingSeconds);
    if (render.pathOnlyPersistence)
        return LcdPageModel::kPathOnlyNotice;
    return {};
}

/// True when the FX FREE causality clamp is in force (ADR-006): either the
/// engine reported it via the FIFO, or the clamped snapshot raised the raw
/// timeFactor (ModelSpec::clamp is the single clamping authority).
bool fxClampShown(const ParamSnapshot& raw, const ParamSnapshot& clamped,
                  const LcdRenderInfo& render)
{
    if (clamped.pluginMode != PluginMode::Fx
        || clamped.fxWindow != engine::FxWindow::Free)
        return false;
    return render.fxClampActive || clamped.timeFactor > raw.timeFactor + 1e-9;
}

// --- page builders ----------------------------------------------------------

/// S1000/S1100 SAMPLE-mode TIME-STRETCH page (ui-design §1 mockup; [MAN §3]).
LcdPage buildS1000SamplePage(const ParamSnapshot& p, const model::ModelSpec& spec,
                             const LcdSampleInfo& sample, const LcdRenderInfo& render)
{
    LcdPage page;
    page.rows.resize(kLcdRowsPage);

    // r0: TIME-STRETCH        sample: AMEN_165
    put(page.rows[0], 0, "TIME-STRETCH");
    put(page.rows[0], 20, "sample:");
    put(page.rows[0], 28,
        sample.name.empty() ? "-" : truncated(sample.name, kNameLen));

    // r1: stretch zone:        0  to:    131071
    put(page.rows[1], 0, "stretch zone:");
    putRight(page.rows[1], 14, 8, fmt("%lld", static_cast<long long>(sample.zoneStart)));
    put(page.rows[1], 24, "to:");
    putRight(page.rows[1], 28, 8, fmt("%lld", static_cast<long long>(sample.zoneEnd)));
    page.fields.push_back({LcdFieldKind::ZoneStart, ParamId::TimeFactor, 1, 14, 8, true, nullptr});
    page.fields.push_back({LcdFieldKind::ZoneEnd, ParamId::TimeFactor, 1, 28, 8, true, nullptr});

    // r2: Cycle length: 1000   time factor:    300%
    put(page.rows[2], 0, "Cycle length:");
    putRight(page.rows[2], 14, 4, LcdPageModel::formatCycleLen(p.cycleLen));
    put(page.rows[2], 20, "time factor:");
    putRight(page.rows[2], 32, 8, LcdPageModel::formatTimeFactor(p.timeFactor, p.hopMode));
    page.fields.push_back({LcdFieldKind::Param, ParamId::CycleLen, 2, 14, 4, true, nullptr});
    page.fields.push_back({LcdFieldKind::Param, ParamId::TimeFactor, 2, 32, 8, true, nullptr});

    // r3: stretch mode: CYCLIC  qual: -- width: --   (qual/width greyed,
    // "INTELL only" at v1 — [MAN §3 p.47]; ui-design §2)
    put(page.rows[3], 0, "stretch mode:");
    put(page.rows[3], 14, toLcd(p.stretchMode));
    put(page.rows[3], 22, "qual: --", LcdCellStyle::Greyed);
    put(page.rows[3], 31, "width: --", LcdCellStyle::Greyed);
    page.fields.push_back({LcdFieldKind::Param, ParamId::StretchMode, 3, 14, 6, true, nullptr});
    page.fields.push_back({LcdFieldKind::Param, ParamId::Qual, 3, 28, 2, false,
                           LcdPageModel::kIntellOnlyHint});
    page.fields.push_back({LcdFieldKind::Param, ParamId::Width, 3, 38, 2, false,
                           LcdPageModel::kIntellOnlyHint});

    // r4: new sample: AMEN_165*ST      mem:  7%
    put(page.rows[4], 0, "new sample:");
    put(page.rows[4], 12, newSampleName(sample));
    put(page.rows[4], 30, "mem:");
    putRight(page.rows[4], 34, 3, fmt("%d", sample.memPercent));
    put(page.rows[4], 37, "%");
    page.fields.push_back({LcdFieldKind::NewName, ParamId::TimeFactor, 4, 12,
                           static_cast<int>(kNameLen), true, nullptr});

    // r5: message line, else the achieved new-length readout. CLASSIC shows
    // the schedule-quantized length, never round(N*T) (dsp-engine.md §3.2).
    const std::string message = activeMessage(render);
    if (!message.empty())
    {
        put(page.rows[5], 0, message);
    }
    else if (render.achievedLengthFrames >= 0 && !sample.name.empty())
    {
        const double secs = static_cast<double>(render.achievedLengthFrames)
                            / spec.modelRateHz(p);
        put(page.rows[5], 0,
            fmt("new length: %lld (%.2f sec)",
                static_cast<long long>(render.achievedLengthFrames), secs));
    }
    return page;
}

/// S950 SAMPLE-mode page — the 2-line EDIT SAMPLE page 14, [MAN §2 p.30]:
///   ">14 STRETCH #New name NEW SAMPLE #200%"
///   " D-time 1000 #Auto D_ [Mon1/Pol2] 2 #Do_"
/// Deviation (PI): a bandwidth field is appended to line 2 — our plugin has
/// a single page per model and ParamVisibility exposes bandwidth on the S950.
LcdPage buildS950SamplePage(const ParamSnapshot& p, const LcdSampleInfo& sample,
                            const LcdRenderInfo& render)
{
    LcdPage page;
    page.rows.resize(kLcdRows2Line);

    // r0: >14 STRETCH NEW SAMPLE   L+R>MONO  999%
    put(page.rows[0], 0, ">14 STRETCH");
    put(page.rows[0], 13, newSampleName(sample));
    if (sample.monoSummed)
        put(page.rows[0], 25, LcdPageModel::kMonoSumNotice);
    putRight(page.rows[0], 33, 7, LcdPageModel::formatTimeFactor(p.timeFactor, p.hopMode));
    page.fields.push_back({LcdFieldKind::NewName, ParamId::TimeFactor, 0, 13,
                           static_cast<int>(kNameLen), true, nullptr});
    page.fields.push_back({LcdFieldKind::Param, ParamId::TimeFactor, 0, 33, 7, true, nullptr});

    // r1:  D-time 1000  AUTO-D  POL2  bw19.2  DO   (message replaces it —
    // 2-line displays flash messages on the bottom line)
    const std::string message = activeMessage(render);
    if (!message.empty())
    {
        put(page.rows[1], 0, message);
        return page;
    }
    put(page.rows[1], 1, "D-time");
    putRight(page.rows[1], 8, 4, LcdPageModel::formatCycleLen(p.cycleLen));
    put(page.rows[1], 14, "AUTO-D");
    put(page.rows[1], 22, toLcd(p.material));
    put(page.rows[1], 28, "bw");
    putRight(page.rows[1], 30, 4, LcdPageModel::formatBandwidth(p.bandwidth));
    put(page.rows[1], 36, "DO");
    page.fields.push_back({LcdFieldKind::Param, ParamId::CycleLen, 1, 8, 4, true, nullptr});
    page.fields.push_back({LcdFieldKind::Param, ParamId::Material, 1, 22, 4, true, nullptr});
    page.fields.push_back({LcdFieldKind::Param, ParamId::Bandwidth, 1, 30, 4, true, nullptr});
    return page;
}

/// S900 page — ADR-003 honesty: no timestretch, varispeed repitch, used in
/// both SAMPLE and FX mode (varispeed is inherently realtime). The LCD shows
/// the resulting semitone offset (-12*log2(T) + transpose, dsp-engine.md §6).
LcdPage buildS900Page(const ParamSnapshot& p, const LcdSampleInfo& sample,
                      const LcdRenderInfo& render)
{
    LcdPage page;
    page.rows.resize(kLcdRows2Line);

    // r0: the ADR-003 notice, verbatim.
    put(page.rows[0], 0, LcdPageModel::kS900Notice);

    // r1: pitch -12.00st tr +0.00 bw16.0 L+R>MONO  (message replaces it)
    const std::string message = activeMessage(render);
    if (!message.empty())
    {
        put(page.rows[1], 0, message);
        return page;
    }
    const double t = p.timeFactor / 100.0;
    const double pitchSt = -12.0 * std::log2(t) + p.transpose;
    put(page.rows[1], 0, "pitch");
    putRight(page.rows[1], 6, 6, LcdPageModel::formatTranspose(pitchSt));
    put(page.rows[1], 12, "st");
    put(page.rows[1], 15, "tr");
    putRight(page.rows[1], 18, 6, LcdPageModel::formatTranspose(p.transpose));
    put(page.rows[1], 25, "bw");
    putRight(page.rows[1], 27, 4, LcdPageModel::formatBandwidth(p.bandwidth));
    if (sample.monoSummed)
        put(page.rows[1], 32, LcdPageModel::kMonoSumNotice);
    page.fields.push_back({LcdFieldKind::Param, ParamId::Transpose, 1, 18, 6, true, nullptr});
    page.fields.push_back({LcdFieldKind::Param, ParamId::Bandwidth, 1, 27, 4, true, nullptr});
    return page;
}

/// FX-mode "TIME-STRETCH (REALTIME)" page, S1000-family layout
/// (ui-design §6.4 — clearly non-authentic wording).
LcdPage buildFxPage(const ParamSnapshot& raw, const ParamSnapshot& p,
                        const LcdRenderInfo& render)
{
    LcdPage page;
    page.rows.resize(kLcdRowsPage);

    // r0: TIME-STRETCH (REALTIME)
    put(page.rows[0], 0, LcdPageModel::kFxPageTitle);

    // r1: time factor:     100%  FX MIN 100%
    put(page.rows[1], 0, "time factor:");
    putRight(page.rows[1], 13, 8, LcdPageModel::formatTimeFactor(p.timeFactor, p.hopMode));
    if (fxClampShown(raw, p, render))
        put(page.rows[1], 24, LcdPageModel::kFxMinNotice, LcdCellStyle::Inverse);
    page.fields.push_back({LcdFieldKind::Param, ParamId::TimeFactor, 1, 13, 8, true, nullptr});

    // r2: window: 1/4 BAR  timing: CLASSIC
    put(page.rows[2], 0, "window:");
    put(page.rows[2], 8, toLcd(p.fxWindow));
    put(page.rows[2], 20, "timing:");
    put(page.rows[2], 28, toLcd(p.hopMode));

    // r3: sync: 174.0 -> 87.0 = 200%   (or "sync: OFF")
    put(page.rows[3], 0, "sync:");
    if (p.tempoSync == TempoSync::Host && render.sourceBpm > 0.0 && render.hostBpm > 0.0)
        put(page.rows[3], 6,
            LcdPageModel::formatSyncReadout(render.sourceBpm, render.hostBpm, p.hopMode));
    else
        put(page.rows[3], 6, "OFF");

    // r4: Cycle length: 1000   character: ON
    put(page.rows[4], 0, "Cycle length:");
    putRight(page.rows[4], 14, 4, LcdPageModel::formatCycleLen(p.cycleLen));
    put(page.rows[4], 20, "character:");
    put(page.rows[4], 31, p.character ? "ON" : "OFF");
    page.fields.push_back({LcdFieldKind::Param, ParamId::CycleLen, 4, 14, 4, true, nullptr});

    // r5: message line.
    const std::string message = activeMessage(render);
    if (!message.empty())
        put(page.rows[5], 0, message);
    return page;
}

/// FX-mode page for the 2-line S950 display class.
LcdPage buildS950FxPage(const ParamSnapshot& raw, const ParamSnapshot& p,
                        const LcdSampleInfo& sample, const LcdRenderInfo& render)
{
    LcdPage page;
    page.rows.resize(kLcdRows2Line);

    // r0: REALTIME STRETCH    300% FX MIN 100%
    put(page.rows[0], 0, "REALTIME STRETCH");
    putRight(page.rows[0], 17, 7, LcdPageModel::formatTimeFactor(p.timeFactor, p.hopMode));
    if (fxClampShown(raw, p, render))
        put(page.rows[0], 25, LcdPageModel::kFxMinNotice, LcdCellStyle::Inverse);
    page.fields.push_back({LcdFieldKind::Param, ParamId::TimeFactor, 0, 17, 7, true, nullptr});

    // r1:  D-time 1000  sync 174.0->87.0=200% / L+R>MONO  (message replaces)
    const std::string message = activeMessage(render);
    if (!message.empty())
    {
        put(page.rows[1], 0, message);
        return page;
    }
    put(page.rows[1], 1, "D-time");
    putRight(page.rows[1], 8, 4, LcdPageModel::formatCycleLen(p.cycleLen));
    if (p.tempoSync == TempoSync::Host && render.sourceBpm > 0.0 && render.hostBpm > 0.0)
    {
        const long long factor = std::llround(100.0 * render.sourceBpm / render.hostBpm);
        put(page.rows[1], 14,
            fmt("%.1f->%.1f=%lld%%", render.sourceBpm, render.hostBpm, factor));
    }
    else if (sample.monoSummed)
    {
        put(page.rows[1], 14, LcdPageModel::kMonoSumNotice);
    }
    page.fields.push_back({LcdFieldKind::Param, ParamId::CycleLen, 1, 8, 4, true, nullptr});
    return page;
}

} // namespace

// ---------------------------------------------------------------------------
// LcdPage
// ---------------------------------------------------------------------------

std::string LcdPage::textJoined() const
{
    std::string joined;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (i > 0)
            joined += '\n';
        joined += rows[i].text;
    }
    return joined;
}

// ---------------------------------------------------------------------------
// Field labels (ui-design §7: screen-reader names = LCD field labels)
// ---------------------------------------------------------------------------

std::string fieldLabel(const LcdField& field)
{
    switch (field.kind)
    {
        case LcdFieldKind::ZoneStart: return "STRETCH ZONE START";
        case LcdFieldKind::ZoneEnd:   return "STRETCH ZONE END";
        case LcdFieldKind::NewName:   return "NEW SAMPLE NAME";
        case LcdFieldKind::Param:
            switch (field.param)
            {
                case ParamId::TimeFactor:    return "TIME FACTOR";
                case ParamId::CycleLen:      return "CYCLE LENGTH";
                case ParamId::StretchMode:   return "STRETCH MODE";
                case ParamId::HopMode:       return "HOP MODE";
                case ParamId::Transpose:     return "TRANSPOSE";
                case ParamId::Qual:          return "QUALITY";
                case ParamId::Width:         return "WIDTH";
                case ParamId::Material:      return "MATERIAL";
                case ParamId::Bandwidth:     return "BANDWIDTH";
                case ParamId::SampleRateSel: return "SAMPLE RATE";
                case ParamId::Character:     return "CHARACTER";
                case ParamId::Norm:          return "NORMALIZE";
                case ParamId::TempoSync:     return "TEMPO SYNC";
                case ParamId::FxWindow:      return "FX WINDOW";
                case ParamId::OutTrim:       return "OUTPUT TRIM";
            }
            break;
    }
    return "LCD FIELD";
}

// ---------------------------------------------------------------------------
// LcdPageModel
// ---------------------------------------------------------------------------

LcdPage LcdPageModel::build(const engine::ParamSnapshot& params,
                            const model::ModelSpec& spec,
                            const LcdSampleInfo& sample,
                            const LcdRenderInfo& render)
{
    // The LCD shows the CLAMPED hardware value (dsp-engine.md §2;
    // ModelSpec::clamp is the single clamping authority).
    const engine::ParamSnapshot p = spec.clamp(params);

    // S900: the varispeed page in both modes (ADR-003; dsp-engine.md §6).
    if (spec.id == model::ModelId::S900)
        return buildS900Page(p, sample, render);

    if (p.pluginMode == PluginMode::Fx)
        return spec.variableClock ? buildS950FxPage(params, p, sample, render)
                                  : buildFxPage(params, p, render);

    return spec.variableClock ? buildS950SamplePage(p, sample, render)
                              : buildS1000SamplePage(p, spec, sample, render);
}

std::string LcdPageModel::formatTimeFactor(double pct, engine::HopMode mode)
{
    if (mode == HopMode::Classic)
        return fmt("%lld%%", std::llround(pct));
    return fmt("%.2f%%", pct);
}

std::string LcdPageModel::formatCycleLen(int samples)
{
    return fmt("%d", samples);
}

std::string LcdPageModel::formatTranspose(double semitones)
{
    return fmt("%+.2f", semitones);
}

std::string LcdPageModel::formatBandwidth(double kHz)
{
    return fmt("%.1f", kHz);
}

std::string LcdPageModel::formatSyncReadout(double sourceBpm, double hostBpm,
                                            engine::HopMode mode)
{
    // timeFactor := 100 * sourceBPM / hostBPM (dsp-engine.md §2 tempoSync
    // row; e.g. a 174 BPM loop into an 87 BPM host => 200%, longer). CLASSIC
    // quantizes the factor to integer % — the readout shows what is achieved.
    const double factor = 100.0 * sourceBpm / hostBpm;
    return fmt("%.1f -> %.1f = ", sourceBpm, hostBpm)
           + formatTimeFactor(factor, mode);
}

std::string LcdPageModel::formatProgress(int percent, double remainingSeconds)
{
    // "The display will display a message showing that the time-stretch is in
    // progress, and will also show you how much processing time remains"
    // [MAN §3 p.47]. Wording (PI).
    const auto total = static_cast<long long>(std::ceil(std::max(0.0, remainingSeconds)));
    return fmt("** STRETCHING %d%%  REMAIN %lld:%02lld **", percent, total / 60,
               total % 60);
}

std::string LcdPageModel::messageForLoadError(LcdLoadError error)
{
    // Hardware-idiom wording, `** WRONG DISK **`-flavored (PI) —
    // ui-design §6.1 step 3. This map IS the code->message authority.
    switch (error)
    {
        case LcdLoadError::None:              return {};
        case LcdLoadError::UnsupportedFormat: return "** WRONG DISK - FORMAT NOT SUPPORTED **";
        case LcdLoadError::ReadFailure:       return "** DISK ERROR - LOAD FAILED **";
    }
    return {};
}

} // namespace mws::ui
