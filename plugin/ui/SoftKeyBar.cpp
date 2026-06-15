// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "SoftKeyBar.h"

#include "FaceplateGeometry.h"
#include "lookandfeel/SeriesLookAndFeel.h"

namespace mws::ui {

namespace {

// The bar owns BOTH §1 mockup frames (soft keys + cursor cluster — the cursor
// keys are part of this component, ui-design §2). Its bounds therefore span
// the union of the two frames; these base-canvas proportions place the rows
// inside it at any scale. The band between the rows is transparent and does
// NOT intercept mouse clicks (the waveform region 043 lives underneath).
namespace geo = geometry;
constexpr float kUnionW = (float) geo::kSoftKeys.w;  // 560 (cursor row is narrower)
constexpr float kUnionH = (float) (geo::kCursorKeys.y + geo::kCursorKeys.h
                                   - geo::kSoftKeys.y);  // 176

constexpr float kSoftRowH = (float) geo::kSoftKeys.h / kUnionH;
constexpr float kCursorRowY = (float) (geo::kCursorKeys.y - geo::kSoftKeys.y) / kUnionH;
constexpr float kCursorRowH = (float) geo::kCursorKeys.h / kUnionH;
constexpr float kCursorRowW = (float) geo::kCursorKeys.w / kUnionW;

/// Fraction of the soft-key row taken by the key caps; the caption legend
/// strip sits below (mockup: `[F1] ... [F8]` over `TIME ... ABORT`).
constexpr float kCapFraction = 0.62f;

/// §1 mockup TIME-page default captions; the page model relabels at runtime.
constexpr const char* kDefaultCaptions[SoftKeyBar::kNumSoftKeys] = {
    "TIME", "autC", "ZONE", "GO", "PLAY", "A/B", "SYNC", "ABORT",
};

constexpr const char* kCursorGlyphs[5] = {
    "\xE2\x97\x84",  // ◄ Left
    "\xE2\x96\xBA",  // ► Right
    "\xE2\x96\xB2",  // ▲ Up
    "\xE2\x96\xBC",  // ▼ Down
    "ENT",
};

constexpr const char* kCursorTitles[5] = {
    "Cursor left", "Cursor right", "Cursor up", "Cursor down", "Enter",
};

} // namespace

const juce::Identifier SoftKeyBar::kModeDisabledProp{ "mwsSoftKeyModeDisabled" };

SoftKeyBar::SoftKeyBar()
{
    setWantsKeyboardFocus(true);  // arrows/Enter mirroring, ui-design §7
    // The transparent band between the two rows must not block the waveform
    // region underneath; children (the keys) still get their clicks.
    setInterceptsMouseClicks(false, true);
    setTitle("Soft key bar");

    timeSource = [] { return (juce::int64) juce::Time::getMillisecondCounter(); };

    for (int i = 0; i < kNumSoftKeys; ++i)
    {
        auto& key = keys[(size_t) i];
        key.button = std::make_unique<juce::TextButton>("F" + juce::String(i + 1));
        key.caption = kDefaultCaptions[i];

        // Mouse press/release drive the same gesture entry points the tests
        // and keyboard use (soft keys fire on press, hardware-style; hold
        // keys arm the hold gesture instead).
        auto* button = key.button.get();
        button->onStateChange = [this, i, button, wasDown = false]() mutable {
            const bool down = button->isDown();
            if (down == wasDown)
                return;
            wasDown = down;
            down ? pressKey(i) : releaseKey(i);
        };

        addAndMakeVisible(*button);
        refreshAccessibilityTitle(i);
    }

    // F8 ships as the ABORT hold key (ui-design §6.3) — still reconfigurable.
    setKeyRequiresHold(kNumSoftKeys - 1, kAbortHoldMs);

    for (int c = 0; c < 5; ++c)
    {
        auto& button = cursorKeys[(size_t) c];
        button = std::make_unique<juce::TextButton>(
            juce::String::fromUTF8(kCursorGlyphs[c]));
        button->setTitle(kCursorTitles[c]);
        button->onClick = [this, c] {
            if (c < 4)
                triggerCursor((CursorDir) c);
            else
                triggerEnter();
        };
        addAndMakeVisible(*button);
    }
}

SoftKeyBar::~SoftKeyBar() = default;

// --- runtime configuration ---------------------------------------------------

void SoftKeyBar::setKeyLabel(int index, const juce::String& caption)
{
    if (! juce::isPositiveAndBelow(index, kNumSoftKeys))
        return;

    keys[(size_t) index].caption = caption;
    refreshAccessibilityTitle(index);
    repaint();
}

juce::String SoftKeyBar::keyLabel(int index) const
{
    return juce::isPositiveAndBelow(index, kNumSoftKeys) ? keys[(size_t) index].caption
                                                         : juce::String();
}

void SoftKeyBar::setKeyEnabled(int index, bool enabled)
{
    if (! juce::isPositiveAndBelow(index, kNumSoftKeys))
        return;

    auto& key = keys[(size_t) index];
    key.modeEnabled = enabled;

    // Keep the JUCE button ENABLED so a press still reaches onStateChange ->
    // pressKey even when mode-gated (task 057): pressKey then gates the action
    // and fires onDisabledKey for the greyed-key LCD hint. A truly disabled
    // JUCE button would swallow the click and look broken. The greyed cap is
    // driven by the kModeDisabledProp the LookAndFeel reads, plus the dimmed
    // legend in paint(); the dimmed button TEXT keys off this property too.
    key.button->getProperties().set(kModeDisabledProp, ! enabled);

    if (! enabled && heldIndex == index)
        releaseKey(index);  // mode-gating a key cancels its in-flight hold
    key.button->repaint();
    repaint();
}

bool SoftKeyBar::isKeyEnabled(int index) const
{
    return juce::isPositiveAndBelow(index, kNumSoftKeys)
           && keys[(size_t) index].modeEnabled;
}

float SoftKeyBar::keyLegendAlpha(int index) const
{
    if (! juce::isPositiveAndBelow(index, kNumSoftKeys))
        return 0.0f;
    return isKeyEnabled(index) ? 1.0f : kDisabledDim;
}

void SoftKeyBar::setKeyRequiresHold(int index, int milliseconds)
{
    if (! juce::isPositiveAndBelow(index, kNumSoftKeys))
        return;

    keys[(size_t) index].holdMs = juce::jmax(0, milliseconds);
    if (heldIndex == index)
        releaseKey(index);
}

int SoftKeyBar::keyHoldMs(int index) const
{
    return juce::isPositiveAndBelow(index, kNumSoftKeys) ? keys[(size_t) index].holdMs
                                                         : 0;
}

// --- gesture entry points ------------------------------------------------------

void SoftKeyBar::pressKey(int index)
{
    if (! isKeyEnabled(index))  // disabled keys emit no ACTION (§6.4) ...
    {
        // ... but a press still gets visible feedback so the greyed key reads
        // as mode-gated, not broken (task 057). The action stays gated.
        if (juce::isPositiveAndBelow(index, kNumSoftKeys) && onDisabledKey != nullptr)
            onDisabledKey(index);
        return;
    }

    const auto& key = keys[(size_t) index];
    if (key.holdMs > 0)
    {
        // Arm the hold gesture; it fires from updateHoldProgress() only.
        heldIndex = index;
        holdStart = now();
        holdFired = false;
        progress = 0.0f;
        startTimerHz(30);
        repaint();
        return;
    }

    if (onSoftKey != nullptr)
        onSoftKey(index);
}

void SoftKeyBar::releaseKey(int index)
{
    if (heldIndex != index)
        return;

    heldIndex = -1;
    holdFired = false;
    progress = 0.0f;
    stopTimer();
    repaint();
}

void SoftKeyBar::triggerCursor(CursorDir dir)
{
    if (onCursor != nullptr)
        onCursor(dir);
}

void SoftKeyBar::triggerEnter()
{
    if (onEnter != nullptr)
        onEnter();
}

// --- hold gesture --------------------------------------------------------------

void SoftKeyBar::setTimeSource(std::function<juce::int64()> nowMs)
{
    timeSource = nowMs != nullptr
                     ? std::move(nowMs)
                     : [] { return (juce::int64) juce::Time::getMillisecondCounter(); };
}

juce::int64 SoftKeyBar::now() const
{
    return timeSource();
}

void SoftKeyBar::updateHoldProgress()
{
    if (heldIndex < 0)
        return;

    const auto holdMs = (juce::int64) keys[(size_t) heldIndex].holdMs;
    const auto elapsed = now() - holdStart;
    progress = juce::jlimit(0.0f, 1.0f, (float) elapsed / (float) juce::jmax(
                                            (juce::int64) 1, holdMs));

    // Fires exactly once per continuous press, at >= the configured hold time
    // (ui-design §6.3: 599 ms must not abort, 600 ms must).
    if (! holdFired && elapsed >= holdMs)
    {
        holdFired = true;
        if (onSoftKey != nullptr)
            onSoftKey(heldIndex);
    }

    repaint();
}

float SoftKeyBar::holdProgress(int index) const
{
    return index == heldIndex ? progress : 0.0f;
}

void SoftKeyBar::timerCallback()
{
    updateHoldProgress();
}

// --- keyboard mirroring (ui-design §7) ------------------------------------------

bool SoftKeyBar::keyPressed(const juce::KeyPress& key)
{
    if (key.isKeyCode(juce::KeyPress::leftKey))
        return triggerCursor(CursorDir::Left), true;
    if (key.isKeyCode(juce::KeyPress::rightKey))
        return triggerCursor(CursorDir::Right), true;
    if (key.isKeyCode(juce::KeyPress::upKey))
        return triggerCursor(CursorDir::Up), true;
    if (key.isKeyCode(juce::KeyPress::downKey))
        return triggerCursor(CursorDir::Down), true;
    if (key.isKeyCode(juce::KeyPress::returnKey))
        return triggerEnter(), true;

    return false;
}

// --- layout + painting -----------------------------------------------------------

juce::Rectangle<float> SoftKeyBar::softKeyRow() const
{
    const auto b = getLocalBounds().toFloat();
    return { b.getX(), b.getY(), b.getWidth(), b.getHeight() * kSoftRowH };
}

juce::Rectangle<float> SoftKeyBar::cursorRow() const
{
    const auto b = getLocalBounds().toFloat();
    return { b.getX(), b.getY() + b.getHeight() * kCursorRowY,
             b.getWidth() * kCursorRowW, b.getHeight() * kCursorRowH };
}

juce::Rectangle<float> SoftKeyBar::softKeyCell(int index) const
{
    const auto row = softKeyRow();
    const float cellW = row.getWidth() / (float) kNumSoftKeys;
    return { row.getX() + cellW * (float) index, row.getY(), cellW, row.getHeight() };
}

void SoftKeyBar::resized()
{
    const float gap = juce::jmax(2.0f, softKeyRow().getWidth() * 0.006f);

    for (int i = 0; i < kNumSoftKeys; ++i)
    {
        auto cell = softKeyCell(i);
        auto cap = cell.removeFromTop(cell.getHeight() * kCapFraction)
                       .reduced(gap, 0.0f);
        keys[(size_t) i].button->setBounds(cap.toNearestInt());
    }

    auto row = cursorRow();
    const float cellW = row.getWidth() / 5.0f;
    // Mockup order: ENT first, then ◄ ► ▲ ▼ (ui-design §1).
    const int order[5] = { 4, 0, 1, 2, 3 };
    for (int slot = 0; slot < 5; ++slot)
    {
        const juce::Rectangle<float> cell{ row.getX() + cellW * (float) slot, row.getY(),
                                           cellW, row.getHeight() };
        cursorKeys[(size_t) order[slot]]->setBounds(
            cell.reduced(gap, 0.0f).toNearestInt());
    }
}

void SoftKeyBar::paint(juce::Graphics& g)
{
    // Caption legends under the caps — printed-on-the-panel style, greyed
    // with the key (FX-mode disabled state, ui-design §6.4).
    const auto* lnf = dynamic_cast<SeriesLookAndFeel*>(&getLookAndFeel());
    const auto legend = lnf != nullptr ? lnf->spec().legend
                                       : findColour(juce::Label::textColourId);

    for (int i = 0; i < kNumSoftKeys; ++i)
    {
        auto cell = softKeyCell(i);
        cell.removeFromTop(cell.getHeight() * kCapFraction);
        const auto strip = cell.reduced(1.0f);

        g.setColour(legend.withMultipliedAlpha(keyLegendAlpha(i)));
        g.setFont(SeriesLookAndFeel::legendFont(
            juce::jmin(11.0f, strip.getHeight() * 0.85f)));
        g.drawFittedText(keys[(size_t) i].caption, strip.toNearestInt(),
                         juce::Justification::centred, 1);
    }
}

void SoftKeyBar::paintOverChildren(juce::Graphics& g)
{
    // Hold-gesture visual cue (§6.3): an accent progress bar filling along
    // the bottom edge of the held key cap.
    if (heldIndex < 0 || progress <= 0.0f)
        return;

    const auto* lnf = dynamic_cast<SeriesLookAndFeel*>(&getLookAndFeel());
    const auto accent =
        lnf != nullptr ? lnf->spec().accent : juce::Colour(0xFF1F7A6B);
    const auto legend =
        lnf != nullptr ? lnf->spec().legend : findColour(juce::Label::textColourId);

    const auto capBounds = keys[(size_t) heldIndex].button->getBounds().toFloat();

    // Holding affordance (task 057): an overlay word on the held cap so the
    // 600 ms hold reads as a deliberate gesture in progress, not a dead press —
    // "HOLD…" while filling, "ABORT" the moment it fires.
    g.setColour(legend);
    g.setFont(SeriesLookAndFeel::legendFont(
        juce::jmin(11.0f, capBounds.getHeight() * 0.4f)));
    g.drawFittedText(holdFired ? "ABORT" : "HOLD\xE2\x80\xA6",
                     capBounds.reduced(2.0f).toNearestInt(),
                     juce::Justification::centred, 1);

    auto bar = capBounds;
    const auto fill = bar.removeFromBottom(3.0f).withWidth(capBounds.getWidth() * progress);
    g.setColour(accent);
    g.fillRoundedRectangle(fill.reduced(1.0f, 0.0f), 1.0f);
}

void SoftKeyBar::refreshAccessibilityTitle(int index)
{
    auto& key = keys[(size_t) index];
    auto title = "Soft key F" + juce::String(index + 1);
    if (key.caption.isNotEmpty())
        title << ": " << key.caption;
    key.button->setTitle(title);
}

} // namespace mws::ui
