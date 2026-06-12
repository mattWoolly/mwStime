// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "JogWheel.h"

#include <cmath>

#include "lookandfeel/SeriesLookAndFeel.h"

namespace mws::ui {

namespace {

/// Drag-speed → velocity multiplier gain, in ms/radian (PI): a leisurely turn
/// stays 1:1, a flick of ~4 rad/s already doubles the step rate.
constexpr float kVelocityGainMsPerRad = 250.0f;

/// Mouse-wheel → rotation gain (PI): a deltaY of 0.1 (one typical notch) is
/// exactly one coarse detent.
constexpr float kWheelGain = JogWheel::kRadiansPerStep * 10.0f;

/// Detent-boundary epsilon: absorbs float rounding in the accumulated angle
/// so an exact N-detent rotation reliably lands on detent N.
constexpr float kDetentEpsilon = 1.0e-4f;

} // namespace

JogWheel::JogWheel()
{
    setWantsKeyboardFocus(true);  // Up/Down mirroring, ui-design §7
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTitle("Jog wheel");
    setDescription("Endless data wheel: drag or scroll to edit the focused "
                   "field; hold Shift for fine adjustment");
}

// --- rotation core ------------------------------------------------------------

void JogWheel::rotateBy(float angleRadians, bool fine, float velocityMultiplier)
{
    if (fine != accumFine)
    {
        // Mode switch starts a fresh detent grid (no cross-mode remainder).
        accumFine = fine;
        accumAngle = 0.0f;
        lastDetent = 0;
    }

    // Velocity sensitivity scales coarse spins only — fine (Shift) gestures
    // stay 1:1 for precision (ui-design §2).
    const float multiplier =
        fine ? 1.0f : juce::jlimit(1.0f, kMaxVelocityMultiplier, velocityMultiplier);

    accumAngle += angleRadians * multiplier;
    visualAngle += angleRadians;  // the dimple follows the physical gesture

    emitWholeSteps(fine);
    repaint();
}

void JogWheel::nudge(int steps, bool fine)
{
    if (steps == 0)
        return;

    visualAngle += (float) steps * kRadiansPerStep;
    if (onDelta != nullptr)
        onDelta(steps, fine);
    repaint();
}

void JogWheel::emitWholeSteps(bool fine)
{
    const float perStep = kRadiansPerStep * (fine ? (float) kFineFactor : 1.0f);
    const int detent = (int) std::floor(accumAngle / perStep + kDetentEpsilon);

    if (detent != lastDetent)
    {
        const int steps = detent - lastDetent;
        lastDetent = detent;
        if (onDelta != nullptr)
            onDelta(steps, fine);
    }
}

// --- mouse / wheel / keyboard ---------------------------------------------------

float JogWheel::angleOf(const juce::MouseEvent& e) const
{
    const auto centre = getLocalBounds().toFloat().getCentre();
    const auto pos = e.position;
    return std::atan2(pos.x - centre.x, centre.y - pos.y);  // 0 = up, cw positive
}

void JogWheel::mouseDown(const juce::MouseEvent& e)
{
    lastDragAngle = angleOf(e);
    lastDragMs = e.eventTime.toMilliseconds();
    grabKeyboardFocus();
}

void JogWheel::mouseDrag(const juce::MouseEvent& e)
{
    const float angle = angleOf(e);
    float delta = angle - lastDragAngle;

    // Endless: wrap the shortest way across the ±pi seam.
    while (delta > juce::MathConstants<float>::pi)
        delta -= juce::MathConstants<float>::twoPi;
    while (delta < -juce::MathConstants<float>::pi)
        delta += juce::MathConstants<float>::twoPi;

    const auto nowMs = e.eventTime.toMilliseconds();
    const auto dtMs = (float) juce::jmax((juce::int64) 1, nowMs - lastDragMs);
    const float speed = std::abs(delta) / dtMs;  // rad/ms
    const float velocity = 1.0f + speed * kVelocityGainMsPerRad;

    lastDragAngle = angle;
    lastDragMs = nowMs;

    rotateBy(delta, e.mods.isShiftDown(), velocity);
}

void JogWheel::mouseWheelMove(const juce::MouseEvent& e,
                              const juce::MouseWheelDetails& wheel)
{
    const float dy = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    if (dy == 0.0f)
        return;

    rotateBy(dy * kWheelGain, e.mods.isShiftDown());
}

bool JogWheel::keyPressed(const juce::KeyPress& key)
{
    const bool fine = key.getModifiers().isShiftDown();

    if (key.isKeyCode(juce::KeyPress::upKey))
        return nudge(1, fine), true;
    if (key.isKeyCode(juce::KeyPress::downKey))
        return nudge(-1, fine), true;

    return false;
}

// --- painting (pure vector: concentric ring + dimple, ADR-005) -------------------

void JogWheel::paint(juce::Graphics& g)
{
    const auto* lnf = dynamic_cast<SeriesLookAndFeel*>(&getLookAndFeel());
    const auto chassis =
        lnf != nullptr ? lnf->spec().chassis : juce::Colour(0xFFB9B6AE);
    const auto edge =
        lnf != nullptr ? lnf->spec().chassisEdge : chassis.darker(0.4f);
    const auto legend =
        lnf != nullptr ? lnf->spec().legend : juce::Colour(0xFF2A2A2A);

    auto bounds = getLocalBounds().toFloat();
    const float size = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto well = bounds.withSizeKeepingCentre(size, size).reduced(2.0f);
    const auto centre = well.getCentre();
    const float r = well.getWidth() * 0.5f;

    // Recessed well the wheel sits in.
    g.setColour(chassis.darker(0.35f));
    g.fillEllipse(well);
    g.setColour(edge);
    g.drawEllipse(well, 1.5f);

    // Wheel disc with an off-centre sheen (moulded plastic, PI).
    const float wheelR = r * 0.82f;
    const auto disc = juce::Rectangle<float>(wheelR * 2.0f, wheelR * 2.0f)
                          .withCentre(centre);
    const auto body = chassis.darker(0.55f);
    g.setGradientFill(juce::ColourGradient(
        body.brighter(0.30f), centre.x - wheelR * 0.4f, centre.y - wheelR * 0.4f,
        body.darker(0.25f), centre.x + wheelR * 0.7f, centre.y + wheelR * 0.7f, true));
    g.fillEllipse(disc);
    g.setColour(edge);
    g.drawEllipse(disc, 1.2f);

    // Concentric inner ring (the grip groove).
    const float ringR = wheelR * 0.62f;
    g.setColour(body.darker(0.4f));
    g.drawEllipse(juce::Rectangle<float>(ringR * 2.0f, ringR * 2.0f).withCentre(centre),
                  1.5f);

    // Finger dimple — rotates with interaction (visualAngle, 0 = up).
    const float dimpleR = wheelR * 0.13f;
    const float orbitR = wheelR * 0.62f;
    const juce::Point<float> dimpleCentre(
        centre.x + orbitR * std::sin(visualAngle),
        centre.y - orbitR * std::cos(visualAngle));
    const auto dimple =
        juce::Rectangle<float>(dimpleR * 2.0f, dimpleR * 2.0f).withCentre(dimpleCentre);
    g.setColour(body.darker(0.6f));
    g.fillEllipse(dimple);
    g.setColour(body.brighter(0.25f));
    g.drawEllipse(dimple, 1.0f);

    // +/- ring legends (§1 mockup flavor).
    g.setColour(legend.withAlpha(0.8f));
    g.setFont(SeriesLookAndFeel::legendFont(juce::jmax(9.0f, size * 0.09f)));
    g.drawText("+", well.withTrimmedLeft(well.getWidth() * 0.82f),
               juce::Justification::centred, false);
    g.drawText("-", well.withTrimmedRight(well.getWidth() * 0.82f),
               juce::Justification::centred, false);
}

} // namespace mws::ui
