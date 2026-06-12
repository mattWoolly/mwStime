// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "LcdDisplay.h"

namespace mws::ui {

namespace {

/// Character cell footprint on the dot grid: 5 glyph dots + 1 spacing column,
/// 7 glyph dots + 2 spacing rows (PI — period module pitch).
constexpr float kCellDotsW = (float) lcdfont::kGlyphWidth + 1.0f;
constexpr float kCellDotsH = (float) lcdfont::kGlyphHeight + 2.0f;

/// Classic cursor/blink period (PI, ~ the period module blink rate).
constexpr int kBlinkIntervalMs = 530;

/// Greyed-style ink alpha (the "INTELL only" dim — PI).
constexpr float kGreyedAlpha = 0.30f;

} // namespace

LcdDisplay::LcdDisplay()
{
    // Default palette: the canonical S1000 grey + green look.
    setSpec(faceplateSpecFor(model::ModelId::S1000));
    setInterceptsMouseClicks(false, false);  // field focus/editing is task 045
    clear();
}

LcdDisplay::~LcdDisplay() { stopTimer(); }

// --- palette / layout --------------------------------------------------------

void LcdDisplay::setSpec(const FaceplateSpec& spec)
{
    back = spec.lcdBack;
    ink = spec.lcdInk;
    glow = spec.lcdGlow;
    setLayout(spec.lcdLayout);
    repaint();
}

void LcdDisplay::setLayout(LcdLayout newLayout)
{
    if (currentLayout == newLayout)
        return;
    currentLayout = newLayout;
    repaint();
}

// --- content -----------------------------------------------------------------

void LcdDisplay::clear()
{
    for (auto& row : cells)
        row.fill(Cell{});
    clearCursor();
    repaint();
}

bool LcdDisplay::cellInActiveRegion(int row, int col) const noexcept
{
    return row >= 0 && row < activeRows() && col >= 0 && col < kCols;
}

void LcdDisplay::setCell(int row, int col, char character, Style style)
{
    if (! cellInActiveRegion(row, col))
        return;  // silently clipped — never an assertion (task 040 contract)

    auto& cell = cells[(size_t) row][(size_t) col];
    const auto ch = (unsigned char) character;
    if (cell.ch == ch && cell.style == style)
        return;

    cell = { ch, style };
    updateBlinkTimer();
    repaint();
}

void LcdDisplay::setLine(int row, std::string_view text, Style style)
{
    if (row < 0 || row >= activeRows())
        return;

    for (int col = 0; col < kCols; ++col)
    {
        const char ch = (size_t) col < text.size() ? text[(size_t) col] : ' ';
        setCell(row, col, ch, style);
    }
}

char LcdDisplay::cellChar(int row, int col) const noexcept
{
    if (row < 0 || row >= kRows || col < 0 || col >= kCols)
        return '\0';
    return (char) cells[(size_t) row][(size_t) col].ch;
}

LcdDisplay::Style LcdDisplay::cellStyle(int row, int col) const noexcept
{
    if (row < 0 || row >= kRows || col < 0 || col >= kCols)
        return Style::normal;
    return cells[(size_t) row][(size_t) col].style;
}

// --- field cursor ------------------------------------------------------------

void LcdDisplay::setCursor(int row, int col)
{
    if (! cellInActiveRegion(row, col))
        return;
    cursorRow = row;
    cursorCol = col;
    blinkPhase = true;  // cursor moves land on the visible phase (period feel)
    updateBlinkTimer();
    repaint();
}

void LcdDisplay::clearCursor()
{
    cursorRow = cursorCol = -1;
    updateBlinkTimer();
    repaint();
}

// --- blink -------------------------------------------------------------------

void LcdDisplay::updateBlinkTimer()
{
    bool needsBlink = hasCursor();
    if (! needsBlink)
        for (const auto& row : cells)
            for (const auto& cell : row)
                needsBlink |= (cell.style == Style::blink);

    if (needsBlink && ! isTimerRunning())
        startTimer(kBlinkIntervalMs);
    else if (! needsBlink && isTimerRunning())
    {
        stopTimer();
        blinkPhase = true;
    }
}

void LcdDisplay::timerCallback()
{
    blinkPhase = ! blinkPhase;
    repaint();
}

// --- rendering (ui-design §2, §4) ---------------------------------------------

void LcdDisplay::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    const float corner = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.04f;

    // Glass background.
    g.setColour(back);
    g.fillRoundedRectangle(bounds, corner);

    // Backlight: radial gradient glow, brightest behind the centre (PI).
    {
        juce::ColourGradient backlight(glow, bounds.getCentreX(), bounds.getCentreY(),
                                       glow.withAlpha(0.0f), bounds.getX(),
                                       bounds.getY(), true);
        g.setGradientFill(backlight);
        g.fillRoundedRectangle(bounds, corner);
    }

    // --- character-cell grid -------------------------------------------------
    const int rows = activeRows();
    const auto inner = bounds.reduced(bounds.getWidth() * 0.025f,
                                      bounds.getHeight() * 0.06f);
    const float pitch = juce::jmin(inner.getWidth() / (kCellDotsW * (float) kCols),
                                   inner.getHeight() / (kCellDotsH * (float) rows));
    if (pitch <= 0.0f)
        return;

    const float cellW = pitch * kCellDotsW;
    const float cellH = pitch * kCellDotsH;
    const float x0 = bounds.getCentreX() - cellW * (float) kCols * 0.5f;
    const float y0 = bounds.getCentreY() - cellH * (float) rows * 0.5f;

    // One Path per ink class keeps this a handful of fill calls (dynamic-layer
    // budget, ui-design §4).
    juce::Path normalDots, greyedDots, knockoutDots;  // knockout = glyph in `back`
    juce::Path inverseBlocks;

    auto cellRect = [&](int row, int col) {
        return juce::Rectangle<float>(x0 + (float) col * cellW, y0 + (float) row * cellH,
                                      cellW, cellH);
    };

    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < kCols; ++col)
        {
            const auto& cell = cells[(size_t) row][(size_t) col];
            const bool isCursorCell = (row == cursorRow && col == cursorCol);
            const auto r = cellRect(row, col);
            const float gx = r.getX() + pitch * 0.5f;  // half-dot side bearing
            const float gy = r.getY() + pitch;         // one-dot top lead

            // Field cursor: a blinking solid block under the glyph; the glyph
            // knocks out in lcdBack while the block is lit (period look).
            const bool blockLit =
                (isCursorCell && blinkPhase) || cell.style == Style::inverse;

            if (blockLit)
                inverseBlocks.addRoundedRectangle(r.reduced(pitch * 0.15f),
                                                  pitch * 0.4f);

            if (cell.ch == ' ' && ! blockLit)
                continue;
            if (cell.style == Style::blink && ! blinkPhase && ! isCursorCell)
                continue;

            auto& target = blockLit ? knockoutDots
                : cell.style == Style::greyed ? greyedDots
                                              : normalDots;
            lcdfont::appendGlyphPath(target, lcdfont::glyph(cell.ch), gx, gy, pitch);
        }
    }

    g.setColour(ink);
    g.fillPath(inverseBlocks);
    g.fillPath(normalDots);
    g.setColour(ink.withAlpha(kGreyedAlpha));
    g.fillPath(greyedDots);
    g.setColour(back);
    g.fillPath(knockoutDots);

    // --- subtle scanline overlay (ui-design §2, PI) ---------------------------
    {
        g.setColour(juce::Colours::black.withAlpha(0.07f));
        const float spacing = juce::jmax(2.0f, pitch);
        const float lineH = juce::jmax(0.5f, pitch * 0.25f);
        for (float y = bounds.getY() + spacing; y < bounds.getBottom() - 1.0f;
             y += spacing)
            g.fillRect(bounds.getX(), y, bounds.getWidth(), lineH);
    }

    // Top inset shadow — the glass sits behind the bezel lip.
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(),
               juce::jmax(1.0f, bounds.getHeight() * 0.025f));
}

} // namespace mws::ui
