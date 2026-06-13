// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Editor resize policy (task 047; docs/design/ui-design.md §5, architecture.md
// §6). The editor resizes from the 1000×380 base (FaceplateGeometry.h) with a
// FIXED aspect ratio (1000/380) between 0.6× and 2.0×; the scale factor is the
// width divided by the base width, persisted in the non-parameter state tree
// (uiState/scaleFactor) and restored when the editor reopens.
//
// The size↔scale math and the limit/aspect clamp are pure constexpr/inline
// functions here so they are headless-unit-testable (tests/plugin/test_resize)
// without a window; the PluginEditor wires a juce::ComponentBoundsConstrainer
// from these same constants so the live constrainer and the tests agree.

#pragma once

#include <algorithm>
#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FaceplateGeometry.h"

namespace mws::ui::resize {

/// Fixed aspect ratio (width / height) of the base canvas (ui-design §5). The
/// constrainer locks every user resize to this ratio so the faceplate never
/// distorts.
inline constexpr double kAspectRatio =
    (double) geometry::kBaseWidth / (double) geometry::kBaseHeight;

/// Resize limits as scale multiples of the base canvas (ui-design §5, scope).
inline constexpr double kMinScale = 0.6;
inline constexpr double kMaxScale = 2.0;

/// The limit rectangle dimensions the constrainer enforces (scope: min/max
/// 600×228 / 2000×760). Integer pixels — the host window size is integral.
inline constexpr int kMinWidth = (int) (geometry::kBaseWidth * kMinScale);   // 600
inline constexpr int kMinHeight = (int) (geometry::kBaseHeight * kMinScale); // 228
inline constexpr int kMaxWidth = (int) (geometry::kBaseWidth * kMaxScale);   // 2000
inline constexpr int kMaxHeight = (int) (geometry::kBaseHeight * kMaxScale); // 760

/// Width (px) for a scale factor — the canonical editor size for a given scale.
[[nodiscard]] inline int widthForScale(double scale) noexcept
{
    return (int) std::lround((double) geometry::kBaseWidth * scale);
}

/// Height (px) for a scale factor.
[[nodiscard]] inline int heightForScale(double scale) noexcept
{
    return (int) std::lround((double) geometry::kBaseHeight * scale);
}

/// The scale factor a given editor width represents (width / base width).
[[nodiscard]] inline double scaleForWidth(int width) noexcept
{
    return (double) width / (double) geometry::kBaseWidth;
}

/// Clamp a requested scale to [kMinScale, kMaxScale].
[[nodiscard]] inline double clampScale(double scale) noexcept
{
    return std::clamp(scale, kMinScale, kMaxScale);
}

/// Configures a constrainer with the fixed aspect + 0.6×–2.0× limits. The
/// PluginEditor and tests/plugin/test_resize.cpp both call this so the live
/// constrainer and the tests can never drift (constrainers are non-copyable,
/// so this mutates one in place).
inline void configureConstrainer(juce::ComponentBoundsConstrainer& c) noexcept
{
    c.setFixedAspectRatio(kAspectRatio);
    c.setSizeLimits(kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
}

} // namespace mws::ui::resize
