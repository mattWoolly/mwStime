// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// MidiVoice (task 035) — the SAMPLE-mode chromatic MIDI repitch audition voice.
//
// This is explicitly NOT the offline transpose stage re-used (architecture.md
// §4.2 — "~50 lines was wrong by an order of magnitude"). It is a distinct
// REAL-TIME path: a monophonic, last-note-priority voice doing a variable-rate
// read head over the PUBLISHED render buffer (the same task-030/034 copy-once
// RCU snapshot SamplePlayer reads — MidiVoice never owns or allocates a buffer).
//
//   - note-on: rate = 2^((note − 60)/12). The root is C3 = MIDI note 60
//     (ui-design.md §6.3 step 3), so C3 plays at rate 1.0 and C4 (note 72) at
//     2.0 (the buffer is consumed twice as fast; a 440 Hz source reads at 880).
//   - the read head interpolates per the model's playback paradigm
//     (dsp-engine.md §8.1 vs §8.2): 2-point LINEAR for the fixed-rate 16-bit
//     models (S1000/S1100), ZERO-ORDER HOLD for the variable-clock 12-bit models
//     (S900/S950 — "zero-order hold at the virtual clock, NO interpolation"
//     [DRR F4]).
//   - S900/S950 ONLY: the per-voice virtual clock = modelClock × rate, and the
//     6th-order tracking Butterworth reconstruction filter is RETUNED on
//     note-on, on the AUDIO THREAD, closed-form and allocation-free
//     (cutoff = voiceClock / 2.5; Butterworth6LP::setCutoff — architecture.md
//     §4.2, dsp-engine.md §8.1). Retuning preserves filter state (no reset
//     click). The filter is in the signal path, not bookkeeping.
//   - monophonic, last-note priority: a new note-on steals the voice; the steal
//     and every note-off apply a short (~5 ms) equal-power-ish linear declick
//     crossfade so no discontinuity exceeds −40 dBFS (PI).
//   - host MIDI (delivered inside processBlock on the audio thread) and UI
//     audition (PLAY soft key / waveform click — there is no on-screen keyboard
//     in the design, architecture.md §7) post into the SAME lock-free event
//     queue, drained once at the top of process(). UI wiring lands in 045b.
//
// Threading (architecture.md §4): all audio runs on the audio thread; the event
// queue is a wait-free SPSC ring of POD note events. noteOn/noteOff/allNotesOff
// only enqueue (no DSP), so they are noexcept and allocation-free and may be
// called from either the message thread (UI) or the audio thread (host MIDI) —
// a single logical producer per the 045b wiring. process() is the consumer and
// is allocation/lock/free-free on the whole path.

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "mws/core/Buffer.h"
#include "mws/core/Butterworth.h"
#include "mws/model/ModelId.h"

#include "EngineHost.h"   // RenderedSample

namespace mws::plugin {

/// The MIDI note that plays the render verbatim (rate 1.0). C3 in the S-series
/// keymap convention used by the design (ui-design.md §6.3 step 3).
inline constexpr int kRepitchRootNote = 60;

/// Default declick fade in milliseconds applied at a steal / note-off so the
/// monophonic voice never produces a hard discontinuity (PI, ~5 ms;
/// architecture.md §4.2).
inline constexpr double kDeclickMs = 5.0;

/// The closed-form per-note repitch setup computed on note-on (audio thread).
/// `MidiVoice::resolveNoteSetup` (MidiVoice.cpp) is the single authority for the
/// rate / read-step and the S900/S950 virtual-clock tracking cutoff so the
/// model-specific DSP math lives in one compiled translation unit.
struct NoteSetup
{
    double rate = 1.0;         ///< 2^((note−60)/12)
    double step = 1.0;         ///< source frames consumed per output frame
    double cutoffHz = 0.0;     ///< varclock tracking cutoff (0 for fixed-rate)
    bool varclock = false;     ///< S900/S950 (ZOH + tracking filter) vs linear
};

/// Monophonic, last-note-priority MIDI repitch voice. Reads a per-block COPY of
/// the published RenderedSample (RCU) handed in by the caller; owns no buffer,
/// allocates nothing on the audio path. See the file header for the full
/// contract.
class MidiVoice
{
public:
    MidiVoice() = default;

    MidiVoice(const MidiVoice&) = delete;
    MidiVoice& operator=(const MidiVoice&) = delete;

    // --- Producer side (message thread UI OR audio-thread host MIDI) ----------

    /// Capture the host rate (the declick-fade length depends on it) and clear
    /// the voice + event queue. Message thread; may allocate is irrelevant — it
    /// does not (no containers grow). Call before the first process().
    void prepare(double hostRate) noexcept
    {
        hostRate_ = hostRate > 0.0 ? hostRate : 44100.0;
        // Clear the queue and silence the voice deterministically.
        readIdx_.store(0, std::memory_order_relaxed);
        writeIdx_.store(0, std::memory_order_relaxed);
        active_.store(false, std::memory_order_release);
        // The fields below are only touched by the consumer; reset them too so a
        // fresh prepare() starts clean (no concurrent process() during prepare).
        gen_ = Gen{};
        steal_ = Gen{};
        stealing_ = false;
        releasing_ = false;
        fadeGain_ = 0.0;
        cutoffHz_.store(0.0, std::memory_order_relaxed);
    }

    /// Enqueue a note-on (last-note priority is resolved at drain time).
    /// `velocity` is in [0, 1]; it scales the voice gain.
    void noteOn(int note, float velocity) noexcept
    {
        push(Event{ Event::Kind::On, static_cast<std::int16_t>(note),
                    clamp01(velocity) });
    }

    /// Enqueue a note-off. Only releases the voice if `note` is the sounding
    /// note (monophonic last-note priority — a stale off for an already-stolen
    /// note is ignored at drain time).
    void noteOff(int note) noexcept
    {
        push(Event{ Event::Kind::Off, static_cast<std::int16_t>(note), 0.0f });
    }

    /// Enqueue an all-notes-off (panic / transport stop).
    void allNotesOff() noexcept
    {
        push(Event{ Event::Kind::AllOff, 0, 0.0f });
    }

    /// True iff the voice is currently sounding (consumer-updated; observable
    /// from any thread). Tests / UI feedback only.
    [[nodiscard]] bool isActive() const noexcept
    {
        return active_.load(std::memory_order_acquire);
    }

    /// The tracking-filter cutoff (Hz) the consumer computed for the currently
    /// sounding S900/S950 note; 0 for fixed-rate models or when idle. Observable
    /// from any thread (test hook for the clock-tracking property).
    [[nodiscard]] double trackingCutoffHz() const noexcept
    {
        return cutoffHz_.load(std::memory_order_acquire);
    }

    // --- Consumer side (audio thread; RT-safe: no alloc / lock / free) --------

    /// Render one block into `outputs` (numChannels views of equal length). The
    /// per-block buffer copies are handed in by the caller (RCU): `render` is the
    /// published RenderedSample (B), `source` the loaded ORIGINAL audio (A); the
    /// MIDI voice auditions the RENDER (B) by default, falling back to the source
    /// only when no render exists yet. `model` selects the playback paradigm
    /// (ZOH + tracking filter for S900/S950, linear for S1000/S1100). `outTrimDb`
    /// scales the output (dsp-engine.md §2 OUTPUT — "Applies to: all").
    ///
    /// Drains the event queue first (last-note priority), then reads. When no
    /// buffer is available, or the voice is idle and not fading, the block is
    /// silence.
    void process(mws::core::AudioView* outputs, std::size_t numChannels,
                 const std::shared_ptr<const RenderedSample>& render,
                 const std::shared_ptr<const mws::core::AudioBuffer>& source,
                 mws::model::ModelId model, double hostRate,
                 double outTrimDb) noexcept
    {
        if (hostRate > 0.0)
            hostRate_ = hostRate;

        // Active audition buffer: render (B) if present, else the source (A).
        const mws::core::AudioBuffer* buf = nullptr;
        double bufRate = hostRate_;
        if (render && render->audio.numFrames() > 0)
        {
            buf = &render->audio;
            bufRate = render->audio.sampleRate > 0.0 ? render->audio.sampleRate
                                                     : hostRate_;
        }
        else if (source && source->numFrames() > 0)
        {
            buf = source.get();
            bufRate = source->sampleRate > 0.0 ? source->sampleRate : hostRate_;
        }

        const std::size_t frames =
            numChannels > 0 ? outputs[0].size() : std::size_t{ 0 };

        // Drain queued note events (last-note priority) before reading. Needs the
        // buffer rate to set up a new note's read step and (varclock) clock.
        drainEvents(bufRate, model);

        if (buf == nullptr)
        {
            fillSilence(outputs, numChannels);
            return;
        }

        const bool varclock = isVarclock(model);
        const auto bufFrames = static_cast<std::int64_t>(buf->numFrames());
        const std::size_t bufChans = buf->numChannels();
        const double declickStep = declickStepPerSample();

        const float trim = (outTrimDb == 0.0)
                               ? 1.0f
                               : static_cast<float>(std::pow(10.0, outTrimDb / 20.0));

        for (std::size_t i = 0; i < frames; ++i)
        {
            // The crossfade: when stealing, the OUTGOING note fades out while the
            // incoming fades in; otherwise the single voice fades in on note-on
            // and out on note-off. fadeGain ∈ [0,1] applies to the new/sustaining
            // note; (1 − fadeGain) over the steal tail belongs to the old note.
            float sample = 0.0f;

            if (stealing_)
            {
                // Mix outgoing tail (steal_) fading out + incoming (gen_) fading in.
                const float fadeIn = static_cast<float>(fadeGain_);
                const float fadeOut = 1.0f - fadeIn;
                sample += fadeOut * readVoice(steal_, buf, bufFrames, bufChans,
                                              varclock, /*advance=*/true);
                sample += fadeIn * readVoice(gen_, buf, bufFrames, bufChans,
                                             varclock, /*advance=*/true);

                fadeGain_ += declickStep;
                if (fadeGain_ >= 1.0)
                {
                    fadeGain_ = 1.0;
                    stealing_ = false;
                    steal_ = Gen{}; // the old note is fully gone
                }
            }
            else if (gen_.sounding)
            {
                const float g = static_cast<float>(fadeGain_);
                sample = g * readVoice(gen_, buf, bufFrames, bufChans, varclock,
                                       /*advance=*/true);

                if (releasing_)
                {
                    fadeGain_ -= declickStep;
                    if (fadeGain_ <= 0.0)
                    {
                        fadeGain_ = 0.0;
                        gen_.sounding = false;
                        releasing_ = false;
                    }
                }
                else if (fadeGain_ < 1.0)
                {
                    fadeGain_ += declickStep;
                    if (fadeGain_ > 1.0)
                        fadeGain_ = 1.0;
                }
            }

            sample *= trim;
            for (std::size_t ch = 0; ch < numChannels; ++ch)
                outputs[ch][i] = sample;
        }

        // Auto-stop when the sounding note has run off the end of the buffer (and
        // no steal tail remains).
        if (!stealing_ && gen_.sounding
            && gen_.pos >= static_cast<double>(bufFrames))
        {
            gen_.sounding = false;
        }

        active_.store(gen_.sounding || stealing_, std::memory_order_release);
    }

private:
    // --- Event queue (wait-free SPSC ring of POD note events) -----------------

    struct Event
    {
        enum class Kind : std::uint8_t { On, Off, AllOff };
        Kind kind = Kind::On;
        std::int16_t note = 0;
        float velocity = 0.0f;
    };

    // Power-of-two capacity; one slot left empty to disambiguate full/empty.
    static constexpr std::size_t kQueueCap = 256;
    static constexpr std::size_t kQueueMask = kQueueCap - 1;

    void push(const Event& ev) noexcept
    {
        const std::size_t w = writeIdx_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & kQueueMask;
        if (next == readIdx_.load(std::memory_order_acquire))
            return; // full — drop (the voice is monophonic; a dropped event is
                    // at worst a missed note, never a crash or a leak)
        queue_[w] = ev;
        writeIdx_.store(next, std::memory_order_release);
    }

    // --- Per-note read state --------------------------------------------------

    struct Gen
    {
        bool sounding = false;
        int note = kRepitchRootNote;
        float velocity = 1.0f;
        double pos = 0.0;       ///< fractional read head in BUFFER frames
        double step = 1.0;      ///< source frames consumed per output frame
        mws::core::Butterworth6LP filter{}; ///< varclock tracking reconstruction
    };

    /// Drain all queued events on the audio thread. Last-note priority: the final
    /// note-on in the batch wins; note-offs only release if they match the
    /// sounding note (monophonic).
    void drainEvents(double bufRate, mws::model::ModelId model) noexcept
    {
        std::size_t r = readIdx_.load(std::memory_order_relaxed);
        const std::size_t w = writeIdx_.load(std::memory_order_acquire);
        while (r != w)
        {
            const Event ev = queue_[r];
            r = (r + 1) & kQueueMask;

            switch (ev.kind)
            {
                case Event::Kind::On:
                    triggerNote(ev.note, ev.velocity, bufRate, model);
                    break;
                case Event::Kind::Off:
                    if (gen_.sounding && gen_.note == ev.note && !releasing_)
                        releasing_ = true; // declick release of the held note
                    break;
                case Event::Kind::AllOff:
                    // Hard panic but still declicked: release whatever sounds.
                    if (gen_.sounding)
                        releasing_ = true;
                    break;
            }
        }
        readIdx_.store(r, std::memory_order_release);
    }

    /// Start (or steal to) a new note. Computes the rate, sets up the read head,
    /// and — for S900/S950 — recomputes the tracking Butterworth closed-form on
    /// the audio thread (no allocation). Sets up a declick crossfade from the
    /// currently sounding note, if any.
    void triggerNote(int note, float velocity, double bufRate,
                     mws::model::ModelId model) noexcept
    {
        // Single authority for the per-note repitch math (MidiVoice.cpp). Runs on
        // the audio thread, closed-form, no allocation (architecture.md §4.2).
        const NoteSetup setup = resolveNoteSetup(note, bufRate, hostRate_, model);

        // If a note is already sounding (or a previous steal is mid-fade), start
        // a declick crossfade: the current voice becomes the outgoing tail. This
        // bounds to a single tail (no unbounded chain) — for the monophonic v1 we
        // hand the current gen_ to the tail and restart the fade.
        if (gen_.sounding || stealing_)
        {
            steal_ = gen_;
            stealing_ = true;
        }
        else
        {
            stealing_ = false;
        }
        fadeGain_ = 0.0; // the incoming note always fades in from zero
        releasing_ = false;

        gen_ = Gen{};
        gen_.sounding = true;
        gen_.note = note;
        gen_.velocity = velocity;
        gen_.pos = 0.0;
        gen_.step = setup.step;

        if (setup.varclock)
        {
            // Retune the 6th-order tracking Butterworth closed-form (no alloc) and
            // start it clean for the new note (cutoff = voiceClock / 2.5; the
            // reconstruction filter runs at the audition/host rate).
            gen_.filter.setCutoff(setup.cutoffHz, hostRate_);
            gen_.filter.reset();
            cutoffHz_.store(setup.cutoffHz, std::memory_order_release);
        }
        else
        {
            cutoffHz_.store(0.0, std::memory_order_release);
        }

        active_.store(true, std::memory_order_release);
    }

    /// Closed-form per-note repitch resolver (MidiVoice.cpp): rate = 2^((note−60)
    /// /12); read step = (bufRate/hostRate) × rate; for S900/S950 the virtual
    /// clock = bufRate × rate and the tracking cutoff = clock / 2.5. Pure,
    /// noexcept, allocation-free — callable per note-on on the audio thread.
    [[nodiscard]] static NoteSetup resolveNoteSetup(int note, double bufRate,
                                                    double hostRate,
                                                    mws::model::ModelId model) noexcept;

    /// Read one output sample for a voice and advance its read head. ZOH for the
    /// varclock models (nearest sample, no interpolation — DRR F4) then through
    /// the tracking reconstruction filter; 2-point linear for the fixed-rate
    /// models. Velocity-scaled.
    float readVoice(Gen& g, const mws::core::AudioBuffer* buf,
                    std::int64_t bufFrames, std::size_t bufChans, bool varclock,
                    bool advance) noexcept
    {
        float s = 0.0f;
        if (g.pos < static_cast<double>(bufFrames))
        {
            const std::size_t ch = 0; // mono audition (S950 is a mono machine;
                                       // the render is mono-summed there). Mono
                                       // buffer feeds every output channel.
            const auto* data = bufChans > 0 ? buf->channel(ch).data() : nullptr;
            if (data != nullptr)
            {
                if (varclock)
                {
                    // Zero-order hold at the virtual clock: nearest (floor)
                    // sample, NO interpolation [DRR F4], then reconstruction.
                    const auto idx = static_cast<std::int64_t>(g.pos);
                    if (idx >= 0 && idx < bufFrames)
                    {
                        const float zoh = data[static_cast<std::size_t>(idx)];
                        s = g.filter.processSample(zoh);
                    }
                }
                else
                {
                    // 2-point linear interpolation (fixed-rate models).
                    const auto i0 = static_cast<std::int64_t>(g.pos);
                    if (i0 >= 0 && i0 < bufFrames)
                    {
                        const double frac = g.pos - static_cast<double>(i0);
                        const float a = data[static_cast<std::size_t>(i0)];
                        if (frac == 0.0)
                        {
                            s = a;
                        }
                        else
                        {
                            const std::int64_t i1 = i0 + 1;
                            const float b = (i1 < bufFrames)
                                                ? data[static_cast<std::size_t>(i1)]
                                                : a;
                            s = a + static_cast<float>(
                                        static_cast<double>(b - a) * frac);
                        }
                    }
                }
            }
        }
        s *= g.velocity;
        if (advance)
            g.pos += g.step;
        return s;
    }

    [[nodiscard]] double declickStepPerSample() const noexcept
    {
        const double fadeSamples = std::max(1.0, kDeclickMs * 0.001 * hostRate_);
        return 1.0 / fadeSamples;
    }

    [[nodiscard]] static bool isVarclock(mws::model::ModelId m) noexcept
    {
        return m == mws::model::ModelId::S900 || m == mws::model::ModelId::S950;
    }

    static void fillSilence(mws::core::AudioView* outputs,
                            std::size_t numChannels) noexcept
    {
        for (std::size_t ch = 0; ch < numChannels; ++ch)
            for (float& s : outputs[ch])
                s = 0.0f;
    }

    [[nodiscard]] static float clamp01(float v) noexcept
    {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // --- State ----------------------------------------------------------------

    double hostRate_ = 44100.0;

    // Event queue (SPSC).
    std::array<Event, kQueueCap> queue_{};
    std::atomic<std::size_t> writeIdx_{ 0 };
    std::atomic<std::size_t> readIdx_{ 0 };

    // Voice state (consumer / audio-thread owned).
    Gen gen_{};                 ///< the sounding (or fading-in) note
    Gen steal_{};               ///< the outgoing tail during a steal crossfade
    bool stealing_ = false;     ///< a steal crossfade is in progress
    bool releasing_ = false;    ///< the held note is fading out (note-off)
    double fadeGain_ = 0.0;     ///< [0,1] gain of the incoming/sounding note

    // Cross-thread observables.
    std::atomic<bool> active_{ false };
    std::atomic<double> cutoffHz_{ 0.0 };
};

} // namespace mws::plugin
