// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdFont — the bespoke 5×7 LCD pixel font (docs/design/ui-design.md §4;
// ADR-005). All glyphs are code-defined bit patterns — no font files, no
// image assets, no licensing baggage. Covers printable ASCII 32–126 plus the
// few extra glyphs the manuals' printed screens need (cursor block and the
// four cursor-key arrows; percent `%` and asterisk `*` are plain ASCII and
// live in the main table).
//
// ENCODING (the "generator" contract — what a glyph-editing agent must emit):
//   * One Glyph = kGlyphHeight (7) bytes, TOP row first.
//   * Each byte holds one 5-dot row in its LOW 5 bits; bit 4 (0b10000) is the
//     LEFTMOST column, bit 0 (0b00001) the rightmost. Bits 5–7 must be 0.
//   * Written as binary literals so the dot matrix is readable in the diff —
//     e.g. the letter T:
//         0b11111,   // #####
//         0b00100,   //   #
//         0b00100,   //   #     (and four more 0b00100 rows)
//   * Patterns are clean-room renditions in the style of period 5×7 character
//     LCD/VFD modules (PI) — drawn from the shape of the letters, not copied
//     from any ROM dump.
//
// Rendering: glyphs are drawn as rounded-rect "pixels" appended to a
// juce::Path (appendGlyphPath below) — the period dot-matrix look at any
// scale, resolution-independent (ADR-005).

#pragma once

#include <array>
#include <cstdint>

#include <juce_graphics/juce_graphics.h>

namespace mws::ui::lcdfont {

/// Dot-matrix cell of the font itself (the character cell adds inter-glyph
/// spacing on top — LcdDisplay owns that).
inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;

/// One 5×7 glyph: 7 rows, top first, low 5 bits used (bit 4 = leftmost).
struct Glyph {
    std::array<std::uint8_t, kGlyphHeight> rows;
};

inline constexpr unsigned char kFirstPrintable = 32;   // ' '
inline constexpr unsigned char kLastPrintable = 126;   // '~'

// --- non-ASCII glyph codes (manual-screen extras) ---------------------------
// Placed in otherwise-unused code points so plain C strings can embed them:
// arrows sit in the C0 control range (CP437-style positions, PI); the cursor
// block takes DEL.
inline constexpr unsigned char kArrowRight = 0x10;  ///< jog/cursor right
inline constexpr unsigned char kArrowLeft = 0x11;   ///< jog/cursor left
inline constexpr unsigned char kArrowUp = 0x1E;     ///< cursor up
inline constexpr unsigned char kArrowDown = 0x1F;   ///< cursor down
inline constexpr unsigned char kBlock = 0x7F;       ///< solid field-cursor block

namespace detail {

/// Printable ASCII 32–126, indexed [c - 32].
inline constexpr std::array<Glyph, kLastPrintable - kFirstPrintable + 1> kPrintable{ {
    { { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000 } },  // 32 ' '
    { { 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100 } },  // 33 '!'
    { { 0b01010, 0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000 } },  // 34 '"'
    { { 0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010 } },  // 35 '#'
    { { 0b00100, 0b01111, 0b10100, 0b01110, 0b00101, 0b11110, 0b00100 } },  // 36 '$'
    { { 0b11000, 0b11001, 0b00010, 0b00100, 0b01000, 0b10011, 0b00011 } },  // 37 '%'
    { { 0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101 } },  // 38 '&'
    { { 0b01100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000 } },  // 39 '\''
    { { 0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010 } },  // 40 '('
    { { 0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000 } },  // 41 ')'
    { { 0b00000, 0b00100, 0b10101, 0b01110, 0b10101, 0b00100, 0b00000 } },  // 42 '*'
    { { 0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000 } },  // 43 '+'
    { { 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000 } },  // 44 ','
    { { 0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000 } },  // 45 '-'
    { { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100 } },  // 46 '.'
    { { 0b00000, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b00000 } },  // 47 '/'
    { { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 } },  // 48 '0'
    { { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },  // 49 '1'
    { { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 } },  // 50 '2'
    { { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 } },  // 51 '3'
    { { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 } },  // 52 '4'
    { { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 } },  // 53 '5'
    { { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 } },  // 54 '6'
    { { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 } },  // 55 '7'
    { { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 } },  // 56 '8'
    { { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 } },  // 57 '9'
    { { 0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000 } },  // 58 ':'
    { { 0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b00100, 0b01000 } },  // 59 ';'
    { { 0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010 } },  // 60 '<'
    { { 0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000 } },  // 61 '='
    { { 0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000 } },  // 62 '>'
    { { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100 } },  // 63 '?'
    { { 0b01110, 0b10001, 0b00001, 0b01101, 0b10101, 0b10101, 0b01110 } },  // 64 '@'
    { { 0b01110, 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001 } },  // 65 'A'
    { { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 } },  // 66 'B'
    { { 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 } },  // 67 'C'
    { { 0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100 } },  // 68 'D'
    { { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111 } },  // 69 'E'
    { { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000 } },  // 70 'F'
    { { 0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111 } },  // 71 'G'
    { { 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },  // 72 'H'
    { { 0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },  // 73 'I'
    { { 0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100 } },  // 74 'J'
    { { 0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001 } },  // 75 'K'
    { { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 } },  // 76 'L'
    { { 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 } },  // 77 'M'
    { { 0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001 } },  // 78 'N'
    { { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 } },  // 79 'O'
    { { 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 } },  // 80 'P'
    { { 0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101 } },  // 81 'Q'
    { { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 } },  // 82 'R'
    { { 0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110 } },  // 83 'S'
    { { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 } },  // 84 'T'
    { { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 } },  // 85 'U'
    { { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 } },  // 86 'V'
    { { 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010 } },  // 87 'W'
    { { 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 } },  // 88 'X'
    { { 0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100 } },  // 89 'Y'
    { { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 } },  // 90 'Z'
    { { 0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110 } },  // 91 '['
    { { 0b00000, 0b10000, 0b01000, 0b00100, 0b00010, 0b00001, 0b00000 } },  // 92 '\\'
    { { 0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110 } },  // 93 ']'
    { { 0b00100, 0b01010, 0b10001, 0b00000, 0b00000, 0b00000, 0b00000 } },  // 94 '^'
    { { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111 } },  // 95 '_'
    { { 0b01000, 0b00100, 0b00010, 0b00000, 0b00000, 0b00000, 0b00000 } },  // 96 '`'
    { { 0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111 } },  // 97 'a'
    { { 0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b11110 } },  // 98 'b'
    { { 0b00000, 0b00000, 0b01110, 0b10000, 0b10000, 0b10001, 0b01110 } },  // 99 'c'
    { { 0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10001, 0b01111 } },  // 100 'd'
    { { 0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110 } },  // 101 'e'
    { { 0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000 } },  // 102 'f'
    { { 0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110 } },  // 103 'g'
    { { 0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001 } },  // 104 'h'
    { { 0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110 } },  // 105 'i'
    { { 0b00010, 0b00000, 0b00110, 0b00010, 0b00010, 0b10010, 0b01100 } },  // 106 'j'
    { { 0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010 } },  // 107 'k'
    { { 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },  // 108 'l'
    { { 0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10001, 0b10001 } },  // 109 'm'
    { { 0b00000, 0b00000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001 } },  // 110 'n'
    { { 0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110 } },  // 111 'o'
    { { 0b00000, 0b11110, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 } },  // 112 'p'
    { { 0b00000, 0b01101, 0b10011, 0b01111, 0b00001, 0b00001, 0b00001 } },  // 113 'q'
    { { 0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000 } },  // 114 'r'
    { { 0b00000, 0b00000, 0b01110, 0b10000, 0b01110, 0b00001, 0b11110 } },  // 115 's'
    { { 0b01000, 0b01000, 0b11100, 0b01000, 0b01000, 0b01001, 0b00110 } },  // 116 't'
    { { 0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101 } },  // 117 'u'
    { { 0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 } },  // 118 'v'
    { { 0b00000, 0b00000, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010 } },  // 119 'w'
    { { 0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001 } },  // 120 'x'
    { { 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110 } },  // 121 'y'
    { { 0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111 } },  // 122 'z'
    { { 0b00010, 0b00100, 0b00100, 0b01000, 0b00100, 0b00100, 0b00010 } },  // 123 '{'
    { { 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 } },  // 124 '|'
    { { 0b01000, 0b00100, 0b00100, 0b00010, 0b00100, 0b00100, 0b01000 } },  // 125 '}'
    { { 0b00000, 0b01000, 0b10101, 0b00010, 0b00000, 0b00000, 0b00000 } },  // 126 '~'
} };

// --- manual-screen extras ----------------------------------------------------
inline constexpr Glyph kGlyphArrowRight{ { 0b00000, 0b00100, 0b00010, 0b11111,
                                           0b00010, 0b00100, 0b00000 } };
inline constexpr Glyph kGlyphArrowLeft{ { 0b00000, 0b00100, 0b01000, 0b11111,
                                          0b01000, 0b00100, 0b00000 } };
inline constexpr Glyph kGlyphArrowUp{ { 0b00100, 0b01110, 0b10101, 0b00100,
                                        0b00100, 0b00100, 0b00000 } };
inline constexpr Glyph kGlyphArrowDown{ { 0b00000, 0b00100, 0b00100, 0b00100,
                                          0b10101, 0b01110, 0b00100 } };
inline constexpr Glyph kGlyphBlock{ { 0b11111, 0b11111, 0b11111, 0b11111,
                                      0b11111, 0b11111, 0b11111 } };

} // namespace detail

/// True when `code` has a defined pattern (printable ASCII or an extra).
[[nodiscard]] constexpr bool hasGlyph(unsigned char code) noexcept
{
    if (code >= kFirstPrintable && code <= kLastPrintable)
        return true;
    return code == kArrowRight || code == kArrowLeft || code == kArrowUp
        || code == kArrowDown || code == kBlock;
}

/// Pattern lookup. Unknown codes render as a blank cell (period modules show
/// blanks for unmapped codes — no tofu box (PI)).
[[nodiscard]] constexpr const Glyph& glyph(unsigned char code) noexcept
{
    switch (code)
    {
        case kArrowRight: return detail::kGlyphArrowRight;
        case kArrowLeft: return detail::kGlyphArrowLeft;
        case kArrowUp: return detail::kGlyphArrowUp;
        case kArrowDown: return detail::kGlyphArrowDown;
        case kBlock: return detail::kGlyphBlock;
        default: break;
    }
    if (code >= kFirstPrintable && code <= kLastPrintable)
        return detail::kPrintable[code - kFirstPrintable];
    return detail::kPrintable[0];  // blank (space)
}

/// Appends one glyph to `path` as rounded-rect "pixels" (ui-design §4).
/// `dotPitch` is the dot grid spacing; each lit dot is drawn slightly smaller
/// than the pitch so the matrix gaps read at any scale (PI: 85% fill, 30%
/// corner rounding — the period dot look).
inline void appendGlyphPath(juce::Path& path, const Glyph& g, float x, float y,
                            float dotPitch)
{
    const float dotSize = dotPitch * 0.85f;
    const float corner = dotSize * 0.30f;

    for (int row = 0; row < kGlyphHeight; ++row)
        for (int col = 0; col < kGlyphWidth; ++col)
            if ((g.rows[(size_t) row] >> (kGlyphWidth - 1 - col)) & 1u)
                path.addRoundedRectangle(x + (float) col * dotPitch,
                                         y + (float) row * dotPitch, dotSize,
                                         dotSize, corner);
}

} // namespace mws::ui::lcdfont
