// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// SamplePlayer (task 034) — the SAMPLE-mode audio-thread playback voice.
//
// The authentic SAMPLE-mode workflow (ADR-006 SAMPLE-mode bullet; ui-design.md
// §6.3): a sample is loaded, GO renders it offline on the worker thread, and the
// result is auditioned. PLAY plays the result; A/B toggles between the ORIGINAL
// source (A) and the RENDER (B) (S2000-family audition behavior [MAN §5]).
//
// Threading (architecture.md §4):
//   - Playback runs on the AUDIO THREAD only. It holds NO owning buffer itself;
//     instead it is handed a per-block std::shared_ptr<const T> COPY (RCU,
//     task-030 publication protocol) by EngineHost::processSampleBlock, which
//     acquires the published render (B) / source (A) once per block from the
//     Published<T> slots and retires them after. SamplePlayer therefore never
//     allocates, never frees, and never touches a ValueTree.
//   - start()/stop()/setSourceMode()/setZone() are flipped from the message
//     thread via plain std::atomic; the audio thread reads them once per block.
//
// Playback rate: the rendered/original audio carries its own sample rate (the
// source rate — the render is a new sample alongside its source, §5.1). The host
// runs at `hostRate`. SamplePlayer reads at a FIXED ratio = bufferRate/hostRate
// with allocation-free 2-point linear interpolation (the LinearResampler math,
// core/Resampler.h — picked deliberately for the RT path; the offline
// SincResampler allocates and cannot run here). When bufferRate == hostRate the
// ratio is exactly 1.0 and every read is a verbatim sample (no interpolation),
// so outTrim scaling stays bit-exact (the §2 OUTPUT test).
//
// DEVIATION (documented; PR + plan/backlog/034b PI audit): "fixed-rate playback
// via the core resampler tables" — the core only ships the OFFLINE allocating
// windowed-sinc table (core/Resampler.h SincResampler), with no RT-safe
// streaming/table-driven variant (the same gap FxEngine.h documents for the FX
// character chain, plan/backlog/053). A faithful RT-safe playback read therefore
// uses LinearResampler-equivalent interpolation here; an allocation-free sinc
// playback table is follow-up engine-layer work, not plugin glue. At the common
// case (host rate == sample rate) playback is verbatim and the distinction is
// moot.
//
// outTrim applies to ALL output, not just FX (dsp-engine.md §2 OUTPUT "Applies
// to: all") — it is applied here on the SAMPLE playback output.

#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "mws/core/Buffer.h"

#include "EngineHost.h"   // RenderedSample

namespace mws::plugin {

/// Which buffer the SAMPLE-mode audition plays: the ORIGINAL loaded source (A)
/// or the offline RENDER (B). F6 A/B toggles it (ui-design.md §6.3 step 3).
enum class AuditionSource : std::uint8_t {
    Render,   ///< B — the published RenderedSample (PLAY default)
    Original, ///< A — the loaded SourceSample (the dry comparison)
};

/// The SAMPLE-mode playback voice. Audio-thread render of the per-block buffer
/// COPY handed in by EngineHost (RCU); message-thread transport flags
/// (start/stop, A/B, zone). Allocation/lock/IO-free on the audio path.
class SamplePlayer
{
public:
    SamplePlayer() = default;

    SamplePlayer(const SamplePlayer&) = delete;
    SamplePlayer& operator=(const SamplePlayer&) = delete;

    // --- Message thread ------------------------------------------------------

    /// prepareToPlay: capture the host rate (the fixed playback-ratio
    /// denominator) and reset the read head. Message thread only.
    void prepare(double hostRate) noexcept
    {
        hostRate_ = hostRate;
        readPos_.store(0.0, std::memory_order_relaxed);
        playing_.store(false, std::memory_order_release);
    }

    /// Start playback from the head of the active buffer (PLAY soft key).
    void start() noexcept
    {
        readPos_.store(0.0, std::memory_order_relaxed);
        playing_.store(true, std::memory_order_release);
    }

    /// Stop playback (idempotent). The read head is left where it stopped; the
    /// next start() rewinds.
    void stop() noexcept { playing_.store(false, std::memory_order_release); }

    /// Whether playback is active (message thread / tests).
    [[nodiscard]] bool isPlaying() const noexcept
    {
        return playing_.load(std::memory_order_acquire);
    }

    /// Select the A/B audition source (F6 A/B). Takes effect at the next block;
    /// the read head is NOT rewound (an in-place A/B comparison — the buffers
    /// share a play position scaled by length, per setSourceMode below).
    void setSourceMode(AuditionSource mode) noexcept
    {
        sourceMode_.store(mode, std::memory_order_release);
    }

    [[nodiscard]] AuditionSource sourceMode() const noexcept
    {
        return sourceMode_.load(std::memory_order_acquire);
    }

    // --- Audio thread (RT-safe: no alloc, no lock, no free) ------------------

    /// Render one block into `outputs` (numChannels views of numFrames). The
    /// per-block buffer copies are handed in by EngineHost (RCU): `render` is the
    /// published RenderedSample (B), `source` the loaded ORIGINAL audio (A);
    /// either may be empty. The active buffer is chosen by the A/B mode. When no
    /// buffer is available, or playback is stopped, the block is filled with
    /// silence (SAMPLE mode generates audio; it does not pass the host input
    /// through — architecture.md §4 "SamplePlayer::process(out) plays the
    /// rendered buffer").
    ///
    /// `outTrimDb` is applied to the playback output (dsp-engine.md §2 OUTPUT —
    /// "Applies to: all"); 0 dB short-circuits so verbatim playback stays exact.
    void process(mws::core::AudioView* outputs, std::size_t numChannels,
                 const std::shared_ptr<const RenderedSample>& render,
                 const std::shared_ptr<const mws::core::AudioBuffer>& source,
                 double outTrimDb) noexcept
    {
        // Pick the active buffer per the A/B mode. Hold a raw pointer to the
        // immutable AudioBuffer (the shared_ptr keeps it alive for the block).
        const mws::core::AudioBuffer* buf = nullptr;
        const auto mode = sourceMode_.load(std::memory_order_acquire);
        if (mode == AuditionSource::Render)
        {
            if (render && render->audio.numFrames() > 0)
                buf = &render->audio;
        }
        else
        {
            if (source && source->numFrames() > 0)
                buf = source.get();
        }

        if (buf == nullptr || !playing_.load(std::memory_order_acquire))
        {
            fillSilence(outputs, numChannels);
            return;
        }

        const auto bufFrames = static_cast<std::int64_t>(buf->numFrames());
        const auto bufChans = buf->numChannels();
        const double bufRate = buf->sampleRate > 0.0 ? buf->sampleRate : hostRate_;
        // Fixed playback ratio: source frames consumed per output frame. 1.0
        // exactly when the rates match (verbatim playback — no interpolation).
        const double ratio = (hostRate_ > 0.0) ? (bufRate / hostRate_) : 1.0;

        const float gain = (outTrimDb == 0.0)
                               ? 1.0f
                               : static_cast<float>(std::pow(10.0, outTrimDb / 20.0));

        double pos = readPos_.load(std::memory_order_relaxed);
        const std::size_t frames = numChannels > 0 ? outputs[0].size() : 0;

        for (std::size_t i = 0; i < frames; ++i)
        {
            const bool past = pos >= static_cast<double>(bufFrames);
            for (std::size_t ch = 0; ch < numChannels; ++ch)
            {
                float s = 0.0f;
                if (!past)
                {
                    // Map the output channel onto a buffer channel (mono buffer
                    // feeds every output channel; otherwise channel-for-channel,
                    // last channel reused if the buffer has fewer than outputs).
                    const std::size_t srcCh =
                        bufChans == 0 ? 0 : std::min(ch, bufChans - 1);
                    s = readInterp(buf, srcCh, pos, bufFrames) * gain;
                }
                outputs[ch][i] = s;
            }
            pos += ratio;
        }

        // Stop automatically when the read head runs off the end of the buffer.
        if (pos >= static_cast<double>(bufFrames))
            playing_.store(false, std::memory_order_release);

        readPos_.store(pos, std::memory_order_relaxed);
    }

private:
    /// 2-point linear interpolation read at a fractional buffer position
    /// (LinearResampler math, core/Resampler.h). A zero fraction short-circuits
    /// to the verbatim sample so rate-matched playback is bit-exact.
    [[nodiscard]] static float readInterp(const mws::core::AudioBuffer* buf,
                                          std::size_t ch, double pos,
                                          std::int64_t bufFrames) noexcept
    {
        const auto i0 = static_cast<std::int64_t>(pos);
        if (i0 < 0 || i0 >= bufFrames)
            return 0.0f;
        const double frac = pos - static_cast<double>(i0);
        const auto chView = buf->channel(ch);
        const float a = chView[static_cast<std::size_t>(i0)];
        if (frac == 0.0)
            return a; // verbatim — keeps rate-matched playback exact
        const std::int64_t i1 = i0 + 1;
        const float b = (i1 < bufFrames) ? chView[static_cast<std::size_t>(i1)] : a;
        return a + static_cast<float>(static_cast<double>(b - a) * frac);
    }

    static void fillSilence(mws::core::AudioView* outputs,
                            std::size_t numChannels) noexcept
    {
        for (std::size_t ch = 0; ch < numChannels; ++ch)
            for (float& s : outputs[ch])
                s = 0.0f;
    }

    double hostRate_ = 0.0;

    std::atomic<bool> playing_{ false };
    std::atomic<AuditionSource> sourceMode_{ AuditionSource::Render };

    // Fractional read head in BUFFER frames (audio-thread owned; published as a
    // plain atomic so the message thread can observe it without a data race).
    std::atomic<double> readPos_{ 0.0 };
};

} // namespace mws::plugin
