// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// FileLoader (task 031) — off-thread audio file decode.
//
// The dropped/chosen audio file decodes on a dedicated file-loader thread into
// an immutable SourceSample (float32, the file's ORIGINAL sample rate kept) and
// is published through the task-030 RCU protocol (Published<T>, Published.h),
// with hardware-idiom error reporting hooks (architecture.md §4 FILE LOADER
// THREAD box, §5.1 SourceSample in the data flow; ui-design.md §6.1 load flow).
//
//   - juce::AudioFormatManager registered with ONLY WAV/AIFF/FLAC (no MP3 at
//     v1 — ui-design §6.1). Decode keeps the file's sample rate; we capture the
//     name, length, channel count, and a content hash for the state's
//     sourceFile field (architecture.md §6).
//   - Runs on its own juce::Thread. Cancellation-safe: a fresh load() supersedes
//     an in-flight decode — the worker checks a per-request generation token at
//     stage boundaries and drops a superseded result (deterministically the
//     LATEST request wins, mirroring the render-worker latest-wins coalescing).
//   - Publishes std::shared_ptr<const SourceSample> via Published<SourceSample>
//     (the ONE audited publication mechanism — no new atomics; task 030 / 031
//     acceptance criterion). The audio thread acquires/retires exactly as it
//     does the render result.
//   - Typed error results (unsupported format, read failure) are posted onto a
//     lock-free UI FIFO drained on the message thread; the LCD WORDING is the
//     UI layer's job (ui-design §6.1 — e.g. `** WRONG DISK **`-flavored).
//   - Extension hook: a "sample changed" callback fired (on the loader thread)
//     after a successful publish, which task 032 uses to pre-encode the FLAC
//     state blob off the message thread (architecture.md §6).
//
// Threading discipline: decode NEVER happens on the message thread — a debug
// assert on the loader thread proves it (acceptance criterion).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h> // AudioFormatManager/Reader
#include <juce_core/juce_core.h>                    // juce::Thread, File, String

#include "mws/core/Buffer.h"

#include "Published.h"

namespace mws::plugin {

/// An immutable, published decoded source sample. Once published it is never
/// mutated; the audio thread holds a shared_ptr<const SourceSample> for the
/// duration of a block (architecture.md §4 ownership/publication protocol).
/// The audio is float32 at the file's ORIGINAL sample rate (the ingest /
/// resample-to-model-rate step happens later in OfflineRenderer — §5.1).
struct SourceSample {
    mws::core::AudioBuffer audio;     ///< decoded float32 audio (immutable once published)

    /// The original file's display name (e.g. "break.wav") — the LCD top line
    /// shows it (ui-design §6.1 step 2). UI-layer string, captured here.
    juce::String name;

    /// Absolute file path, for the state's sourceFile.path field (§6).
    juce::String path;

    /// Content hash (hex of a 64-bit hash over the file bytes) for the state's
    /// sourceFile.contentHash field (§6). Same file bytes ⇒ same hash, so a
    /// reloaded session can recognise the source without embedding it.
    juce::String contentHash;

    /// Channel count and frame length, mirrored out of `audio` for convenience
    /// (UI readouts / tests) and the file's native sample rate in Hz.
    std::uint32_t numChannels = 0;
    std::int64_t  numFrames = 0;
    double        sampleRate = 0.0;

    /// Monotone token of the load request that produced this sample — echoes
    /// load()'s return so a caller can correlate a publish with its request.
    std::uint64_t requestId = 0;
};

/// Why a load finished, posted on the UI FIFO as a typed enum (LCD strings are
/// the UI layer's job — ui-design §6.1). Mirrors the v1 failure modes.
enum class LoadOutcome : std::uint8_t {
    Loaded,            ///< published a SourceSample
    UnsupportedFormat, ///< extension/format not WAV/AIFF/FLAC (no MP3 at v1)
    ReadFailure,       ///< the file could not be opened/decoded (corrupt, empty, IO)
    Superseded,        ///< a newer load() request replaced this one in flight
};

/// One event posted from the loader thread onto the UI-feedback FIFO and drained
/// on the message thread (timer poll). POD — copied by value through the FIFO.
struct LoadEvent {
    enum class Kind : std::uint8_t { Started, Finished };

    Kind kind = Kind::Finished;
    std::uint64_t requestId = 0;

    /// Outcome for Kind::Finished (Loaded on success). Unused for Started.
    LoadOutcome outcome = LoadOutcome::Loaded;
};

/// Off-thread file loader. Owns one decode thread, the lock-free UI-feedback
/// FIFO, and the shared Published<SourceSample>.
///
/// Lifetime: construct on the message thread; startThread() before use;
/// stop() (or the destructor) joins the loader thread deterministically.
class FileLoader
{
public:
    /// UI-feedback FIFO capacity (PI): a load posts at most Started + Finished
    /// per request, and the UI polls at ~30 Hz; this is generous headroom even
    /// under a drag-storm of superseding loads.
    static constexpr int kEventCapacity = 256;

    FileLoader();
    ~FileLoader();

    FileLoader(const FileLoader&) = delete;
    FileLoader& operator=(const FileLoader&) = delete;

    // --- Message-thread API ---------------------------------------------------

    /// Request a decode of `file` (message thread). Returns the assigned request
    /// id (monotone, never 0). The loader thread wakes and decodes off-thread;
    /// the Started/Finished outcomes arrive on the UI FIFO; on success a
    /// SourceSample is published. A fresh request supersedes any in-flight one:
    /// the LATEST request's result wins deterministically (older work that is
    /// still running detects supersession at a stage boundary and posts
    /// Superseded without publishing).
    std::uint64_t load(const juce::File& file);

    /// Drain one load event from the UI FIFO (message thread, timer poll).
    /// Returns false when the FIFO is empty.
    bool popEvent(LoadEvent& out) noexcept;

    /// Drain the publication graveyard, freeing retired source buffers HERE on
    /// the message thread (timer poll) — never on the audio thread. Returns the
    /// number of buffers collected.
    std::size_t collectGarbage() noexcept { return published_.collectGarbage(); }

    /// The currently published source sample (message thread / tests).
    [[nodiscard]] std::shared_ptr<const SourceSample> currentSource() const noexcept
    {
        return published_.current();
    }

    /// Extension hook (task 032): set a callback fired AFTER a successful
    /// publish, ON THE LOADER THREAD, with the just-published sample. Task 032
    /// uses it to pre-encode the FLAC state blob off the message thread
    /// (architecture.md §6). Set once before startThread(); it must itself be
    /// thread-safe and must not touch the message thread directly.
    using SampleChangedFn = std::function<void(std::shared_ptr<const SourceSample>)>;
    void onSampleChanged(SampleChangedFn fn) { sampleChanged_ = std::move(fn); }

    /// Start / stop the loader thread. stop() joins deterministically.
    void startThread();
    void stop();

    // --- Audio-thread API (RT-safe: no alloc, no lock, no free) ---------------

    /// Audio thread: copy the published source pointer once for this block
    /// (RCU). Returns an empty pointer when nothing has been loaded yet.
    [[nodiscard]] std::shared_ptr<const SourceSample> acquireForAudioBlock() const noexcept
    {
        return published_.acquire();
    }

    /// Audio thread: return the per-block pointer to the graveyard (never frees
    /// it inline). After this returns, `ptr` is empty.
    void retireFromAudioBlock(std::shared_ptr<const SourceSample>& ptr) noexcept
    {
        (void) published_.retire(ptr);
    }

private:
    /// A queued load request. The path is captured by value so the loader thread
    /// owns its own copy and the message thread can let the juce::File go.
    struct LoadRequest {
        std::uint64_t id = 0;
        juce::File file;
    };

    /// The loader thread body: block on the wake event, take the LATEST pending
    /// request (latest wins), decode, publish on success, post the outcome.
    class Worker final : public juce::Thread
    {
    public:
        explicit Worker(FileLoader& owner)
            : juce::Thread("mwStime file loader"), owner_(owner) {}
        void run() override;

        [[nodiscard]] bool shouldExit() const noexcept { return threadShouldExit(); }

    private:
        FileLoader& owner_;
    };

    /// Loader-side: decode one request end to end. Returns the outcome and, on
    /// success, fills `out`.
    LoadOutcome decode(const LoadRequest& req, std::shared_ptr<SourceSample>& out);

    /// Loader-side: true iff a request newer than `id` has arrived (supersession).
    [[nodiscard]] bool superseded(std::uint64_t id) const noexcept
    {
        return latestRequestId_.load(std::memory_order_acquire) != id;
    }

    /// Loader-side: push an event onto the UI FIFO; retries terminal Finished
    /// events so the UI never misses a load's outcome. Returns whether accepted.
    bool pushEvent(const LoadEvent& ev) noexcept;
    void pushFinished(std::uint64_t requestId, LoadOutcome outcome) noexcept;

    [[nodiscard]] bool workerShouldExit() const noexcept { return worker_.shouldExit(); }

    // The format manager registered with WAV/AIFF/FLAC only (no MP3 at v1).
    // Touched only on the loader thread.
    juce::AudioFormatManager formatManager_;

    // Pending load request (message thread -> loader). Only the LATEST matters;
    // a request is published by storing the slot under a short spinlock then
    // bumping latestRequestId_; the loader reads latestRequestId_ then the slot.
    // A single in-flight slot suffices because older queued requests are
    // intentionally discarded (latest wins).
    juce::SpinLock requestLock_;
    LoadRequest pendingRequest_;

    // UI-feedback FIFO (loader -> message thread).
    juce::AbstractFifo eventFifo_{ kEventCapacity };
    LoadEvent eventSlots_[kEventCapacity]{};

    // The ONE publication mechanism for the source sample (architecture.md §4) —
    // the SAME Published<T> the render result and FX history reconfig use.
    Published<SourceSample> published_{};

    SampleChangedFn sampleChanged_{};

    std::atomic<std::uint64_t> nextRequestId_{ 1 };
    // The id of the most recently REQUESTED load. The loader compares the request
    // it is servicing against this to detect supersession at stage boundaries.
    std::atomic<std::uint64_t> latestRequestId_{ 0 };

    juce::WaitableEvent wake_{};
    Worker worker_{ *this };
};

} // namespace mws::plugin
