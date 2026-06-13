---
id: 040
title: LCD pixel font + LcdDisplay component (character-cell grid, glow, both layouts)
status: done
depends-on: [039]
component: ui
estimated-size: M
---

## Objective
The hero component: a bespoke 5×7 pixel font defined as bit patterns in a header, and
`LcdDisplay` rendering a character-cell grid (40×6; S950 pages use a 40×2 region) with
backlight glow and scanline overlay, colored from FaceplateSpec.

## Context
Read first:
- docs/design/ui-design.md §2 LCD row (cell grid spec, 5×7 Path glyphs, radial glow +
  scanlines, LcdPageModel-driven), §3 (lcdBack/lcdInk/lcdGlow per model; LcdLayout
  S950_2LINE vs S1000_PAGE are data variants of ONE component — ADR-005 ruling), §4
  (pixel font as bit patterns in `LcdFont.h`, rounded-rect "pixels", no font files)
- plan/decisions/005-ui-rendering-approach.md
- Faceplate/Spec (039)

## Scope
- `plugin/ui/LcdFont.h`: 5×7 bit patterns for ASCII 32–126 plus the few glyphs the
  manuals' screens need (arrow/cursor block, percent, asterisk); a generator comment
  documents the encoding; glyphs drawn as rounded-rect pixels via `juce::Path`.
- `plugin/ui/LcdDisplay.{h,cpp}`:
  - 40×6 character-cell grid (PI); `setLayout(S950_2LINE | S1000_PAGE)` selects the
    active region (40×2 for S950) — one component, layout as data,
  - API: `setCell(row, col, char, Style{normal, greyed, inverse, blink})`,
    `setLine(row, text, style)`; greyed style for the "INTELL only" fields,
  - rendering: cell pixels in lcdInk on lcdBack, radial-gradient backlight glow,
    subtle scanline overlay; dynamic layer repaints over the cached faceplate,
  - field-cursor visual (block cursor on the focused field) for task 045's
    cursor-key navigation.
- Headless test (`tests/plugin/test_lcdfont.cpp`): every printable ASCII glyph has a
  pattern; pattern width/height bounds; setLine clips at 40 cols without assertion.
- Standalone visual check: render the §1 mockup TIME-STRETCH page text statically;
  screenshot in PR.

## Out of scope
- LcdPageModel content/formatting (041), page navigation/editing (045).

## Acceptance criteria
- [ ] Font/display tests pass; both layout classes render in Standalone.
- [ ] No font files or images added; all glyphs code-defined.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R lcdfont --no-tests=error
```
