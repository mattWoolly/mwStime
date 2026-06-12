// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdDisplay — the hero component (docs/design/ui-design.md §2, §4; ADR-005).
// A 40×6 character-cell grid (PI) rendered with the bespoke 5×7 pixel font
// (LcdFont.h) in lcdInk on lcdBack, with a radial-gradient backlight glow and
// a subtle scanline overlay. ONE component serves both hardware display
// classes — `setLayout` selects the active region (S950_2LINE uses a 40×2
// region with proportionally larger characters, like the period fluorescent
// display; S1000_PAGE uses the full 40×6 page). Layout is data, not a
// component fork (ADR-005 ruling).
//
// This is the dynamic layer that repaints over the cached faceplate (the
// Faceplate draws the bezel; this component sits on the glass). Content is
// driven cell-by-cell — the LcdPageModel that formats pages arrives in task
// 041; cursor-key navigation that moves the field cursor is task 045.

#pragma once

#include <array>
#include <string_view>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FaceplateSpec.h"
#include "LcdFont.h"

namespace mws::ui {

class LcdDisplay final : public juce::Component, private juce::Timer
{
public:
    /// Full character-cell grid (ui-design §2, all (PI)).
    static constexpr int kCols = 40;
    static constexpr int kRows = 6;
    /// Active rows of the S950_2LINE region [MAN §2 display class].
    static constexpr int kRows2Line = 2;

    /// Per-cell render style (ui-design task 040 API):
    ///  - Greyed renders the "INTELL only" qual/width fields dimmed — the
    ///    hardware itself greys them in CYCLIC mode [MAN §3 p.47].
    ///  - Inverse fills the cell with ink and draws the glyph in lcdBack.
    ///  - Blink hides/shows the glyph on the blink phase (~530 ms period).
    enum class Style : std::uint8_t { normal, greyed, inverse, blink };

    LcdDisplay();
    ~LcdDisplay() override;

    // --- palette / layout (data variants of ONE component, ADR-005) --------
    /// Adopts lcdBack/lcdInk/lcdGlow AND the layout class from a model spec.
    void setSpec(const FaceplateSpec& spec);
    /// Selects the active region: S950_2Line → 40×2, S1000_Page → 40×6.
    void setLayout(LcdLayout newLayout);
    [[nodiscard]] LcdLayout layout() const noexcept { return currentLayout; }
    [[nodiscard]] int activeRows() const noexcept
    {
        return currentLayout == LcdLayout::S950_2Line ? kRows2Line : kRows;
    }
    [[nodiscard]] int activeCols() const noexcept { return kCols; }

    // --- content ------------------------------------------------------------
    /// Blanks every cell (and hides the cursor).
    void clear();

    /// Writes one cell. Out-of-range row/col (or a row outside the active
    /// region) is silently ignored — never an assertion.
    void setCell(int row, int col, char character, Style style = Style::normal);

    /// Writes a line starting at column 0, blank-padding to the right edge.
    /// Text longer than 40 columns is clipped — never an assertion. Rows
    /// outside the active region are ignored.
    void setLine(int row, std::string_view text, Style style = Style::normal);

    [[nodiscard]] char cellChar(int row, int col) const noexcept;
    [[nodiscard]] Style cellStyle(int row, int col) const noexcept;

    // --- field cursor (block cursor on the focused field; nav is task 045) --
    void setCursor(int row, int col);
    void clearCursor();
    [[nodiscard]] bool hasCursor() const noexcept { return cursorRow >= 0; }

    void paint(juce::Graphics& g) override;

private:
    struct Cell {
        unsigned char ch = ' ';
        Style style = Style::normal;
    };

    [[nodiscard]] bool cellInActiveRegion(int row, int col) const noexcept;
    void updateBlinkTimer();
    void timerCallback() override;

    std::array<std::array<Cell, kCols>, kRows> cells;

    LcdLayout currentLayout = LcdLayout::S1000_Page;
    juce::Colour back, ink, glow;

    int cursorRow = -1, cursorCol = -1;
    bool blinkPhase = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LcdDisplay)
};

} // namespace mws::ui
