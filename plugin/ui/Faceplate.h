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

class Faceplate final : public juce::Component
{
public:
    Faceplate();

    /// Swaps the FaceplateSpec and invalidates the cache (cross-fade is task 046).
    void setModel(model::ModelId id);
    [[nodiscard]] model::ModelId model() const noexcept { return modelId; }
    [[nodiscard]] const FaceplateSpec& spec() const noexcept
    {
        return faceplateSpecFor(modelId);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void rebuildCacheIfNeeded();

    model::ModelId modelId = model::ModelId::S1000;
    juce::Image cache;  // static layer, keyed on (size, model)
    model::ModelId cachedModel = model::ModelId::S1000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Faceplate)
};

} // namespace mws::ui
