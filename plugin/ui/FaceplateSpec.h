// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// FaceplateSpec — the per-model faceplate descriptor (docs/design/ui-design.md
// §3, verbatim struct + palette table; ADR-005). ONE parametric Faceplate
// component is restyled by this data: adding a model is data, not code. The
// S3000 row is present but NOT shipping at v1 (ADR-004 — reserved slot only).
//
// Clean-room rules (ADR-005): all colors are stylistic pragmatic inventions
// (PI) evoking the period hardware; wordmarks are plain text descriptors; no
// Akai logo, no photo-derived assets anywhere.
//
// Deviation note: ui-design §3 sketches the table as constexpr; juce::Colour
// is not constexpr-constructible in JUCE 8.0.13, so the table is `inline
// const` (still header-only, immutable, and ODR-safe). The hex literals below
// are the §3 palette values verbatim — asserted byte-for-byte by
// tests/plugin/test_faceplatespec.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <juce_graphics/juce_graphics.h>

#include "mws/model/ModelId.h"
#include "mws/model/ModelSpec.h"

namespace mws::ui {

/// Which hardware display class the LCD component lays out (ui-design §2):
/// both are data variants of ONE LcdDisplay component (ADR-005).
enum class LcdLayout : std::uint8_t {
    S950_2Line,  ///< 40×2 region — S900/S950 display class [MAN §2]
    S1000_Page,  ///< 40×6 page — S1000-family graphic LCD [MAN §3, §5]
};

/// Which fields the LCD page exposes for a model (ui-design §3; the per-model
/// "Applies to" column of dsp-engine.md §2). Mode-row and qual/width have
/// their own §3 flags on FaceplateSpec; this covers the remaining page fields.
struct ParamVisibility {
    bool timeFactor;     ///< false on S900 — no timestretch, ADR-003
    bool cycleLen;       ///< CYCLIC & S950 (D-TIME) [dsp-engine §2]
    bool transpose;      ///< all models [dsp-engine §2]
    bool material;       ///< MON1/POL2 — S950 only [MAN §2 p.30]
    bool bandwidth;      ///< varclock models S900/S950 [MAN §2 p.74]
    bool sampleRateSel;  ///< FS 44.1/22.05 — S1000/S1100 [MAN §3 p.81]
};

/// One struct drives everything (ui-design §3, verbatim). Geometry of
/// chassis/keys/wheel never forks per model — only palette, wordmark, LCD
/// layout class, and page content change.
struct FaceplateSpec {
    model::ModelId id;
    juce::Colour chassis, chassisEdge, legend, accent;
    juce::Colour lcdBack, lcdInk, lcdGlow;
    const char* wordmark;        ///< plain text descriptor, e.g. "S950 MIDI DIGITAL SAMPLER"
    LcdLayout lcdLayout;         ///< S950_2Line or S1000_Page
    bool showsModeRow;           ///< CYCLIC/INTELL row (false on S900/S950)
    bool showsQualWidth;         ///< greyed at v1 (INTELL deferred); false on S900/S950
    ParamVisibility visibility;  ///< which fields the LCD page exposes (dsp-engine §2)
};

namespace detail {

// §3 palette table cells that the doc names but does not assign hex values
// (chassisEdge, lcdGlow) are derived here, all (PI):
//   chassisEdge — the chassis darkened toward the bevel shadow;
//   lcdGlow     — the LCD ink at ~35% alpha for the backlight radial glow.
inline constexpr std::uint32_t kOffWhite = 0xFFE8E4D8;   // §3 "off-white"
inline constexpr std::uint32_t kDarkGrey = 0xFF2A2A2A;   // §3 "dark grey"
inline constexpr std::uint32_t kAmberInk = 0xFFFFB02E;   // §3 "amber"
inline constexpr std::uint32_t kGreenInk = 0xFF5CFF7A;   // §3 "green"
inline constexpr std::uint32_t kDeepGreen = 0xFF1A3A24;  // §3 "deep green"
inline constexpr std::uint32_t kAmberGlow = 0x59FFB02E;  // (PI) ink @ 35% alpha
inline constexpr std::uint32_t kGreenGlow = 0x595CFF7A;  // (PI) ink @ 35% alpha

} // namespace detail

/// The faceplate table, indexed by ModelId declaration order (four v1 models
/// + the reserved S3000 slot, ADR-004). Palette hex values are the ui-design
/// §3 table verbatim.
inline const std::array<FaceplateSpec, model::kModelCount> kFaceplateSpecs{ {
    // --- S900 — 1986; varispeed page + "S900: NO TIMESTRETCH" notice (ADR-003)
    {
        model::ModelId::S900,
        juce::Colour(0xFF26282B),          // chassis: near-black charcoal
        juce::Colour(0xFF17181A),          // chassisEdge (PI, derived)
        juce::Colour(detail::kOffWhite),   // legend
        juce::Colour(0xFFD84B20),          // accent: red-orange
        juce::Colour(0xFF2E3324),          // lcdBack: dark olive
        juce::Colour(detail::kAmberInk),   // lcdInk: amber
        juce::Colour(detail::kAmberGlow),  // lcdGlow (PI, derived)
        "S900 MIDI DIGITAL SAMPLER",
        LcdLayout::S950_2Line,
        false,  // showsModeRow
        false,  // showsQualWidth
        { false, false, true, false, true, false },
    },
    // --- S950 — 1988; 2-line LCD, "PAGE 14 STRETCH" layout [MAN §2]
    {
        model::ModelId::S950,
        juce::Colour(0xFF2C2E31),          // chassis: charcoal
        juce::Colour(0xFF1C1E20),          // chassisEdge (PI, derived)
        juce::Colour(detail::kOffWhite),   // legend
        juce::Colour(0xFFE09520),          // accent: amber
        juce::Colour(0xFF333826),          // lcdBack: olive
        juce::Colour(detail::kAmberInk),   // lcdInk: amber
        juce::Colour(detail::kAmberGlow),  // lcdGlow (PI, derived)
        "S950 MIDI DIGITAL SAMPLER",
        LcdLayout::S950_2Line,
        false,  // showsModeRow
        false,  // showsQualWidth
        { true, true, true, true, true, false },
    },
    // --- S1000 — the canonical grey + green look (locked decision)
    {
        model::ModelId::S1000,
        juce::Colour(0xFFB9B6AE),          // chassis: warm light grey
        juce::Colour(0xFF8E8B83),          // chassisEdge (PI, derived)
        juce::Colour(detail::kDarkGrey),   // legend
        juce::Colour(0xFF1F7A6B),          // accent: teal-green
        juce::Colour(detail::kDeepGreen),  // lcdBack: deep green
        juce::Colour(detail::kGreenInk),   // lcdInk: green
        juce::Colour(detail::kGreenGlow),  // lcdGlow (PI, derived)
        "S1000 MIDI STEREO DIGITAL SAMPLER",
        LcdLayout::S1000_Page,
        true,  // showsModeRow
        true,  // showsQualWidth (greyed at v1 — INTELL deferred)
        { true, true, true, false, false, true },
    },
    // --- S1100 — 1990; adds AES/SMPTE badge row text (selector task 044)
    {
        model::ModelId::S1100,
        juce::Colour(0xFF9FA0A0),          // chassis: mid grey
        juce::Colour(0xFF767878),          // chassisEdge (PI, derived)
        juce::Colour(detail::kDarkGrey),   // legend
        juce::Colour(0xFF27557E),          // accent: blue
        juce::Colour(detail::kDeepGreen),  // lcdBack: deep green
        juce::Colour(detail::kGreenInk),   // lcdInk: green
        juce::Colour(detail::kGreenGlow),  // lcdGlow (PI, derived)
        "S1100 MIDI STEREO DIGITAL SAMPLER",
        LcdLayout::S1000_Page,
        true,  // showsModeRow
        true,  // showsQualWidth (greyed at v1)
        { true, true, true, false, false, true },
    },
    // --- S3000 — RESERVED, v1.1 (ADR-004): slot present, NOT shipping at v1.
    {
        model::ModelId::S3000,
        juce::Colour(0xFFC4C2BC),          // chassis: light grey
        juce::Colour(0xFF99978F),          // chassisEdge (PI, derived)
        juce::Colour(detail::kDarkGrey),   // legend
        juce::Colour(0xFF5A5470),          // accent: violet-grey
        juce::Colour(detail::kDeepGreen),  // lcdBack: deep green
        juce::Colour(detail::kGreenInk),   // lcdInk: green
        juce::Colour(detail::kGreenGlow),  // lcdGlow (PI, derived)
        "S3000 MIDI STEREO DIGITAL SAMPLER",
        LcdLayout::S1000_Page,
        true,  // showsModeRow
        true,  // showsQualWidth ("qual/width defaults visible" at v1.1)
        { true, true, true, false, false, true },
    },
} };

/// Spec lookup by model id (table is in ModelId declaration order).
[[nodiscard]] inline const FaceplateSpec& faceplateSpecFor(model::ModelId id) noexcept
{
    return kFaceplateSpecs[static_cast<std::size_t>(id)];
}

/// ADR-004: the S3000 faceplate slot is reserved (not shipping) at v1.
/// Mirrors the core's single source of truth, ModelSpec::isShipping.
[[nodiscard]] inline bool isShippingFaceplate(model::ModelId id) noexcept
{
    return model::ModelSpec::isShipping(id);
}

} // namespace mws::ui
