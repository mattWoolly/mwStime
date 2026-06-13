// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// MidiVoice (task 035) — the closed-form per-note repitch math kept in one
// compiled translation unit. resolveNoteSetup runs on the AUDIO THREAD at
// note-on; it is pure, noexcept and allocation-free (architecture.md §4.2,
// dsp-engine.md §8.1). The hot read/declick loop stays inline in MidiVoice.h.

#include "MidiVoice.h"

#include <cmath>

namespace mws::plugin {

NoteSetup MidiVoice::resolveNoteSetup(int note, double bufRate, double hostRate,
                                      mws::model::ModelId model) noexcept
{
    NoteSetup setup;

    // Chromatic repitch: rate = 2^((note − 60)/12). The root is C3 = MIDI 60
    // (ui-design.md §6.3 step 3), so C3 -> 1.0 and C4 (72) -> 2.0.
    setup.rate =
        std::pow(2.0, static_cast<double>(note - kRepitchRootNote) / 12.0);

    // Read step: source frames consumed per output frame. The buffer plays at
    // its own rate vs the host (the SamplePlayer fixed-ratio bufRate/hostRate)
    // AND is repitched by the note (× rate). At matched rates and the root note
    // the step is exactly 1.0 (verbatim read).
    const double rateRatio = (hostRate > 0.0) ? (bufRate / hostRate) : 1.0;
    setup.step = rateRatio * setup.rate;

    setup.varclock = (model == mws::model::ModelId::S900
                      || model == mws::model::ModelId::S950);

    if (setup.varclock)
    {
        // Per-voice virtual clock = modelClock × rate; the reconstruction filter
        // cutoff TRACKS it (cutoff = clock / 2.5 — dsp-engine.md §8.1, DRR F4).
        // The model clock for this audition is the rendered buffer's own sample
        // rate (the render was produced at the model rate, §8.1). A note up
        // raises the clock and the cutoff; a note down lowers both (the clock-
        // tracking property — DRR F4/F7).
        const double modelClock = bufRate;
        const double voiceClock = modelClock * setup.rate;
        setup.cutoffHz = voiceClock / 2.5;
    }

    return setup;
}

} // namespace mws::plugin
