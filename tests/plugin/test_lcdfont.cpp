// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 040 — LCD pixel font + LcdDisplay, headless. Asserts every printable
// ASCII glyph has a code-defined pattern within the 5×7 cell, the manual-
// screen extras exist, setLine/setCell clip without assertions, the layout
// switch selects the 40×2 vs 40×6 active region, and both layout classes
// render ink pixels headlessly. Also writes the §1 mockup TIME-STRETCH page
// (and the S950 2-line page) to PNGs next to the test binary for the visual
// check — artifacts only, nothing committed (acceptance: no image assets).
//
// Test-case names begin with the tag word so `ctest -R lcdfont` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <string>

#include "ui/Faceplate.h"
#include "ui/FaceplateGeometry.h"
#include "ui/LcdDisplay.h"
#include "ui/LcdFont.h"

namespace font = mws::ui::lcdfont;
using mws::ui::LcdDisplay;
using mws::ui::LcdLayout;

namespace {

bool glyphHasInk(const font::Glyph& g)
{
    for (const auto row : g.rows)
        if (row != 0)
            return true;
    return false;
}

juce::Image renderHeadless(LcdDisplay& display, int width, int height)
{
    display.setSize(width, height);
    juce::Image image(juce::Image::ARGB, width, height, true,
                      juce::SoftwareImageType());
    juce::Graphics g(image);
    display.paintEntireComponent(g, false);
    return image;
}

/// Counts pixels close to the given colour (the ink check tolerates the
/// glow/scanline overlays shifting values slightly).
int countPixelsNear(const juce::Image& image, juce::Colour target)
{
    int count = 0;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto p = image.getPixelAt(x, y);
            if (std::abs((int) p.getRed() - (int) target.getRed()) < 40
                && std::abs((int) p.getGreen() - (int) target.getGreen()) < 40
                && std::abs((int) p.getBlue() - (int) target.getBlue()) < 40)
                ++count;
        }
    return count;
}

void savePng(const juce::Image& image, const juce::String& name)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(name);
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (out.openedOk())
    {
        juce::PNGImageFormat png;
        png.writeImageToStream(image, out);
        std::cout << "lcdfont: wrote visual-check artifact "
                  << file.getFullPathName() << "\n";
    }
}

/// The ui-design §1 mockup TIME-STRETCH page, transcribed statically
/// (content formatting proper is LcdPageModel, task 041).
void fillMockupPage(LcdDisplay& lcd)
{
    lcd.setLine(0, "TIME-STRETCH        sample: AMEN_165");
    lcd.setLine(2, "stretch zone:      0  to:  131071");
    lcd.setLine(3, "Cycle length:   1000  time factor: 300%");
    lcd.setLine(4, "stretch mode: CYCLIC qual: --  width: --");
    lcd.setLine(5, "new sample: AMEN_165*ST        mem:  7%");

    // qual/width grey out — "INTELL only" (hardware greys them in CYCLIC
    // mode [MAN §3 p.47]).
    const std::string_view row4 = "stretch mode: CYCLIC qual: --  width: --";
    for (int col = 21; col < (int) row4.size() && col < LcdDisplay::kCols; ++col)
        lcd.setCell(4, col, row4[(size_t) col], LcdDisplay::Style::greyed);

    // Field cursor on the time-factor value (nav arrives in task 045).
    lcd.setCursor(3, 35);
}

} // namespace

// ---------------------------------------------------------------------------
// Font table
// ---------------------------------------------------------------------------

TEST_CASE("lcdfont: every printable ASCII glyph has a pattern", "[lcdfont]")
{
    for (unsigned c = font::kFirstPrintable; c <= font::kLastPrintable; ++c)
    {
        INFO("code " << c << " ('" << (char) c << "')");
        REQUIRE(font::hasGlyph((unsigned char) c));
        if (c != ' ')
            REQUIRE(glyphHasInk(font::glyph((unsigned char) c)));
    }
}

TEST_CASE("lcdfont: every pattern fits the 5x7 cell", "[lcdfont]")
{
    STATIC_REQUIRE(font::kGlyphWidth == 5);
    STATIC_REQUIRE(font::kGlyphHeight == 7);

    auto checkBounds = [](unsigned char code) {
        const auto& g = font::glyph(code);
        REQUIRE(g.rows.size() == (size_t) font::kGlyphHeight);
        for (const auto row : g.rows)
        {
            INFO("code " << (int) code);
            REQUIRE(row < (1u << font::kGlyphWidth));  // bits 5–7 must be 0
        }
    };

    for (unsigned c = font::kFirstPrintable; c <= font::kLastPrintable; ++c)
        checkBounds((unsigned char) c);
    for (const auto extra : { font::kArrowRight, font::kArrowLeft, font::kArrowUp,
                              font::kArrowDown, font::kBlock })
        checkBounds(extra);
}

TEST_CASE("lcdfont: manual-screen extras exist (arrows, cursor block, %, *)",
          "[lcdfont]")
{
    for (const auto extra : { font::kArrowRight, font::kArrowLeft, font::kArrowUp,
                              font::kArrowDown, font::kBlock })
    {
        REQUIRE(font::hasGlyph(extra));
        REQUIRE(glyphHasInk(font::glyph(extra)));
    }

    // Percent and asterisk are plain ASCII — present in the main table.
    REQUIRE(glyphHasInk(font::glyph((unsigned char) '%')));
    REQUIRE(glyphHasInk(font::glyph((unsigned char) '*')));

    // The cursor block is the fully-lit cell.
    for (const auto row : font::glyph(font::kBlock).rows)
        REQUIRE(row == 0b11111u);

    // Unknown codes render blank, not garbage (and never assert).
    REQUIRE_FALSE(font::hasGlyph(0x01));
    REQUIRE_FALSE(glyphHasInk(font::glyph(0x01)));
}

TEST_CASE("lcdfont: printable glyphs are pairwise distinct", "[lcdfont]")
{
    for (unsigned a = font::kFirstPrintable; a <= font::kLastPrintable; ++a)
        for (unsigned b = a + 1; b <= font::kLastPrintable; ++b)
        {
            INFO("codes " << a << " ('" << (char) a << "') and " << b << " ('"
                          << (char) b << "')");
            REQUIRE(font::glyph((unsigned char) a).rows
                    != font::glyph((unsigned char) b).rows);
        }
}

TEST_CASE("lcdfont: glyph path renders rounded-rect pixels", "[lcdfont]")
{
    juce::Path p;
    font::appendGlyphPath(p, font::glyph((unsigned char) 'A'), 10.0f, 20.0f, 4.0f);
    REQUIRE_FALSE(p.isEmpty());

    // The path stays inside the 5×7 dot footprint at the given pitch.
    const auto bounds = p.getBounds();
    REQUIRE(bounds.getX() >= 10.0f);
    REQUIRE(bounds.getY() >= 20.0f);
    REQUIRE(bounds.getRight() <= 10.0f + 5.0f * 4.0f);
    REQUIRE(bounds.getBottom() <= 20.0f + 7.0f * 4.0f);

    // Space appends nothing.
    juce::Path blank;
    font::appendGlyphPath(blank, font::glyph((unsigned char) ' '), 0.0f, 0.0f, 4.0f);
    REQUIRE(blank.isEmpty());
}

// ---------------------------------------------------------------------------
// LcdDisplay grid contract
// ---------------------------------------------------------------------------

TEST_CASE("lcdfont: setLine clips at 40 cols without assertion", "[lcdfont]")
{
    juce::ScopedJuceInitialiser_GUI libJuce;
    LcdDisplay lcd;

    const std::string overlong(60, 'X');
    lcd.setLine(0, overlong);

    REQUIRE(lcd.cellChar(0, 0) == 'X');
    REQUIRE(lcd.cellChar(0, LcdDisplay::kCols - 1) == 'X');
    // Column 40 does not exist; the read API reports out-of-range as NUL.
    REQUIRE(lcd.cellChar(0, LcdDisplay::kCols) == '\0');

    // Short lines blank-pad to the right edge.
    lcd.setLine(1, "AB");
    REQUIRE(lcd.cellChar(1, 0) == 'A');
    REQUIRE(lcd.cellChar(1, 1) == 'B');
    REQUIRE(lcd.cellChar(1, 2) == ' ');
    REQUIRE(lcd.cellChar(1, LcdDisplay::kCols - 1) == ' ');

    // Out-of-range rows/cols are silently ignored — never an assertion.
    lcd.setLine(-1, overlong);
    lcd.setLine(99, overlong);
    lcd.setCell(0, -1, 'Q');
    lcd.setCell(0, 99, 'Q');
    lcd.setCell(99, 0, 'Q');
    lcd.setCursor(99, 99);
    REQUIRE_FALSE(lcd.hasCursor());
}

TEST_CASE("lcdfont: setLayout selects the 40x2 vs 40x6 active region", "[lcdfont]")
{
    juce::ScopedJuceInitialiser_GUI libJuce;
    LcdDisplay lcd;

    REQUIRE(lcd.layout() == LcdLayout::S1000_Page);  // canonical default
    REQUIRE(lcd.activeRows() == 6);
    REQUIRE(lcd.activeCols() == 40);

    lcd.setLayout(LcdLayout::S950_2Line);
    REQUIRE(lcd.activeRows() == 2);

    // Rows outside the 40×2 region are clipped writes.
    lcd.setLine(1, "PAGE 14  STRETCH");
    lcd.setLine(3, "SHOULD BE IGNORED");
    REQUIRE(lcd.cellChar(1, 0) == 'P');
    REQUIRE(lcd.cellChar(3, 0) == ' ');

    // setSpec adopts a model's layout class (ONE component, layout as data).
    lcd.setSpec(mws::ui::faceplateSpecFor(mws::model::ModelId::S1000));
    REQUIRE(lcd.activeRows() == 6);
    lcd.setSpec(mws::ui::faceplateSpecFor(mws::model::ModelId::S950));
    REQUIRE(lcd.activeRows() == 2);
}

TEST_CASE("lcdfont: cell styles are stored per cell", "[lcdfont]")
{
    juce::ScopedJuceInitialiser_GUI libJuce;
    LcdDisplay lcd;

    lcd.setCell(0, 0, 'N');
    lcd.setCell(0, 1, 'G', LcdDisplay::Style::greyed);
    lcd.setCell(0, 2, 'I', LcdDisplay::Style::inverse);
    lcd.setCell(0, 3, 'B', LcdDisplay::Style::blink);

    REQUIRE(lcd.cellStyle(0, 0) == LcdDisplay::Style::normal);
    REQUIRE(lcd.cellStyle(0, 1) == LcdDisplay::Style::greyed);
    REQUIRE(lcd.cellStyle(0, 2) == LcdDisplay::Style::inverse);
    REQUIRE(lcd.cellStyle(0, 3) == LcdDisplay::Style::blink);

    lcd.clear();
    REQUIRE(lcd.cellChar(0, 3) == ' ');
    REQUIRE(lcd.cellStyle(0, 3) == LcdDisplay::Style::normal);
}

// ---------------------------------------------------------------------------
// Headless render — both layout classes (visual-check artifacts)
// ---------------------------------------------------------------------------

TEST_CASE("lcdfont: S1000_PAGE layout renders the mockup page headlessly",
          "[lcdfont]")
{
    juce::ScopedJuceInitialiser_GUI libJuce;
    LcdDisplay lcd;
    lcd.setSpec(mws::ui::faceplateSpecFor(mws::model::ModelId::S1000));
    fillMockupPage(lcd);

    const auto image = renderHeadless(lcd, 546, 128);  // kLcd glass aspect

    const auto& spec = mws::ui::faceplateSpecFor(mws::model::ModelId::S1000);
    const int inkPixels = countPixelsNear(image, spec.lcdInk);
    REQUIRE(inkPixels > 500);  // glyph dots are present
    REQUIRE(countPixelsNear(image, spec.lcdBack) > 1000);  // on the LCD back

    savePng(image, "lcd_s1000_page.png");
}

TEST_CASE("lcdfont: S950_2LINE layout renders the 2-line page headlessly",
          "[lcdfont]")
{
    juce::ScopedJuceInitialiser_GUI libJuce;
    LcdDisplay lcd;
    lcd.setSpec(mws::ui::faceplateSpecFor(mws::model::ModelId::S950));
    REQUIRE(lcd.activeRows() == 2);

    // PAGE 14 STRETCH, transcription-flavored [MAN §2 p.30] (PI).
    lcd.setLine(0, "PAGE 14  STRETCH    ST.TIME= 200%");
    lcd.setLine(1, "D-TIME= 1000  AUTO-D  MON1");

    const auto image = renderHeadless(lcd, 546, 128);

    const auto& spec = mws::ui::faceplateSpecFor(mws::model::ModelId::S950);
    REQUIRE(countPixelsNear(image, spec.lcdInk) > 500);

    savePng(image, "lcd_s950_2line.png");
}

TEST_CASE("lcdfont: dynamic layer composes over the cached faceplate", "[lcdfont]")
{
    // Exactly what the Standalone shows at 1.0 scale: the cached static
    // faceplate layer with the LcdDisplay painted on the bezel glass
    // (PluginEditor::resized uses the same kLcd-reduced(7) placement).
    juce::ScopedJuceInitialiser_GUI libJuce;
    namespace geo = mws::ui::geometry;

    const auto& spec = mws::ui::faceplateSpecFor(mws::model::ModelId::S1000);
    auto image = mws::ui::renderFaceplateStaticLayer(spec, geo::kBaseWidth,
                                                     geo::kBaseHeight);

    LcdDisplay lcd;
    lcd.setSpec(spec);
    fillMockupPage(lcd);

    const auto glass = mws::ui::scaledRegion(geo::kLcd, geo::kBaseWidth,
                                             geo::kBaseHeight)
                           .reduced(7.0f)
                           .toNearestInt();
    lcd.setSize(glass.getWidth(), glass.getHeight());

    juce::Graphics g(image);
    {
        juce::Graphics::ScopedSaveState save(g);
        g.setOrigin(glass.getPosition());
        lcd.paintEntireComponent(g, false);
    }

    REQUIRE(countPixelsNear(image, spec.lcdInk) > 500);
    savePng(image, "lcd_standalone_s1000.png");
}
