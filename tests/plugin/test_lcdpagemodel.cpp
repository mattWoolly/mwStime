// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 041 — LcdPageModel headless tests (docs/design/testing-strategy.md §2
// LcdPageModel bullet): hardware-unit formatting, per-model field visibility
// (dsp-engine.md §2), clamp feedback strings (999% on S950, FX MIN 100%),
// achieved-length honesty (engine schedule, never round(N*T)), hardware-idiom
// load-error lines, and the task-032 embed/path-only status line.
//
// Test-case names begin with the tag word so `ctest -R lcdpagemodel` matches
// (plan/backlog/README.md test-selection rules). No GUI, no window: the page
// model is pure C++.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/LcdPageModel.h"

#include "mws/engine/Params.h"
#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"
#include "mws/stretch/CyclicEngine.h"

using mws::engine::HopMode;
using mws::engine::ParamId;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::TempoSync;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::ui::LcdCellStyle;
using mws::ui::LcdField;
using mws::ui::LcdFieldKind;
using mws::ui::LcdLoadError;
using mws::ui::LcdPage;
using mws::ui::LcdPageModel;
using mws::ui::LcdRenderInfo;
using mws::ui::LcdSampleInfo;

namespace {

ParamSnapshot snapshotFor(ModelId model, PluginMode mode)
{
    ParamSnapshot p;
    p.model = model;
    p.pluginMode = mode;
    return p;
}

LcdPage build(const ParamSnapshot& p, const LcdSampleInfo& sample = {},
              const LcdRenderInfo& render = {})
{
    return LcdPageModel::build(p, ModelSpec::get(p.model), sample, render);
}

bool contains(const LcdPage& page, const std::string& needle)
{
    return page.textJoined().find(needle) != std::string::npos;
}

const LcdField* findParamField(const LcdPage& page, ParamId param)
{
    for (const auto& f : page.fields)
        if (f.kind == LcdFieldKind::Param && f.param == param)
            return &f;
    return nullptr;
}

LcdSampleInfo amenSample()
{
    LcdSampleInfo s;
    s.name = "AMEN_165";
    s.lengthFrames = 131072;
    s.zoneStart = 0;
    s.zoneEnd = 131071;
    s.memPercent = 7;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Hardware-unit formatting (testing-strategy §2)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: hardware-unit formatting", "[lcdpagemodel]")
{
    // timeFactor 300 -> "300%" (integer % in CLASSIC, [AKZ §2.1])
    CHECK(LcdPageModel::formatTimeFactor(300.0, HopMode::Classic) == "300%");
    // REVISED keeps the 0.01 step visible.
    CHECK(LcdPageModel::formatTimeFactor(287.5, HopMode::Revised) == "287.50%");
    // cycle 1000 -> "1000" (samples at model rate [MAN §3 p.46])
    CHECK(LcdPageModel::formatCycleLen(1000) == "1000");
    // transpose -12 -> "-12.00" (1-cent steps [MAN §3 p.81])
    CHECK(LcdPageModel::formatTranspose(-12.0) == "-12.00");
    CHECK(LcdPageModel::formatTranspose(0.0) == "+0.00");
}

TEST_CASE("lcdpagemodel: formatted values land on the S1000 page", "[lcdpagemodel]")
{
    auto p = snapshotFor(ModelId::S1000, PluginMode::Sample);
    p.timeFactor = 300.0;
    p.cycleLen = 1000;

    const auto page = build(p, amenSample());
    CHECK(contains(page, "TIME-STRETCH"));
    CHECK(contains(page, "sample: AMEN_165"));
    CHECK(contains(page, "300%"));
    CHECK(contains(page, "1000"));
    CHECK(contains(page, "stretch mode: CYCLIC"));
    CHECK(contains(page, "new sample: AMEN_165*ST"));
    CHECK(contains(page, "mem:  7%"));
}

TEST_CASE("lcdpagemodel: every row is exactly 40 cells", "[lcdpagemodel]")
{
    for (const ModelId model : {ModelId::S900, ModelId::S950, ModelId::S1000,
                                ModelId::S1100})
        for (const PluginMode mode : {PluginMode::Fx, PluginMode::Sample})
        {
            const auto page = build(snapshotFor(model, mode), amenSample());
            REQUIRE(!page.rows.empty());
            const std::size_t expectedRows =
                ModelSpec::get(model).variableClock
                    ? static_cast<std::size_t>(mws::ui::kLcdRows2Line)
                    : static_cast<std::size_t>(mws::ui::kLcdRowsPage);
            CHECK(page.rows.size() == expectedRows);
            for (const auto& row : page.rows)
                CHECK(row.text.size() == static_cast<std::size_t>(mws::ui::kLcdCols));
            // Field rects must lie inside the page.
            for (const auto& f : page.fields)
            {
                CHECK(f.row >= 0);
                CHECK(f.row < static_cast<int>(page.rows.size()));
                CHECK(f.col >= 0);
                CHECK(f.col + f.width <= mws::ui::kLcdCols);
            }
        }
}

// ---------------------------------------------------------------------------
// Clamp feedback (testing-strategy §2; dsp-engine.md §2 — the LCD shows the
// CLAMPED hardware value)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: S950 caps the displayed time factor at 999%", "[lcdpagemodel]")
{
    auto p = snapshotFor(ModelId::S950, PluginMode::Sample);
    p.timeFactor = 1500.0;  // superset allows it; the S950 engine-clamps [MAN §2]

    const auto page = build(p, amenSample());
    CHECK(contains(page, "999%"));
    CHECK_FALSE(contains(page, "1500"));
}

TEST_CASE("lcdpagemodel: FX FREE T=80 shows the FX MIN 100% clamp notice", "[lcdpagemodel]")
{
    auto p = snapshotFor(ModelId::S1000, PluginMode::Fx);
    p.fxWindow = mws::engine::FxWindow::Free;
    p.timeFactor = 80.0;  // ADR-006: compression needs future input

    const auto page = build(p);
    CHECK(contains(page, LcdPageModel::kFxMinNotice));  // "FX MIN 100%"
    CHECK(contains(page, "100%"));                      // the clamped value
    CHECK(contains(page, LcdPageModel::kFxPageTitle));  // ui-design §6.4 page

    // No clamp, no notice.
    p.timeFactor = 200.0;
    CHECK_FALSE(contains(build(p), LcdPageModel::kFxMinNotice));
}

// ---------------------------------------------------------------------------
// Per-model field visibility (dsp-engine.md §2 "Applies to"; §6 for S900)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: S900 hides cycle and stretch-mode fields", "[lcdpagemodel]")
{
    for (const PluginMode mode : {PluginMode::Fx, PluginMode::Sample})
    {
        const auto page = build(snapshotFor(ModelId::S900, mode), amenSample());
        CHECK(findParamField(page, ParamId::CycleLen) == nullptr);
        CHECK(findParamField(page, ParamId::StretchMode) == nullptr);
        CHECK_FALSE(contains(page, "Cycle length"));
        CHECK_FALSE(contains(page, "stretch mode"));
        // ADR-003 honesty notice + semitone readout instead.
        CHECK(contains(page, LcdPageModel::kS900Notice));
        // pitch -12*log2(2.0) = -12.00 st at the default-ish T... (T=100 here
        // so the readout is +0.00; the varispeed math test below pins -12).
        CHECK(findParamField(page, ParamId::Transpose) != nullptr);
    }
}

TEST_CASE("lcdpagemodel: S900 varispeed semitone readout", "[lcdpagemodel]")
{
    auto p = snapshotFor(ModelId::S900, PluginMode::Sample);
    p.timeFactor = 200.0;  // half speed => -12 st (dsp-engine.md §6)

    const auto page = build(p, amenSample());
    CHECK(contains(page, "-12.00"));
}

TEST_CASE("lcdpagemodel: S950 hides the mode row", "[lcdpagemodel]")
{
    const auto page =
        build(snapshotFor(ModelId::S950, PluginMode::Sample), amenSample());
    CHECK(findParamField(page, ParamId::StretchMode) == nullptr);
    CHECK_FALSE(contains(page, "stretch mode"));
    CHECK_FALSE(contains(page, "CYCLIC"));
    // ...and shows the page-14 layout instead [MAN §2 p.30].
    CHECK(contains(page, ">14 STRETCH"));
    CHECK(contains(page, "D-time"));
    CHECK(contains(page, "AUTO-D"));
    CHECK(contains(page, "POL2"));
    CHECK(findParamField(page, ParamId::Material) != nullptr);
}

TEST_CASE("lcdpagemodel: S1000 greys stretchMode/qual/width as INTELL only", "[lcdpagemodel]")
{
    const auto page =
        build(snapshotFor(ModelId::S1000, PluginMode::Sample), amenSample());

    // INTELL is deferred to v1.1 and genuinely unreachable at v1: stretchMode
    // joins qual/width as a greyed, cursor-skipped, INTELL-only field so the
    // engine never renders a guess under the authentic name (ADR-001 res.#2;
    // dsp-engine.md §4 L239-240; task 054 / QA F1+F3).
    for (const ParamId param : {ParamId::StretchMode, ParamId::Qual, ParamId::Width})
    {
        const auto* field = findParamField(page, param);
        REQUIRE(field != nullptr);
        CHECK_FALSE(field->editable);  // cursor skips greyed fields
        REQUIRE(field->hint != nullptr);
        CHECK(std::string(field->hint) == LcdPageModel::kIntellOnlyHint);
        const auto& row = page.rows[static_cast<std::size_t>(field->row)];
        for (int c = field->col; c < field->col + field->width; ++c)
            CHECK(row.styles[static_cast<std::size_t>(c)] == LcdCellStyle::Greyed);
    }

    // The greyed set on the S1000 page is EXACTLY {stretchMode, qual, width}:
    // every other Param field stays editable so the cursor can reach it.
    for (const auto& f : page.fields)
    {
        if (f.kind != mws::ui::LcdFieldKind::Param)
            continue;
        const bool greyed = f.param == ParamId::StretchMode
                            || f.param == ParamId::Qual
                            || f.param == ParamId::Width;
        INFO("param field index editable mismatch");
        CHECK(f.editable == !greyed);
    }

    // The mockup renders the inert values as dashes (ui-design §1).
    CHECK(contains(page, "qual: --"));
    CHECK(contains(page, "width: --"));
}

// ---------------------------------------------------------------------------
// Achieved-length honesty (dsp-engine.md §3.2/§3.4; ui-design §6.2 step 2)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: CLASSIC new-length readout is the schedule-derived value",
          "[lcdpagemodel]")
{
    auto p = snapshotFor(ModelId::S1000, PluginMode::Sample);
    p.timeFactor = 300.0;
    p.cycleLen = 1000;
    p.hopMode = HopMode::Classic;

    auto sample = amenSample();
    const std::int64_t n = sample.lengthFrames;

    // The readout source contract: task-010 CyclicEngine::expectedOutputLength.
    const mws::stretch::CyclicEngine engine;
    const std::int64_t achieved =
        engine.expectedOutputLength(n, p.cycleLen, p.timeFactor, p.hopMode);

    const auto naive = static_cast<std::int64_t>(
        std::llround(static_cast<double>(n) * p.timeFactor / 100.0));
    REQUIRE(achieved != naive);  // params chosen so the quantization shows

    LcdRenderInfo render;
    render.achievedLengthFrames = achieved;
    const auto page = build(p, sample, render);

    CHECK(contains(page, std::to_string(achieved)));        // the honest value
    CHECK_FALSE(contains(page, std::to_string(naive)));     // never round(N*T)
    CHECK(contains(page, "new length:"));
}

// ---------------------------------------------------------------------------
// Message lines
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: load-error codes map to hardware-idiom lines", "[lcdpagemodel]")
{
    LcdRenderInfo render;
    render.loadError = LcdLoadError::UnsupportedFormat;

    // The code->message map lives in LcdPageModel (ui-design §6.1 step 3).
    CHECK(LcdPageModel::messageForLoadError(LcdLoadError::UnsupportedFormat)
              .find("WRONG DISK") != std::string::npos);
    CHECK(LcdPageModel::messageForLoadError(LcdLoadError::None).empty());

    for (const ModelId model : {ModelId::S950, ModelId::S1000})
    {
        const auto page =
            build(snapshotFor(model, PluginMode::Sample), amenSample(), render);
        CHECK(contains(page, "WRONG DISK"));
    }
}

TEST_CASE("lcdpagemodel: over-cap sample shows the path-only embed status", "[lcdpagemodel]")
{
    LcdRenderInfo render;
    render.pathOnlyPersistence = true;  // task-032 over-16MB flag

    const auto page =
        build(snapshotFor(ModelId::S1000, PluginMode::Sample), amenSample(), render);
    CHECK(contains(page, "PATH ONLY"));
    CHECK(contains(page, LcdPageModel::kPathOnlyNotice));
}

TEST_CASE("lcdpagemodel: not-enough-memory refusal line", "[lcdpagemodel]")
{
    LcdRenderInfo render;
    render.notEnoughMemory = true;

    const auto page =
        build(snapshotFor(ModelId::S1000, PluginMode::Sample), amenSample(), render);
    CHECK(contains(page, "** NOT ENOUGH MEMORY **"));
}

TEST_CASE("lcdpagemodel: progress line shows percent and remaining time", "[lcdpagemodel]")
{
    LcdRenderInfo render;
    render.progressPercent = 42;
    render.remainingSeconds = 7.0;

    const auto page =
        build(snapshotFor(ModelId::S1000, PluginMode::Sample), amenSample(), render);
    CHECK(contains(page, "42%"));
    CHECK(contains(page, "0:07"));
}

TEST_CASE("lcdpagemodel: message priority — load error beats embed status", "[lcdpagemodel]")
{
    LcdRenderInfo render;
    render.loadError = LcdLoadError::ReadFailure;
    render.pathOnlyPersistence = true;

    const auto page =
        build(snapshotFor(ModelId::S1000, PluginMode::Sample), amenSample(), render);
    CHECK(contains(page, "DISK ERROR"));
    CHECK_FALSE(contains(page, "PATH ONLY"));
}

// ---------------------------------------------------------------------------
// Mono-sum notice (dsp-engine.md §5: authentic, stated on the LCD)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: mono-sum notice on S900/S950, never on S1000", "[lcdpagemodel]")
{
    auto sample = amenSample();
    sample.monoSummed = true;

    for (const ModelId model : {ModelId::S900, ModelId::S950})
        CHECK(contains(build(snapshotFor(model, PluginMode::Sample), sample),
                       LcdPageModel::kMonoSumNotice));

    CHECK_FALSE(contains(build(snapshotFor(ModelId::S1000, PluginMode::Sample), sample),
                         LcdPageModel::kMonoSumNotice));

    // Not shown when the input was not summed.
    CHECK_FALSE(contains(build(snapshotFor(ModelId::S950, PluginMode::Sample),
                               amenSample()),
                         LcdPageModel::kMonoSumNotice));
}

// ---------------------------------------------------------------------------
// Sync readout (ui-design §6.2 step 4)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: sync readout 174.0 -> 87.0 = 200%", "[lcdpagemodel]")
{
    CHECK(LcdPageModel::formatSyncReadout(174.0, 87.0, HopMode::Classic)
          == "174.0 -> 87.0 = 200%");

    auto p = snapshotFor(ModelId::S1000, PluginMode::Fx);
    p.tempoSync = TempoSync::Host;
    LcdRenderInfo render;
    render.sourceBpm = 174.0;
    render.hostBpm = 87.0;

    const auto page = build(p, {}, render);
    CHECK(contains(page, "174.0 -> 87.0 = 200%"));
}

// ---------------------------------------------------------------------------
// Field map (cursor order + bindings, consumed by task 045)
// ---------------------------------------------------------------------------

TEST_CASE("lcdpagemodel: S1000 page field map order and bindings", "[lcdpagemodel]")
{
    const auto page =
        build(snapshotFor(ModelId::S1000, PluginMode::Sample), amenSample());

    std::vector<LcdFieldKind> kinds;
    std::vector<ParamId> params;
    for (const auto& f : page.fields)
    {
        kinds.push_back(f.kind);
        if (f.kind == LcdFieldKind::Param)
            params.push_back(f.param);
    }

    // Hardware cursor order: left-to-right, top-to-bottom [MAN §3].
    CHECK(kinds == std::vector<LcdFieldKind>{
              LcdFieldKind::ZoneStart, LcdFieldKind::ZoneEnd, LcdFieldKind::Param,
              LcdFieldKind::Param, LcdFieldKind::Param, LcdFieldKind::Param,
              LcdFieldKind::Param, LcdFieldKind::NewName});
    CHECK(params == std::vector<ParamId>{ParamId::CycleLen, ParamId::TimeFactor,
                                         ParamId::StretchMode, ParamId::Qual,
                                         ParamId::Width});

    // Fields are ordered for cursor traversal (row-major, non-decreasing).
    for (std::size_t i = 1; i < page.fields.size(); ++i)
    {
        const auto& a = page.fields[i - 1];
        const auto& b = page.fields[i];
        CHECK((b.row > a.row || (b.row == a.row && b.col > a.col)));
    }
}
