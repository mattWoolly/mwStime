// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdPageModel (task 041) — the pure-C++, GUI-free single source of truth for
// everything the LCD shows (docs/design/ui-design.md §2): per-model page text
// mirroring the manuals' printed screens, hardware-unit formatting
// (dsp-engine.md §2 — the LCD always shows the CLAMPED hardware value),
// engine-clamp feedback strings (`999%` cap on the S950, `FX MIN 100%` in FX
// FREE — ADR-006), the CLASSIC achieved-length honesty readout (never
// round(N*T) — dsp-engine.md §3.2), hardware-idiom message lines, and the
// per-model editable-field map consumed by the editor (task 045).
//
// HEADLESS RULE: this header and its .cpp include no JUCE headers at all —
// std::string only. The FileLoader error enum (task 031) and the task-032
// embed/path-only flag are mirrored here as plain input fields so the file
// stays JUCE-free and unit-testable without a window.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

namespace mws::ui {

// ---------------------------------------------------------------------------
// LCD geometry (ui-design §2): one 40-column character-cell display; the
// S1000-family page uses 6 rows, the S900/S950 hardware display class uses a
// 40x2 region. Which class a model uses is derived from ModelSpec
// (variableClock <=> the 2-line varclock machines) so this file does not
// depend on the JUCE-side FaceplateSpec table.
// ---------------------------------------------------------------------------

inline constexpr int kLcdCols = 40;
inline constexpr int kLcdRowsPage = 6;   ///< S1000-family multi-line page
inline constexpr int kLcdRows2Line = 2;  ///< S900/S950 2-line display class

/// Per-cell render style (matches the task-040 LcdDisplay style set).
/// Greyed is the "INTELL only" treatment for qual/width at v1 [MAN §3 p.47].
enum class LcdCellStyle : std::uint8_t { Normal, Greyed, Inverse, Blink };

/// One LCD row: exactly kLcdCols characters plus a per-cell style.
struct LcdRow {
    std::string text = std::string(kLcdCols, ' ');
    std::array<LcdCellStyle, kLcdCols> styles{};  // zero-init == Normal
};

// ---------------------------------------------------------------------------
// Field map — which cells are editable, their cursor order, and each field's
// parameter binding (consumed by task 045 for cursor-key navigation and jog
// editing). Bindings use the JUCE-free core ParamId; non-parameter fields
// (stretch zone, new-name text entry) carry their own kind.
// ---------------------------------------------------------------------------

enum class LcdFieldKind : std::uint8_t {
    Param,      ///< bound to an engine::ParamId
    ZoneStart,  ///< "stretch zone:" [MAN §3] — sample-zone value, not a param
    ZoneEnd,    ///< "to:" [MAN §3]
    NewName,    ///< destination sample name text field [MAN §2 p.30, §3 p.46]
};

struct LcdField {
    LcdFieldKind kind = LcdFieldKind::Param;
    engine::ParamId param = engine::ParamId::TimeFactor;  ///< valid when kind==Param
    int row = 0;     ///< cell row of the value text
    int col = 0;     ///< first cell column of the value text
    int width = 0;   ///< value width in cells
    bool editable = true;        ///< false == greyed (cursor skips it)
    const char* hint = nullptr;  ///< e.g. "INTELL only" for greyed qual/width
};

/// The human-readable LCD label for a field — the printed-on-the-screen name of
/// the value the cursor is on ("TIME FACTOR", "CYCLE LENGTH", "STRETCH ZONE
/// START", "NEW SAMPLE NAME", …). The PluginEditor surfaces this as the focused
/// field's screen-reader name so "screen-reader names = LCD field labels"
/// (ui-design §7) holds verbatim. JUCE-free (std::string); the editor wraps it
/// in a JUCE accessibility title.
[[nodiscard]] std::string fieldLabel(const LcdField& field);

/// A complete LCD page: rows of {text, perCellStyle} plus the field map in
/// cursor-navigation order.
struct LcdPage {
    std::vector<LcdRow> rows;
    std::vector<LcdField> fields;

    /// All row texts joined with '\n' — test/debug convenience.
    [[nodiscard]] std::string textJoined() const;
};

// ---------------------------------------------------------------------------
// Plain-struct inputs (ui-design §2: the page model is fed by the editor's
// FIFO poll; everything arrives as POD so this stays headless).
// ---------------------------------------------------------------------------

/// Typed file-load error, mirrored from the task-031 FileLoader so this file
/// stays JUCE-free. The code -> hardware-idiom LCD message map lives in
/// LcdPageModel::messageForLoadError (ui-design §6.1 step 3).
enum class LcdLoadError : std::uint8_t {
    None,
    UnsupportedFormat,  ///< not WAV/AIFF/FLAC (no MP3 at v1)
    ReadFailure,        ///< decode/IO failure
};

/// Loaded-sample info (from FileLoader / sample slot).
struct LcdSampleInfo {
    std::string name;             ///< loaded sample name; empty == none loaded
    std::string newName;          ///< render destination; empty -> name + "*ST"
    std::int64_t lengthFrames = 0;
    std::int64_t zoneStart = 0;   ///< "stretch zone:" [MAN §3]
    std::int64_t zoneEnd = 0;     ///< "to:" (inclusive, hardware-style)
    int memPercent = 0;           ///< new-sample memory readout ("mem:  7%")
    bool monoSummed = false;      ///< stereo input summed (S900/S950, dsp §5)
};

/// Render/engine feedback (from the UI FIFO).
struct LcdRenderInfo {
    /// CLASSIC achieved (schedule-quantized) output length. CONTRACT: must be
    /// the task-010 CyclicEngine::expectedOutputLength value — never
    /// round(N*T) (achieved-length honesty, dsp-engine.md §3.2/§3.4,
    /// ui-design §6.2). Negative == no readout.
    std::int64_t achievedLengthFrames = -1;

    int progressPercent = -1;       ///< >= 0 == render in progress [MAN §3 p.47]
    double remainingSeconds = 0.0;  ///< remaining-time readout [MAN §3 p.47]
    bool notEnoughMemory = false;   ///< render refused (architecture.md §5.1)
    bool fxClampActive = false;     ///< engine-reported FX FREE clamp (ADR-006)
    double sourceBpm = 0.0;         ///< SYNC readout inputs (ui-design §6.2.4)
    double hostBpm = 0.0;
    LcdLoadError loadError = LcdLoadError::None;
    bool pathOnlyPersistence = false;  ///< task-032 over-16MB embed flag
};

// ---------------------------------------------------------------------------
// The page model.
// ---------------------------------------------------------------------------

class LcdPageModel
{
public:
    /// Builds the active page for the snapshot's model + plugin mode.
    /// Displays CLAMPED hardware values throughout: the snapshot is passed
    /// through ModelSpec::clamp (the single clamping authority) first, so the
    /// S950 page caps at 999% [MAN §2] and FX FREE shows 100% + the
    /// `FX MIN 100%` notice (ADR-006).
    [[nodiscard]] static LcdPage build(const engine::ParamSnapshot& params,
                                       const model::ModelSpec& spec,
                                       const LcdSampleInfo& sample,
                                       const LcdRenderInfo& render);

    // --- hardware-unit formatting (dsp-engine.md §2 table) -----------------

    /// TIME FACTOR: integer percent in CLASSIC ("Akai samplers don't use
    /// decimal values for Time Factor" [AKZ §2.1]) -> "300%"; two decimals in
    /// REVISED (0.01 step) -> "300.00%".
    [[nodiscard]] static std::string formatTimeFactor(double pct, engine::HopMode mode);

    /// CYCLE LENGTH / D-TIME, samples at model rate -> "1000".
    [[nodiscard]] static std::string formatCycleLen(int samples);

    /// TRANSPOSE, semitones in 1-cent steps -> "-12.00" / "+0.00".
    [[nodiscard]] static std::string formatTranspose(double semitones);

    /// BANDWIDTH, kHz, 0.1 steps -> "19.2".
    [[nodiscard]] static std::string formatBandwidth(double kHz);

    /// SYNC readout (ui-design §6.2 step 4): "174.0 -> 87.0 = 200%".
    /// Factor = 100 * sourceBpm / hostBpm (dsp-engine.md §2 tempoSync row);
    /// integer-quantized in CLASSIC.
    [[nodiscard]] static std::string formatSyncReadout(double sourceBpm,
                                                       double hostBpm,
                                                       engine::HopMode mode);

    /// Progress/remaining-time line [MAN §3 p.47] -> "** STRETCHING 42%  REMAIN 0:07 **".
    [[nodiscard]] static std::string formatProgress(int percent, double remainingSeconds);

    /// The typed-error -> hardware-idiom LCD message map (ui-design §6.1
    /// step 3; `** WRONG DISK **`-flavored wording (PI)).
    [[nodiscard]] static std::string messageForLoadError(LcdLoadError error);

    // --- fixed notice strings (asserted verbatim by tests) -----------------

    /// ADR-003 honesty notice on the S900 page (dsp-engine.md §6).
    static constexpr const char* kS900Notice = "S900: NO TIMESTRETCH - VARISPEED";

    /// ADR-006 FX FREE causality clamp feedback (dsp-engine.md §3.5).
    static constexpr const char* kFxMinNotice = "FX MIN 100%";

    /// Render-refused message (architecture.md §5.1; hardware [MAN §3 p.47]).
    static constexpr const char* kNotEnoughMemory = "** NOT ENOUGH MEMORY **";

    /// Mono-sum tag on S900/S950 pages (dsp-engine.md §5: "authentic, stated
    /// on the LCD"); terse 2-line-display idiom (PI).
    static constexpr const char* kMonoSumNotice = "L+R>MONO";

    /// Task-032 over-cap embed status (sample > 16 MB persists path-only).
    static constexpr const char* kPathOnlyNotice = "** SAMPLE > 16MB: PATH ONLY SAVED **";

    /// Greyed qual/width hint at v1 (ui-design §2; [MAN §3 p.47]).
    static constexpr const char* kIntellOnlyHint = "INTELL only";

    /// FX-mode page title — deliberately non-authentic wording so users know
    /// this mode exceeds hardware capability (ui-design §6.4).
    static constexpr const char* kFxPageTitle = "TIME-STRETCH (REALTIME)";
};

} // namespace mws::ui
