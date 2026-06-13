// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Faceplate — the cached vector chassis (docs/design/ui-design.md §1, §2, §4;
// ADR-005). Draws chassis, edge bevel, header strip (power LED, product name,
// model wordmark, hamburger placeholder), screws/vents flavor, and the region
// frames for LCD/softkeys/waveform/selector/jog/cursors/floppy per the §1
// mockup geometry (FaceplateGeometry.h). Static layers are rendered once per
// size/model into a juce::Image and only blitted on repaint (< 2 ms budget at
// 1.0 scale); the cache rebuilds on resize or model change only.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "FaceplateGeometry.h"
#include "FaceplateSpec.h"

namespace mws::ui {

/// Maps a base-canvas (1000×380) region onto a component of the given size,
/// so child components align with the frames the Faceplate draws.
[[nodiscard]] juce::Rectangle<float> scaledRegion(const geometry::RegionRect& region,
                                                  int componentWidth,
                                                  int componentHeight) noexcept;

/// Renders the complete static faceplate layer for a spec at the given pixel
/// size (pure vector — no Component needed, so it is headless-testable and
/// reusable by the screenshot harness later).
[[nodiscard]] juce::Image renderFaceplateStaticLayer(const FaceplateSpec& spec,
                                                     int width, int height);

class Faceplate final : public juce::Component, private juce::Timer
{
public:
    Faceplate();
    ~Faceplate() override;

    /// Palette cross-fade duration on a model switch (ui-design §6.5 (PI)).
    static constexpr int kCrossfadeMs = 150;

    /// Swaps the FaceplateSpec and invalidates the cache (instant, no fade).
    void setModel(model::ModelId id);

    /// Swaps to `id` with a kCrossfadeMs palette cross-fade (ui-design §6.5):
    /// the current cached image is captured as the fade-FROM layer and blended
    /// out over the freshly built fade-TO layer. A no-op (identical to
    /// setModel) when the model is unchanged. Driven by a private timer.
    void crossfadeToModel(model::ModelId id);

    /// True while a cross-fade is animating (tests / the editor's repaint
    /// coordination). False once the fade completes.
    [[nodiscard]] bool isCrossfading() const noexcept { return crossfading; }

    [[nodiscard]] model::ModelId model() const noexcept { return modelId; }
    [[nodiscard]] const FaceplateSpec& spec() const noexcept
    {
        return faceplateSpecFor(modelId);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void rebuildCacheIfNeeded();
    void timerCallback() override;

    model::ModelId modelId = model::ModelId::S1000;
    juce::Image cache;  // static layer, keyed on (size, model)
    model::ModelId cachedModel = model::ModelId::S1000;

    // Cross-fade state (ui-design §6.5): the previous model's cached image is
    // the fade-FROM layer; `fadeStartMs` anchors the kCrossfadeMs blend. The
    // timer repaints at the UI rate while crossfading == true.
    bool crossfading = false;
    juce::Image fadeFrom;       // the old model's static layer (kept full size)
    double fadeStartMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Faceplate)
};

} // namespace mws::ui
