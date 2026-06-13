// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdFieldEditor (task 045) — see LcdFieldEditor.h. Cursor traversal, jog
// stepping and direct-text-entry round-trip for the LCD field map. Parameter
// writes go through complete change gestures; zone/name fields fire callbacks.

#include "LcdFieldEditor.h"

#include <algorithm>
#include <cmath>

#include "state/Parameters.h"  // mws::plugin::paramid

namespace mws::ui {

namespace {

using engine::ParamId;
namespace pid = mws::plugin::paramid;

/// Clamp a denormalized value into a parameter's range, snapping float ints.
double clampToRange(const juce::RangedAudioParameter& p, double denorm)
{
    const auto& range = p.getNormalisableRange();
    return static_cast<double>(
        range.snapToLegalValue(static_cast<float>(denorm)));
}

} // namespace

// ---------------------------------------------------------------------------
// jog step table (ui-design §2; dsp-engine.md §2 increments)
// ---------------------------------------------------------------------------

LcdFieldEditor::Step LcdFieldEditor::stepFor(ParamId param) noexcept
{
    switch (param)
    {
        // TIME FACTOR: one percent per detent (the field shows integer percent
        // in CLASSIC); fine = the 0.01 % param increment (REVISED precision).
        case ParamId::TimeFactor: return { 1.0, 0.01 };
        // CYCLE LENGTH / D-TIME: samples; coarse 10, fine one sample.
        case ParamId::CycleLen:   return { 10.0, 1.0 };
        // TRANSPOSE: one semitone coarse, one cent (0.01 st) fine.
        case ParamId::Transpose:  return { 1.0, 0.01 };
        // BANDWIDTH: 0.1 kHz step both modes (already the param interval).
        case ParamId::Bandwidth:  return { 0.1, 0.1 };
        // Enum toggles: one choice index per detent (fine == coarse).
        case ParamId::StretchMode:
        case ParamId::Material:
        case ParamId::SampleRateSel:
        case ParamId::HopMode:
        case ParamId::Character:
        case ParamId::Norm:
        case ParamId::TempoSync:
        case ParamId::FxWindow:    return { 1.0, 1.0 };
        // OUTPUT / inert qual-width: 1 unit (qual/width are non-editable here).
        case ParamId::OutTrim:     return { 0.1, 0.1 };
        case ParamId::Qual:
        case ParamId::Width:       return { 1.0, 1.0 };
    }
    return { 1.0, 1.0 };
}

const char* LcdFieldEditor::paramIdFor(ParamId param) const noexcept
{
    switch (param)
    {
        case ParamId::TimeFactor:    return pid::timeFactor;
        case ParamId::CycleLen:      return pid::cycleLen;
        case ParamId::StretchMode:   return pid::stretchMode;
        case ParamId::HopMode:       return pid::hopMode;
        case ParamId::Transpose:     return pid::transpose;
        case ParamId::Qual:          return pid::qual;
        case ParamId::Width:         return pid::width;
        case ParamId::Material:      return pid::material;
        case ParamId::Bandwidth:     return pid::bandwidth;
        case ParamId::SampleRateSel: return pid::sampleRateSel;
        case ParamId::Character:     return pid::character;
        case ParamId::Norm:          return pid::norm;
        case ParamId::TempoSync:     return pid::tempoSync;
        case ParamId::FxWindow:      return pid::fxWindow;
        case ParamId::OutTrim:       return pid::outTrim;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

LcdFieldEditor::LcdFieldEditor(juce::AudioProcessorValueTreeState& apvts)
    : state(apvts)
{
}

bool LcdFieldEditor::fieldEditable(const LcdField& f) const noexcept
{
    return f.editable;
}

juce::RangedAudioParameter* LcdFieldEditor::parameterFor(const LcdField& f) const
{
    if (f.kind != LcdFieldKind::Param)
        return nullptr;
    const char* id = paramIdFor(f.param);
    return id != nullptr ? state.getParameter(id) : nullptr;
}

const LcdField* LcdFieldEditor::focusedField() const noexcept
{
    if (focused < 0 || focused >= static_cast<int>(fields.size()))
        return nullptr;
    return &fields[static_cast<std::size_t>(focused)];
}

void LcdFieldEditor::setPage(const LcdPage& page)
{
    // NOTE: in-flight text entry is intentionally preserved — the editor
    // re-adopts the (stable) field map on every 30 Hz poll, and that periodic
    // refresh must not abort a double-click edit. moveCursor/focusField still
    // cancel editing, which is the correct user-driven behavior.
    fields = page.fields;

    // Keep the focus on the same map slot when it stays editable; otherwise
    // fall back to the first editable field (or -1 if the page has none).
    auto firstEditable = [this]() -> int {
        for (std::size_t i = 0; i < fields.size(); ++i)
            if (fieldEditable(fields[i]))
                return static_cast<int>(i);
        return -1;
    };

    if (focused >= 0 && focused < static_cast<int>(fields.size())
        && fieldEditable(fields[static_cast<std::size_t>(focused)]))
        return;  // focus stays put

    focused = firstEditable();
}

void LcdFieldEditor::focusField(int index)
{
    if (index < 0 || index >= static_cast<int>(fields.size()))
        return;
    if (!fieldEditable(fields[static_cast<std::size_t>(index)]))
        return;
    editingText = false;
    focused = index;
    if (onChanged)
        onChanged();
}

void LcdFieldEditor::moveCursor(CursorDir dir)
{
    editingText = false;

    const int n = static_cast<int>(fields.size());
    if (n == 0)
        return;

    // Count editable fields up front so an all-greyed page is a no-op.
    int editableCount = 0;
    for (const auto& f : fields)
        editableCount += fieldEditable(f) ? 1 : 0;
    if (editableCount == 0)
    {
        focused = -1;
        return;
    }

    const bool forward = (dir == CursorDir::Right || dir == CursorDir::Down);
    const int delta = forward ? 1 : -1;

    int idx = focused < 0 ? (forward ? -1 : 0) : focused;
    for (int hops = 0; hops < n; ++hops)
    {
        idx = (idx + delta + n) % n;
        if (fieldEditable(fields[static_cast<std::size_t>(idx)]))
        {
            focused = idx;
            if (onChanged)
                onChanged();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// jog editing
// ---------------------------------------------------------------------------

void LcdFieldEditor::writeParam(juce::RangedAudioParameter& p, double newDenorm)
{
    const double clamped = clampToRange(p, newDenorm);
    p.beginChangeGesture();
    p.setValueNotifyingHost(p.convertTo0to1(static_cast<float>(clamped)));
    p.endChangeGesture();
}

void LcdFieldEditor::nudgeParam(const LcdField& f, int steps, bool fine)
{
    auto* p = parameterFor(f);
    if (p == nullptr)
        return;

    const Step step = stepFor(f.param);
    const double inc = (fine ? step.fine : step.coarse) * static_cast<double>(steps);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(p))
    {
        // Enum toggle: move the choice index by `steps`, clamped to the valid
        // range. convertTo0to1 maps the index onto the parameter's normalized
        // value (don't hand-roll the 1/(num-1) division).
        const int num = choice->choices.size();
        const int next = juce::jlimit(0, num - 1, choice->getIndex() + steps);
        choice->beginChangeGesture();
        choice->setValueNotifyingHost(choice->convertTo0to1(static_cast<float>(next)));
        choice->endChangeGesture();
        return;
    }

    const double current = static_cast<double>(
        p->convertFrom0to1(p->getValue()));
    writeParam(*p, current + inc);
}

void LcdFieldEditor::applyJog(int steps, bool fine)
{
    if (editingText || steps == 0)
        return;
    const LcdField* f = focusedField();
    if (f == nullptr || !fieldEditable(*f))
        return;

    switch (f->kind)
    {
        case LcdFieldKind::Param:
            nudgeParam(*f, steps, fine);
            break;
        case LcdFieldKind::ZoneStart:
        case LcdFieldKind::ZoneEnd:
            if (onZoneJog)
            {
                const std::int64_t frames =
                    (fine ? kZoneFineFrames : kZoneCoarseFrames)
                    * static_cast<std::int64_t>(steps);
                onZoneJog(f->kind, frames);
            }
            break;
        case LcdFieldKind::NewName:
            // The destination name has no jog semantics — double-click to type.
            return;
    }

    if (onChanged)
        onChanged();
}

// ---------------------------------------------------------------------------
// direct text entry (double-click; ADR-005, no numeric keypad)
// ---------------------------------------------------------------------------

void LcdFieldEditor::beginTextEntry()
{
    const LcdField* f = focusedField();
    if (f == nullptr || !fieldEditable(*f))
        return;
    editingText = true;
}

void LcdFieldEditor::cancelText()
{
    editingText = false;
}

void LcdFieldEditor::commitText(const juce::String& typed)
{
    if (!editingText)
        return;
    editingText = false;

    const LcdField* f = focusedField();
    if (f == nullptr)
        return;

    switch (f->kind)
    {
        case LcdFieldKind::Param:
        {
            auto* p = parameterFor(*f);
            if (p != nullptr)
            {
                // The parameter's value-from-string conversion is the single
                // parse authority (Parameters.cpp registers it per param;
                // strips the unit suffix). 0..1 normalized result; write it
                // through a complete gesture.
                const float norm = p->getValueForText(typed);
                p->beginChangeGesture();
                p->setValueNotifyingHost(norm);
                p->endChangeGesture();
            }
            break;
        }
        case LcdFieldKind::ZoneStart:
        case LcdFieldKind::ZoneEnd:
            if (onZoneCommit)
            {
                const std::int64_t frame =
                    static_cast<std::int64_t>(typed.trim().getLargeIntValue());
                onZoneCommit(f->kind, frame);
            }
            break;
        case LcdFieldKind::NewName:
            if (onNameCommit)
                onNameCommit(typed.trim());
            break;
    }

    if (onChanged)
        onChanged();
}

juce::String LcdFieldEditor::currentFieldText() const
{
    const LcdField* f = focusedField();
    if (f == nullptr)
        return {};

    switch (f->kind)
    {
        case LcdFieldKind::Param:
        {
            auto* p = parameterFor(*f);
            if (p != nullptr)
                return p->getCurrentValueAsText();
            return {};
        }
        case LcdFieldKind::ZoneStart:
        case LcdFieldKind::ZoneEnd:
        {
            const std::int64_t v = zoneValueProvider ? zoneValueProvider(f->kind) : 0;
            return juce::String(static_cast<juce::int64>(v));
        }
        case LcdFieldKind::NewName:
            return {};
    }
    return {};
}

} // namespace mws::ui
