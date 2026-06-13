// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// MidiVoice tests (plan/backlog/035) — the monophonic chromatic MIDI repitch
// audition voice, a real-time variable-rate read head over the published render
// buffer (architecture.md §4.2; dsp-engine.md §8.1; ui-design.md §6.3 step 3):
//   - C3 (root, note 60) plays at rate 1.0 (output matches a verbatim read of
//     the published buffer); C4 (note 72) plays at rate 2.0 (length halves; a
//     440 Hz source reads back at 880 Hz),
//   - monophonic last-note priority: an overlapping note steals the voice and
//     the steal point carries no click louder than -40 dBFS (the ~5 ms declick
//     fade),
//   - S950 (variable-clock model): a note down a fifth lowers the measured
//     tracking-filter cutoff proportionally (clock tracking — DRR F4/F7),
//   - RT-safety: the note-on / process path allocates nothing and takes no lock.
//
// Test-case names begin with "midivoice" so `ctest -R midivoice` selects them
// (README test-selection rules). The global operator-new replacement
// (TestAllocationCounter.h) is owned by test_alloc_counter.cpp in this binary.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "EngineHost.h"   // RenderedSample
#include "MidiVoice.h"

#include "TestAllocationCounter.h"

#include "mws/core/Buffer.h"
#include "mws/model/ModelId.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::model::ModelId;
using mws::plugin::MidiVoice;
using mws::plugin::RenderedSample;

constexpr double kPi = 3.14159265358979323846;

// Root note: C3 maps to MIDI 60 (rate 1.0); C4 is 72 (rate 2.0), a fifth above
// C3 is G3 = 67 (architecture.md §4.2 / ui-design.md §6.3 step 3).
constexpr int kRootNote = 60;

// A published render holding a single-channel sine at `freqHz`, `frames` long.
std::shared_ptr<const RenderedSample> makeRender(double freqHz, std::size_t frames,
                                                 double rate = 44100.0)
{
    auto rs = std::make_shared<RenderedSample>();
    rs->audio.resize(1, frames);
    rs->audio.sampleRate = rate;
    auto v = rs->audio.channel(0);
    for (std::size_t i = 0; i < frames; ++i)
        v[i] = static_cast<float>(
            std::sin(2.0 * kPi * freqHz * static_cast<double>(i) / rate));
    return rs;
}

// A published render holding a DC-ramp so a verbatim read is trivially checkable.
std::shared_ptr<const RenderedSample> makeRamp(std::size_t frames, double rate = 44100.0)
{
    auto rs = std::make_shared<RenderedSample>();
    rs->audio.resize(1, frames);
    rs->audio.sampleRate = rate;
    auto v = rs->audio.channel(0);
    for (std::size_t i = 0; i < frames; ++i)
        v[i] = static_cast<float>(i);
    return rs;
}

// Drive the voice over `total` output frames in `block`-sized chunks (mono),
// returning the concatenated output. The render/source pair and model are held
// constant for the run (the caller posts note events between calls if needed).
std::vector<float> run(MidiVoice& voice, std::size_t total, std::size_t block,
                       const std::shared_ptr<const RenderedSample>& render,
                       ModelId model, double hostRate = 44100.0)
{
    std::vector<float> out(total, 0.0f);
    std::vector<float> chan(block, 0.0f);
    const std::shared_ptr<const AudioBuffer> noSource;
    std::size_t pos = 0;
    while (pos < total)
    {
        const std::size_t len = std::min(block, total - pos);
        std::fill(chan.begin(), chan.begin() + static_cast<std::ptrdiff_t>(len), 0.0f);
        AudioView view{ chan.data(), len };
        voice.process(&view, 1, render, noSource, model, hostRate, 0.0);
        for (std::size_t i = 0; i < len; ++i)
            out[pos + i] = chan[i];
        pos += len;
    }
    return out;
}

// Dominant-frequency estimate via zero-crossing count over a steady region of a
// real sine (good to a few percent for the spot checks below).
double estimateFreq(const std::vector<float>& x, double rate)
{
    // Skip the leading fade-in region.
    const std::size_t start = std::min<std::size_t>(x.size() / 8, x.size());
    std::size_t crossings = 0;
    for (std::size_t i = start + 1; i < x.size(); ++i)
        if ((x[i - 1] <= 0.0f && x[i] > 0.0f))
            ++crossings;
    const double span = static_cast<double>(x.size() - 1 - start) / rate;
    return span > 0.0 ? static_cast<double>(crossings) / span : 0.0;
}

double peak(const std::vector<float>& x)
{
    double p = 0.0;
    for (float s : x)
        p = std::max(p, std::abs(static_cast<double>(s)));
    return p;
}
} // namespace

// note-on must be callable on the audio thread (architecture.md §4.2): the voice
// process and event posting are noexcept by contract.
static_assert(noexcept(std::declval<MidiVoice&>().noteOn(60, 1.0f)));
static_assert(noexcept(std::declval<MidiVoice&>().noteOff(60)));
static_assert(noexcept(std::declval<MidiVoice&>().allNotesOff()));

TEST_CASE("midivoice: C3 (root) plays the render verbatim at rate 1.0", "[midivoice]")
{
    constexpr std::size_t frames = 2000;
    auto render = makeRamp(frames);

    MidiVoice voice;
    voice.prepare(44100.0);
    voice.noteOn(kRootNote, 1.0f); // C3 -> rate exactly 1.0

    // Read past the short declick fade-in, then sample a steady region: at rate
    // 1.0 over a matched-rate buffer the read is verbatim, so out[i] == i once
    // the fade has reached unity.
    const auto out = run(voice, frames, 256, render, ModelId::S1000);

    // After the ~5 ms fade-in (≈220 frames @ 44.1k) the gain is unity, so the
    // verbatim ramp shows through exactly.
    const std::size_t check = 1000;
    REQUIRE_THAT(static_cast<double>(out[check]),
                 Catch::Matchers::WithinAbs(static_cast<double>(check), 1.0e-3));
    REQUIRE_THAT(static_cast<double>(out[check + 1]),
                 Catch::Matchers::WithinAbs(static_cast<double>(check + 1), 1.0e-3));
}

TEST_CASE("midivoice: C4 plays at rate 2.0 — length halves and 440 reads as 880",
          "[midivoice]")
{
    constexpr std::size_t frames = 44100; // 1 s of 440 Hz
    auto render = makeRender(440.0, frames);

    // C3 (root): plays the whole buffer back at rate 1.0 -> stays 440 Hz, runs
    // for the full second.
    {
        MidiVoice voice;
        voice.prepare(44100.0);
        voice.noteOn(kRootNote, 1.0f);
        const auto out = run(voice, frames, 512, render, ModelId::S1000);
        REQUIRE_THAT(estimateFreq(out, 44100.0),
                     Catch::Matchers::WithinRel(440.0, 0.02));
        REQUIRE(voice.isActive() == false); // ran off the end exactly at 1 s
    }

    // C4 (one octave up = note 72): rate 2.0 -> reads back at 880 Hz and runs
    // off the end of the buffer in half the time.
    {
        MidiVoice voice;
        voice.prepare(44100.0);
        voice.noteOn(kRootNote + 12, 1.0f);
        // Half the buffer's worth of OUTPUT frames consumes the whole buffer.
        const auto half = run(voice, frames / 2, 512, render, ModelId::S1000);
        REQUIRE_THAT(estimateFreq(half, 44100.0),
                     Catch::Matchers::WithinRel(880.0, 0.02));
        // By the time we've output half the frames the voice has consumed the
        // whole source and stopped.
        REQUIRE(voice.isActive() == false);
    }
}

TEST_CASE("midivoice: last-note priority steals with no click above -40 dBFS",
          "[midivoice]")
{
    constexpr std::size_t frames = 44100;
    auto render = makeRender(440.0, frames);

    const std::size_t block = 64;
    const std::shared_ptr<const AudioBuffer> noSource;

    // The "click" is a STEP DISCONTINUITY — a jump far larger than the signal's
    // own natural inter-sample slope. A 440 Hz tone repitched a fifth up plays
    // back at ~659 Hz, whose smooth waveform already steps by up to ω·A ≈ 0.094
    // per sample; an undeclicked hard steal would instead jump by up to a full
    // ±scale swing (~2.0). So we measure the steal against the new note's OWN
    // steady-state natural slope and require the steal adds no excess above
    // −40 dBFS (0.01 linear).
    auto maxNaturalSlope = [&](int note) {
        MidiVoice v;
        v.prepare(44100.0);
        v.noteOn(note, 1.0f);
        std::vector<float> c(block, 0.0f);
        // Settle past the fade-in.
        for (int b = 0; b < 30; ++b)
        {
            AudioView view{ c.data(), block };
            v.process(&view, 1, render, noSource, ModelId::S1000, 44100.0, 0.0);
        }
        double mx = 0.0;
        float prev = c[block - 1];
        for (int b = 0; b < 10; ++b)
        {
            AudioView view{ c.data(), block };
            v.process(&view, 1, render, noSource, ModelId::S1000, 44100.0, 0.0);
            for (std::size_t i = 0; i < block; ++i)
            {
                mx = std::max(mx, std::abs(static_cast<double>(c[i] - prev)));
                prev = c[i];
            }
        }
        return mx;
    };
    const double naturalSlope = maxNaturalSlope(kRootNote + 7);

    MidiVoice voice;
    voice.prepare(44100.0);
    voice.noteOn(kRootNote, 1.0f); // first note: C3

    std::vector<float> chan(block, 0.0f);
    for (int b = 0; b < 40; ++b) // ~58 ms — well past the 5 ms fade-in
    {
        AudioView view{ chan.data(), block };
        voice.process(&view, 1, render, noSource, ModelId::S1000, 44100.0, 0.0);
    }
    const float lastBeforeSteal = chan[block - 1];

    // Steal: a higher note arrives. Last-note priority -> the new note takes the
    // voice; the declick crossfade must keep the transition smooth.
    voice.noteOn(kRootNote + 7, 1.0f); // a fifth up

    double maxJump = 0.0;
    float prev = lastBeforeSteal;
    for (int b = 0; b < 20; ++b) // cover the steal transition
    {
        AudioView view{ chan.data(), block };
        voice.process(&view, 1, render, noSource, ModelId::S1000, 44100.0, 0.0);
        for (std::size_t i = 0; i < block; ++i)
        {
            maxJump = std::max(maxJump, std::abs(static_cast<double>(chan[i] - prev)));
            prev = chan[i];
        }
    }

    INFO("max steal jump = " << maxJump << ", natural slope = " << naturalSlope
                             << ", excess = " << (maxJump - naturalSlope));
    // No step discontinuity: the steal adds no jump beyond the signal's own
    // natural slope above −40 dBFS, and stays nowhere near a hard-swap click.
    REQUIRE(maxJump < naturalSlope + 0.01); // −40 dBFS excess budget
    REQUIRE(maxJump < 0.2);                 // far below a ~2.0 hard-swap click
    // The voice is still sounding (the stolen-in note plays on).
    REQUIRE(voice.isActive());
}

TEST_CASE("midivoice: S950 note down a fifth lowers the tracking cutoff "
          "proportionally (clock tracking)",
          "[midivoice]")
{
    // The S950 variable-clock chain retunes its reconstruction Butterworth per
    // note-on: cutoff = voiceClock / 2.5, voiceClock = modelClock * rate
    // (dsp-engine.md §8.1; DRR F4). A note a fifth DOWN scales the clock — and
    // therefore the cutoff — by 2^(-7/12) ≈ 0.667. We read the per-note cutoff
    // the voice computed for the active S950 note.
    constexpr std::size_t frames = 8000;
    auto render = makeRender(300.0, frames);

    MidiVoice voice;
    voice.prepare(44100.0);

    // Process one block at the root so the S950 chain is configured for note 60.
    voice.noteOn(kRootNote, 1.0f);
    {
        std::vector<float> chan(64, 0.0f);
        AudioView view{ chan.data(), chan.size() };
        const std::shared_ptr<const AudioBuffer> noSource;
        voice.process(&view, 1, render, noSource, ModelId::S950, 44100.0, 0.0);
    }
    const double cutoffRoot = voice.trackingCutoffHz();
    REQUIRE(cutoffRoot > 0.0);

    // A fifth down (G2 = 55): rate = 2^(-5/12)? No — a perfect fifth is 7
    // semitones. Note 53 is a fifth below 60.
    voice.noteOn(kRootNote - 7, 1.0f);
    {
        std::vector<float> chan(64, 0.0f);
        AudioView view{ chan.data(), chan.size() };
        const std::shared_ptr<const AudioBuffer> noSource;
        voice.process(&view, 1, render, noSource, ModelId::S950, 44100.0, 0.0);
    }
    const double cutoffFifthDown = voice.trackingCutoffHz();

    const double expectedRatio = std::pow(2.0, -7.0 / 12.0); // ≈ 0.6674
    INFO("cutoff root = " << cutoffRoot << " Hz, fifth-down = " << cutoffFifthDown
                          << " Hz, ratio = " << cutoffFifthDown / cutoffRoot);
    REQUIRE_THAT(cutoffFifthDown / cutoffRoot,
                 Catch::Matchers::WithinRel(expectedRatio, 0.01));

    // The cutoff genuinely tracks the clock: the lower note's cutoff is below
    // the root's.
    REQUIRE(cutoffFifthDown < cutoffRoot);
}

TEST_CASE("midivoice: S950 actually attenuates content above the tracking cutoff",
          "[midivoice]")
{
    // The retuned reconstruction filter must be in the signal path, not just a
    // bookkeeping number. Repitch moves the fundamental AND the cutoff together,
    // so a tone's position relative to the tracking cutoff is set by the SOURCE
    // tone vs the model clock / 2.5, independent of the note. At the root note
    // (rate 1.0, matched rates -> verbatim read) the S950 reconstruction cutoff
    // is 44100 / 2.5 = 17640 Hz; a 21 kHz source tone therefore sits above it and
    // the 6th-order Butterworth rolls it off (~-9 dB), while the fixed-rate
    // S1000 model (linear read, NO tracking filter) passes it. This pins the
    // filter into the audible path.
    constexpr std::size_t frames = 44100;
    auto render = makeRender(21000.0, frames); // above the 17640 Hz tracking cutoff

    MidiVoice s950;
    s950.prepare(44100.0);
    s950.noteOn(kRootNote, 1.0f); // root: verbatim read, cutoff = 17640 Hz
    const auto s950out = run(s950, frames, 512, render, ModelId::S950);

    MidiVoice s1000;
    s1000.prepare(44100.0);
    s1000.noteOn(kRootNote, 1.0f);
    const auto s1000out = run(s1000, frames, 512, render, ModelId::S1000);

    INFO("S950 peak = " << peak(s950out) << ", S1000 peak = " << peak(s1000out));
    // The fixed-rate model passes the tone; the S950 tracking filter rolls it off.
    REQUIRE(peak(s1000out) > 0.3);
    REQUIRE(peak(s950out) < 0.5 * peak(s1000out));
}

TEST_CASE("midivoice: note-on and process paths allocate nothing", "[midivoice]")
{
    constexpr std::size_t frames = 8000;
    auto render = makeRender(440.0, frames);

    MidiVoice voice;
    voice.prepare(44100.0); // prepare may allocate; not on the audio path

    std::vector<float> chan(256, 0.0f); // pre-allocated audio block
    const std::shared_ptr<const AudioBuffer> noSource;

    // Warm both varclock and fixed-rate code paths once before measuring so any
    // one-time lazy init (there is none, but be safe) is not counted.
    {
        AudioView view{ chan.data(), chan.size() };
        voice.noteOn(kRootNote, 1.0f);
        voice.process(&view, 1, render, noSource, ModelId::S950, 44100.0, 0.0);
        voice.process(&view, 1, render, noSource, ModelId::S1000, 44100.0, 0.0);
        voice.allNotesOff();
    }

    const auto before = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    // The hot path: a stream of note-ons (each recomputes the varclock filter
    // coefficients closed-form) and note-offs interleaved with block processing,
    // across both model families.
    for (int n = 0; n < 256; ++n)
    {
        voice.noteOn(kRootNote + (n % 25) - 12, 1.0f);
        AudioView view{ chan.data(), chan.size() };
        const ModelId model = (n & 1) ? ModelId::S950 : ModelId::S1100;
        voice.process(&view, 1, render, noSource, model, 44100.0, 0.0);
        if (n % 3 == 0)
            voice.noteOff(kRootNote + (n % 25) - 12);
    }
    voice.allNotesOff();

    const auto after = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    INFO("allocations on the note-on/process hot path = " << (after - before));
    REQUIRE(after == before);
}
