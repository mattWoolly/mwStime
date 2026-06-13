// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdPageBinding (task 045) — see LcdPageBinding.h.

#include "LcdPageBinding.h"

#include <algorithm>
#include <cmath>

namespace mws::ui {

using mws::plugin::RenderOutcome;
using mws::plugin::WorkerEvent;

LcdDisplay::Style toDisplayStyle(LcdCellStyle style) noexcept
{
    switch (style)
    {
        case LcdCellStyle::Normal:  return LcdDisplay::Style::normal;
        case LcdCellStyle::Greyed:  return LcdDisplay::Style::greyed;
        case LcdCellStyle::Inverse: return LcdDisplay::Style::inverse;
        case LcdCellStyle::Blink:   return LcdDisplay::Style::blink;
    }
    return LcdDisplay::Style::normal;
}

void applyWorkerEvent(const WorkerEvent& ev, LcdRenderInfo& info) noexcept
{
    switch (ev.kind)
    {
        case WorkerEvent::Kind::Started:
            info.notEnoughMemory = false;
            info.progressPercent = 0;
            info.remainingSeconds = 0.0;
            break;

        case WorkerEvent::Kind::Progress:
        {
            const int pct = static_cast<int>(
                std::lround(std::clamp(ev.progress, 0.0f, 1.0f) * 100.0f));
            info.progressPercent = pct;
            break;
        }

        case WorkerEvent::Kind::Finished:
            info.progressPercent = -1;  // close the progress line
            info.remainingSeconds = 0.0;
            info.notEnoughMemory = (ev.outcome == RenderOutcome::NotEnoughMemory);
            break;
    }
}

void renderPage(const LcdPage& page, LcdDisplay& lcd, int cursorFieldIndex)
{
    // Layout follows the page's row count (the page model chose it from the
    // model's display class): 2 rows → S950_2Line, else the full 6-row page.
    lcd.setLayout(page.rows.size() <= static_cast<std::size_t>(LcdDisplay::kRows2Line)
                      ? LcdLayout::S950_2Line
                      : LcdLayout::S1000_Page);
    lcd.clear();

    for (std::size_t r = 0; r < page.rows.size(); ++r)
    {
        const LcdRow& row = page.rows[r];
        for (int c = 0; c < kLcdCols; ++c)
        {
            const char ch = row.text[static_cast<std::size_t>(c)];
            lcd.setCell(static_cast<int>(r), c, ch,
                        toDisplayStyle(row.styles[static_cast<std::size_t>(c)]));
        }
    }

    if (cursorFieldIndex >= 0
        && cursorFieldIndex < static_cast<int>(page.fields.size()))
    {
        const LcdField& f = page.fields[static_cast<std::size_t>(cursorFieldIndex)];
        lcd.setCursor(f.row, f.col);
    }
    else
    {
        lcd.clearCursor();
    }
}

} // namespace mws::ui
