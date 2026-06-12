// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// JogWheel — the endless rotary data wheel (docs/design/ui-design.md §1
// region 6, §2): drag-rotate + mouse-wheel input, velocity-sensitive deltas,
// Shift = fine mode, Up/Down keys mirror the wheel (§7). Pure vector
// (ADR-005): a concentric recessed ring + finger dimple that rotates with
// interaction, palette from SeriesLookAndFeel.
//
// The task allows "Slider subclass or Component"; this is a Component — a
// juce::Slider models an absolute bounded value, while an endless jog emits
// RELATIVE detent steps (`onDelta(steps, fine)`) with no value of its own
// (the focused LCD field owns the value, tasks 041/045). No business logic
// lives in here.
//
// All input paths (mouseDrag, mouseWheelMove, keyPressed) funnel through the
// public rotateBy()/nudge() core, so detent accumulation, fine-mode scaling
// and velocity clamping are headlessly unit-testable
// (tests/plugin/test_inputcluster.cpp).

#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace mws::ui {

class JogWheel final : public juce::Component
{
public:
    /// 30 detents per revolution (PI — period data wheels click coarsely).
    static constexpr float kRadiansPerStep =
        juce::MathConstants<float>::twoPi / 30.0f;
    /// Fine mode (Shift): this many times MORE angle per emitted step.
    static constexpr int kFineFactor = 4;
    /// Velocity multiplier ceiling for fast spins (coarse mode only).
    static constexpr float kMaxVelocityMultiplier = 8.0f;

    JogWheel();

    /// Relative detent steps; `fine` is true for Shift-precision gestures
    /// (the consumer applies its own fine increment, ui-design §2).
    std::function<void(int steps, bool fine)> onDelta;

    // --- programmatic rotation core (all input paths + tests use these) -----
    /// Feeds physical rotation in radians (positive = clockwise). Coarse
    /// steps are scaled by `velocityMultiplier` (clamped to
    /// [1, kMaxVelocityMultiplier]); fine mode ignores velocity. Whole detent
    /// steps are emitted via onDelta; the remainder carries over.
    void rotateBy(float angleRadians, bool fine, float velocityMultiplier = 1.0f);

    /// Emits exact steps (keyboard Up/Down path, §7) and turns the dimple.
    void nudge(int steps, bool fine);

    /// Current drawn dimple angle in radians (rotates with interaction).
    [[nodiscard]] float dimpleAngle() const noexcept { return visualAngle; }

    // --- Component ----------------------------------------------------------
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void paint(juce::Graphics& g) override;

private:
    [[nodiscard]] float angleOf(const juce::MouseEvent& e) const;
    void emitWholeSteps(bool fine);

    // Detent model: steps fire when the accumulated angle crosses fixed
    // detent positions (multiples of the per-step angle), so reversing
    // direction re-crosses the same boundaries — endless-encoder semantics.
    float accumAngle = 0.0f;  // accumulated effective rotation (radians)
    int lastDetent = 0;       // last detent index already emitted
    bool accumFine = false;   // mode the accumulator belongs to
    float visualAngle = 0.0f; // drawn dimple rotation (radians)

    float lastDragAngle = 0.0f;
    juce::int64 lastDragMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JogWheel)
};

} // namespace mws::ui
