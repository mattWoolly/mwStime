// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// SoftKeyBar — the hardware input key cluster (docs/design/ui-design.md §1
// regions 3 + 6, §2): eight relabelable soft keys F1–F8 plus the cursor/ENT
// cluster (◄ ► ▲ ▼ ENT) in ONE component (the cursor keys are "part of the
// SoftKeyBar cluster", §2 table). Pure vector, styled by SeriesLookAndFeel
// (ADR-005); captions are runtime data so the active LCD page model can
// relabel keys later (041/045b) — NO business logic lives in here, only the
// callback interface (onSoftKey / onCursor / onEnter).
//
// Hold gestures (§6.3): a key configured with setKeyRequiresHold(ms) fires
// only after a CONTINUOUS press of at least `ms` (F8 ABORT ships at 600 ms
// (PI)), with an accent progress bar as the visual cue. The gesture clock is
// injectable (setTimeSource) so the 599-ms/600-ms boundary is headlessly
// unit-testable (tests/plugin/test_inputcluster.cpp).
//
// Keyboard mirroring (§7): arrow keys and Enter mirror the cursor/ENT keys
// when the bar has focus. All buttons carry accessibility titles.

#pragma once

#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

namespace mws::ui {

/// Cursor-cluster navigation direction (◄ ► ▲ ▼).
enum class CursorDir { Left, Right, Up, Down };

class SoftKeyBar final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kNumSoftKeys = 8;
    /// ui-design §6.3: hold F8 >= 600 ms (PI) to abort.
    static constexpr int kAbortHoldMs = 600;

    SoftKeyBar();
    ~SoftKeyBar() override;

    // --- callback interface (no business logic inside the component) --------
    std::function<void(int /*index 0..7*/)> onSoftKey;
    std::function<void(CursorDir)> onCursor;
    std::function<void()> onEnter;

    // --- runtime configuration (the active page model drives this, 045b) ----
    /// Sets the caption legend under soft key `index` (ignored out of range).
    void setKeyLabel(int index, const juce::String& caption);
    [[nodiscard]] juce::String keyLabel(int index) const;

    /// Greys a key and stops it emitting (FX mode greys GO/PLAY/A-B, §6.4).
    void setKeyEnabled(int index, bool enabled);
    [[nodiscard]] bool isKeyEnabled(int index) const;

    /// Configures key `index` as a hold gesture: it fires only after a
    /// continuous press >= `milliseconds` (0 restores fire-on-press).
    void setKeyRequiresHold(int index, int milliseconds);
    [[nodiscard]] int keyHoldMs(int index) const;

    // --- gesture entry points (mouse, keyboard and headless tests all funnel
    //     through these; they respect the enabled/hold configuration) --------
    void pressKey(int index);
    void releaseKey(int index);
    void triggerCursor(CursorDir dir);
    void triggerEnter();

    /// Injects the hold-gesture clock (milliseconds, monotonic). Defaults to
    /// juce::Time::getMillisecondCounter. Exists for fake-clock tests.
    void setTimeSource(std::function<juce::int64()> nowMs);

    /// Re-evaluates the active hold gesture against the time source: updates
    /// the visual progress cue and fires the key once the threshold is
    /// reached. The internal Timer calls this ~30 Hz while a hold key is
    /// down; fake-clock tests call it directly.
    void updateHoldProgress();

    /// Visual hold-progress cue for key `index`, 0..1 (0 when not held).
    [[nodiscard]] float holdProgress(int index) const;

    // --- Component ----------------------------------------------------------
    bool keyPressed(const juce::KeyPress& key) override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;

private:
    void timerCallback() override;
    [[nodiscard]] juce::int64 now() const;
    void refreshAccessibilityTitle(int index);
    [[nodiscard]] juce::Rectangle<float> softKeyCell(int index) const;
    [[nodiscard]] juce::Rectangle<float> softKeyRow() const;
    [[nodiscard]] juce::Rectangle<float> cursorRow() const;

    struct Key {
        std::unique_ptr<juce::TextButton> button;
        juce::String caption;
        int holdMs = 0;  // 0 = fire on press
    };

    std::array<Key, kNumSoftKeys> keys;

    // ◄ ► ▲ ▼ ENT — order matches CursorDir, ENT last.
    std::array<std::unique_ptr<juce::TextButton>, 5> cursorKeys;

    std::function<juce::int64()> timeSource;

    int heldIndex = -1;        // hold-gesture key currently down, else -1
    juce::int64 holdStart = 0; // time-source stamp of the press
    bool holdFired = false;    // fired once for this press already
    float progress = 0.0f;     // 0..1 visual cue for heldIndex

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoftKeyBar)
};

} // namespace mws::ui
