// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Faceplate region geometry (docs/design/ui-design.md §1 mockup, §3, §5).
// ONE set of constants for the 1000×380 base canvas so every child component
// (LCD 040, soft keys 042, waveform 043, selector 044, editor assembly 045)
// aligns to the same frames the Faceplate draws. Geometry NEVER forks per
// model (ui-design §3; ADR-005) — only palette/wordmark/LCD layout change.
//
// Deliberately JUCE-free (plain constexpr ints) so geometry sanity is
// headless-unit-testable; Faceplate.h provides the juce::Rectangle mapping.
// All values are stylistic pragmatic inventions (PI) matching the §1 mockup
// proportions.

#pragma once

#include <array>

namespace mws::ui::geometry {

/// Base canvas (ui-design §1, §5): rack-unit proportions, 0.6×–2.0× resize.
inline constexpr int kBaseWidth = 1000;
inline constexpr int kBaseHeight = 380;

/// A plain integer rectangle on the base canvas (JUCE-free on purpose).
struct RegionRect {
    int x, y, w, h;

    [[nodiscard]] constexpr int right() const noexcept { return x + w; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + h; }

    /// True when the OPEN interiors overlap (shared edges don't count).
    [[nodiscard]] constexpr bool intersects(const RegionRect& o) const noexcept
    {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }

    /// True when `o` lies entirely inside this rect (edges allowed).
    [[nodiscard]] constexpr bool contains(const RegionRect& o) const noexcept
    {
        return o.x >= x && o.y >= y && o.right() <= right() && o.bottom() <= bottom();
    }
};

// ---------------------------------------------------------------------------
// Region frames per the §1 mockup (S1000-family layout; same for all models).
// ---------------------------------------------------------------------------

/// The whole faceplate canvas.
inline constexpr RegionRect kCanvas{ 0, 0, kBaseWidth, kBaseHeight };

/// §1 region 1 — header strip: power LED, product name, model wordmark,
/// hamburger menu placeholder.
inline constexpr RegionRect kHeader{ 10, 6, 980, 32 };

/// §1 region 2 — the LCD bezel (hero component; contents are task 040).
inline constexpr RegionRect kLcd{ 18, 46, 560, 142 };

/// §1 region 3 — soft key bar F1–F8 (task 042).
inline constexpr RegionRect kSoftKeys{ 18, 196, 560, 50 };

/// §1 region 5 — waveform / drop zone (task 043).
inline constexpr RegionRect kWaveform{ 18, 254, 340, 86 };

/// §1 region 6 (part) — cursor/ENT key cluster under the waveform (task 042).
inline constexpr RegionRect kCursorKeys{ 18, 346, 340, 26 };

/// §1 region 6 (part) — jog wheel well (task 042).
inline constexpr RegionRect kJogWheel{ 380, 254, 118, 118 };

/// §1 region 4 — model selector + plugin-level controls panel (task 044).
inline constexpr RegionRect kModelSelector{ 640, 46, 290, 230 };

/// §1 — floppy slot flavor (drag-out render easter egg, task 036/043).
inline constexpr RegionRect kFloppySlot{ 640, 290, 290, 60 };

/// All child-region frames (excludes kCanvas), for table-driven checks and
/// frame drawing. Must stay inside kCanvas and pairwise non-overlapping —
/// asserted by tests/plugin/test_faceplatespec.cpp.
inline constexpr std::array<RegionRect, 8> kAllRegions{
    kHeader,    kLcd,      kSoftKeys,      kWaveform,
    kCursorKeys, kJogWheel, kModelSelector, kFloppySlot,
};

} // namespace mws::ui::geometry
