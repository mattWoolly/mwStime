// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// LcdFieldEditor (task 045) — the LCD field-editing controller
// (docs/design/ui-design.md §6.2 steps 1–2). It owns the LCD field cursor and
// turns hardware input gestures into parameter writes:
//   · cursor keys (SoftKeyBar onCursor) move the cursor across the editable
//     fields of the active LcdPageModel page, in the page's field-map order
//     (left-to-right / top-to-bottom, [MAN §3]); greyed fields are skipped;
//   · the jog wheel (JogWheel onDelta) edits the focused field by the
//     hardware step for its parameter — coarse one detent per step, fine
//     (Shift) the parameter's smallest increment (ui-design §2);
//   · double-click a field begins direct text entry (no numeric keypad —
//     ADR-005); commitText() parses the typed value and writes it, ENT in the
//     editor commits, Esc/blur cancels.
//
// Parameter-bound fields write the APVTS through complete gestures
// (begin/setValue/end) so host automation records cleanly; non-parameter
// fields (stretch zone, new-sample name) are surfaced through callbacks the
// PluginEditor wires to the WaveformView / state tree. The host-visible
// parameter range is the single superset (architecture.md §6) — the active
// ModelSpec still clamps at the engine and the LCD shows the clamped value.
//
// Logic lives here (not in PluginEditor) so the cursor traversal, jog steps
// and text round-trip are headlessly unit-testable against a plain APVTS
// (tests/plugin/test_editor_wiring.cpp).

#pragma once

#include <functional>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "LcdPageModel.h"
#include "SoftKeyBar.h"  // CursorDir

namespace mws::ui {

class LcdFieldEditor
{
public:
    /// `apvts` must own the task-028 layout (mws::plugin::createParameterLayout).
    /// Message thread only.
    explicit LcdFieldEditor(juce::AudioProcessorValueTreeState& apvts);

    /// Adopt the active page's field map (copies the LcdField list). Keeps the
    /// focused field on the same map index when possible, else parks it on the
    /// first editable field. In-flight text entry is preserved (the editor
    /// re-adopts the stable map on every 30 Hz poll — only an explicit cursor
    /// move / focus change cancels editing).
    void setPage(const LcdPage& page);

    // --- focus ---------------------------------------------------------------
    /// Index into the adopted field map, or -1 when no editable field exists.
    [[nodiscard]] int focusedIndex() const noexcept { return focused; }
    /// The focused field, or nullptr.
    [[nodiscard]] const LcdField* focusedField() const noexcept;
    /// Park the cursor on map index `index` if it is editable (else no-op).
    void focusField(int index);

    /// Move the cursor to the previous (Left/Up) or next (Right/Down) editable
    /// field, wrapping. No-op when there is no editable field. Cancels any
    /// in-flight text entry.
    void moveCursor(CursorDir dir);

    // --- jog editing ---------------------------------------------------------
    /// Apply a jog delta to the focused field: `steps` hardware detents,
    /// `fine` for Shift-precision. Writes the bound parameter (clamped to the
    /// superset) or fires the zone/name callback. No-op without a focused
    /// editable field, or while editing text.
    void applyJog(int steps, bool fine);

    // --- direct text entry (double-click; ADR-005, no numeric keypad) --------
    /// Begin text entry on the focused field (double-click). The seed string is
    /// currentFieldText(). No-op without a focused editable field.
    void beginTextEntry();
    [[nodiscard]] bool isEditingText() const noexcept { return editingText; }

    /// Commit the typed text to the focused field (ENT / editor return):
    /// parses it via the parameter's value-from-string (or as an integer/name
    /// for zone/name fields) and writes it. Ends text-entry mode. Ignored when
    /// not editing.
    void commitText(const juce::String& typed);
    /// Abandon text entry without writing (Esc / focus lost).
    void cancelText();

    /// The current hardware-unit text of the focused field — the seed for the
    /// text editor and the value the LCD shows. Empty without a focused field.
    [[nodiscard]] juce::String currentFieldText() const;

    // --- callbacks the PluginEditor wires --------------------------------------
    /// Fired after any edit (jog / text commit) or cursor move so the editor
    /// re-derives the page model and repaints.
    std::function<void()> onChanged;

    /// Non-parameter field edits (ui-design §6.2): the editor routes these to
    /// the WaveformView zone handles and the new-sample-name state. `delta` is
    /// the signed jog step in frames (zone) — text commits pass the absolute
    /// value through onZoneCommit / onNameCommit.
    std::function<void(LcdFieldKind which, std::int64_t deltaFrames)> onZoneJog;
    std::function<void(LcdFieldKind which, std::int64_t absoluteFrame)> onZoneCommit;
    std::function<void(const juce::String& name)> onNameCommit;

    /// Current zone values (frames) the editor supplies so currentFieldText()
    /// can render them and jog can step relative to them. Defaults to 0.
    std::function<std::int64_t(LcdFieldKind which)> zoneValueProvider;

    // --- jog step table (exposed for tests / documentation) ------------------
    struct Step {
        double coarse;  ///< hardware units per detent in coarse mode
        double fine;    ///< units per detent in fine (Shift) mode
    };
    /// The coarse/fine jog step for a bound parameter (ui-design §2; the
    /// dsp-engine.md §2 increments). Frames-per-detent for zone fields is
    /// kZoneCoarseFrames / kZoneFineFrames.
    [[nodiscard]] static Step stepFor(engine::ParamId param) noexcept;

    static constexpr std::int64_t kZoneCoarseFrames = 100;  ///< (PI) one detent
    static constexpr std::int64_t kZoneFineFrames = 1;      ///< Shift: one frame

private:
    [[nodiscard]] const char* paramIdFor(engine::ParamId param) const noexcept;
    [[nodiscard]] juce::RangedAudioParameter* parameterFor(const LcdField& f) const;
    void writeParam(juce::RangedAudioParameter& p, double newDenormValue);
    void nudgeParam(const LcdField& f, int steps, bool fine);
    [[nodiscard]] bool fieldEditable(const LcdField& f) const noexcept;

    juce::AudioProcessorValueTreeState& state;
    std::vector<LcdField> fields;
    int focused = -1;
    bool editingText = false;

    JUCE_LEAK_DETECTOR(LcdFieldEditor)
};

} // namespace mws::ui
