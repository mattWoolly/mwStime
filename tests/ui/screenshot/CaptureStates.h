// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// CaptureStates — the deterministic UI golden-screenshot state set (task 047b;
// docs/design/ui-design.md §4 screenshot determinism, §5 sizing; ADR-005
// reference-platform-only gate; docs/design/testing-strategy.md §7 Wave 3
// "UI at 0.6×/2.0× scale; golden screenshots macOS arm64 only").
//
// Each state composites the real, software-rendered UI components offscreen —
// the cached vector Faceplate static layer (Faceplate.cpp renderFaceplateStaticLayer)
// with the hero LcdDisplay (040) driven by the headless LcdPageModel (041) — into
// one juce::Image. NO live PluginEditor (timers, FileLoader, animations) is built:
// the capture path is pure and deterministic by construction (fixed fake LCD data,
// no blink/cursor, fixed scale factors), which is exactly what §4 requires for a
// reproducible screenshot gate.
//
// The state list is the single source of truth shared by the capture CLI, the
// CTest registration (CMakeLists.txt), and the bless target — so the gate and the
// blessed set can never drift.

#pragma once

#include <string>
#include <vector>

#include <juce_graphics/juce_graphics.h>

namespace mws::uiscreenshot {

/// One defined capture state: a stable id (also the PNG basename + CTest name
/// suffix) and the parameters needed to reproduce it deterministically.
struct CaptureState {
    std::string id;          ///< e.g. "faceplate_s1000_time" — PNG/test basename
    std::string description; ///< human note for --list / logs
};

/// The full, ordered set of golden states (task 047b scope bullet 1):
///   · each of the four shipping model faceplates at 1.0 scale, with the S1000
///     TIME page content driven by fixed fake data for determinism;
///   · the S950 2-line page and the FX REALTIME page (LCD content variants);
///   · the canonical S1000 faceplate at 0.6× and 2.0× scale.
[[nodiscard]] const std::vector<CaptureState>& captureStates();

/// True if `id` names a known state.
[[nodiscard]] bool isKnownState(const std::string& id);

/// Render the named state into a fresh ARGB software-rendered image. Throws
/// std::runtime_error for an unknown id. Deterministic: identical inputs ⇒
/// byte-identical pixels on the same platform (no animations/blink/cursor, fixed
/// fake LCD data, fixed scale). A juce::ScopedJuceInitialiser_GUI must be live in
/// the caller (JUCE graphics + the bespoke LCD font path need the GUI subsystem).
[[nodiscard]] juce::Image renderState(const std::string& id);

} // namespace mws::uiscreenshot
