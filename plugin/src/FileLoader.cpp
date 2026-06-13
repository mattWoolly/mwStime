// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// FileLoader implementation (task 031). See FileLoader.h and
// docs/design/architecture.md §4 / §5.1 for the protocol; ui-design.md §6.1
// for the load flow + error idiom (WAV/AIFF/FLAC, no MP3 at v1).

#include "FileLoader.h"

#include <utility>

namespace mws::plugin {

namespace {

/// Hex of a 64-bit FNV-1a hash over the file's raw bytes. Same bytes ⇒ same
/// hash; cheap and deterministic, enough to recognise a previously-loaded
/// source across sessions (architecture.md §6 sourceFile.contentHash). NOT a
/// cryptographic hash — it only needs to distinguish distinct sources.
juce::String hashFileContents(const juce::File& file)
{
    juce::FileInputStream stream(file);
    if (stream.failedToOpen())
        return {};

    std::uint64_t h = 1469598103934665603ull; // FNV offset basis
    constexpr std::uint64_t prime = 1099511628211ull;

    constexpr int kChunk = 64 * 1024;
    juce::HeapBlock<std::uint8_t> buffer(kChunk);

    while (!stream.isExhausted())
    {
        const int got = stream.read(buffer.getData(), kChunk);
        if (got <= 0)
            break;
        for (int i = 0; i < got; ++i)
        {
            h ^= static_cast<std::uint64_t>(buffer[i]);
            h *= prime;
        }
    }

    return juce::String::toHexString(static_cast<juce::int64>(h));
}

} // namespace

FileLoader::FileLoader()
{
    // WAV/AIFF/FLAC only — NO MP3 at v1 (ui-design §6.1). We register the three
    // formats explicitly rather than registerBasicFormats() (which would also
    // pull in any platform formats); this is the authoritative v1 format set.
    formatManager_.registerFormat(new juce::WavAudioFormat(), /*makeDefault*/ true);
    formatManager_.registerFormat(new juce::AiffAudioFormat(), false);
    formatManager_.registerFormat(new juce::FlacAudioFormat(), false);
}

FileLoader::~FileLoader()
{
    stop();
}

// --- Message-thread API ------------------------------------------------------

std::uint64_t FileLoader::load(const juce::File& file)
{
    const auto id = nextRequestId_.fetch_add(1, std::memory_order_relaxed);

    {
        const juce::SpinLock::ScopedLockType sl(requestLock_);
        pendingRequest_ = LoadRequest{ id, file };
    }
    // Publish the new request id with release ordering AFTER the slot is written,
    // so the loader (which acquires latestRequestId_ then takes the lock to read
    // the slot) always sees a fully-formed request. Bumping this also supersedes
    // any in-flight decode the loader is running (latest wins).
    latestRequestId_.store(id, std::memory_order_release);

    wake_.signal();
    return id;
}

bool FileLoader::popEvent(LoadEvent& out) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    eventFifo_.prepareToRead(1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return false;

    out = eventSlots_[size1 > 0 ? start1 : start2];
    eventFifo_.finishedRead(1);
    return true;
}

void FileLoader::startThread()
{
    if (!worker_.isThreadRunning())
        worker_.startThread();
}

void FileLoader::stop()
{
    if (worker_.isThreadRunning())
    {
        worker_.signalThreadShouldExit();
        wake_.signal();
        worker_.stopThread(2000);
    }
}

// --- Loader side -------------------------------------------------------------

bool FileLoader::pushEvent(const LoadEvent& ev) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    eventFifo_.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return false;

    eventSlots_[size1 > 0 ? start1 : start2] = ev;
    eventFifo_.finishedWrite(1);
    return true;
}

void FileLoader::pushFinished(std::uint64_t requestId, LoadOutcome outcome) noexcept
{
    // The Finished event must never be lost (the UI relies on it to leave the
    // "loading" state). Retry until the FIFO accepts it.
    const LoadEvent ev{ LoadEvent::Kind::Finished, requestId, outcome };
    while (!pushEvent(ev))
    {
        if (workerShouldExit())
            return;
        juce::Thread::sleep(1);
    }
}

void FileLoader::Worker::run()
{
    while (!threadShouldExit())
    {
        // Take the LATEST pending request. Older queued requests are discarded —
        // a drag-storm collapses to one decode of the most recent file (latest
        // wins, mirroring the render worker's coalescing).
        FileLoader::LoadRequest req{};
        bool haveReq = false;

        {
            const juce::SpinLock::ScopedLockType sl(owner_.requestLock_);
            if (owner_.pendingRequest_.id != 0)
            {
                req = owner_.pendingRequest_;
                owner_.pendingRequest_ = FileLoader::LoadRequest{}; // consume
                haveReq = true;
            }
        }

        if (haveReq)
        {
            owner_.pushEvent(LoadEvent{ LoadEvent::Kind::Started, req.id,
                                        LoadOutcome::Loaded });

            std::shared_ptr<SourceSample> sample;
            const LoadOutcome outcome = owner_.decode(req, sample);

            if (outcome == LoadOutcome::Loaded)
            {
                // Re-check supersession AFTER decode and BEFORE publishing: if a
                // newer request landed while we decoded, drop this result so the
                // newer one wins deterministically (we never publish stale audio).
                if (owner_.superseded(req.id))
                {
                    owner_.pushFinished(req.id, LoadOutcome::Superseded);
                }
                else
                {
                    auto immutable = std::shared_ptr<const SourceSample>(std::move(sample));
                    owner_.published_.publish(immutable);

                    // Extension hook (task 032): pre-encode the FLAC state blob
                    // off the message thread. Fired on the LOADER thread with the
                    // just-published sample.
                    if (owner_.sampleChanged_)
                        owner_.sampleChanged_(immutable);

                    owner_.pushFinished(req.id, LoadOutcome::Loaded);
                }
            }
            else
            {
                owner_.pushFinished(req.id, outcome);
            }
        }

        if (threadShouldExit())
            break;

        // If a fresh request raced in after we consumed the slot but before we
        // sleep, don't miss it: only wait when nothing is pending.
        {
            const juce::SpinLock::ScopedLockType sl(owner_.requestLock_);
            if (owner_.pendingRequest_.id != 0)
                continue;
        }

        owner_.wake_.wait(-1);
    }
}

LoadOutcome FileLoader::decode(const LoadRequest& req, std::shared_ptr<SourceSample>& out)
{
    // ACCEPTANCE CRITERION: decode never happens on the message thread. The
    // loader thread is, by construction, not the JUCE message thread; this
    // assert proves it in debug builds.
    jassert(!juce::MessageManager::existsAndIsCurrentThread());

    const juce::File& file = req.file;

    if (!file.existsAsFile())
        return LoadOutcome::ReadFailure;

    // Format gate: WAV/AIFF/FLAC only (no MP3 at v1 — ui-design §6.1). An
    // unsupported extension yields the typed UnsupportedFormat error before any
    // read is attempted; createReaderFor returns null for those too, but the
    // explicit extension check gives the precise typed outcome the UI maps to
    // the hardware error idiom.
    if (formatManager_.findFormatForFileExtension(file.getFileExtension()) == nullptr)
        return LoadOutcome::UnsupportedFormat;

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager_.createReaderFor(file));
    if (reader == nullptr)
        return LoadOutcome::ReadFailure; // unreadable/corrupt (matched ext, bad data)

    const auto numChannels = static_cast<int>(reader->numChannels);
    const auto numFrames = static_cast<std::int64_t>(reader->lengthInSamples);
    if (numChannels <= 0 || numFrames <= 0)
        return LoadOutcome::ReadFailure; // empty/degenerate file

    // Decode to a JUCE float buffer at the file's ORIGINAL sample rate (no
    // resampling here — §5.1: the ingest/resample-to-model-rate step is later in
    // OfflineRenderer). AudioFormatReader::read converts integer PCM to float32.
    juce::AudioBuffer<float> juceBuf(numChannels, static_cast<int>(numFrames));
    reader->read(&juceBuf, 0, static_cast<int>(numFrames), 0, true, true);

    auto sample = std::make_shared<SourceSample>();
    sample->audio = mws::core::AudioBuffer(static_cast<std::size_t>(numChannels),
                                           static_cast<std::size_t>(numFrames));
    sample->audio.sampleRate = reader->sampleRate;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto dst = sample->audio.channel(static_cast<std::size_t>(ch));
        const float* srcCh = juceBuf.getReadPointer(ch);
        for (std::int64_t i = 0; i < numFrames; ++i)
            dst[static_cast<std::size_t>(i)] = srcCh[i];
    }

    sample->name = file.getFileName();
    sample->path = file.getFullPathName();
    sample->contentHash = hashFileContents(file);
    sample->numChannels = static_cast<std::uint32_t>(numChannels);
    sample->numFrames = numFrames;
    sample->sampleRate = reader->sampleRate;
    sample->requestId = req.id;

    out = std::move(sample);
    return LoadOutcome::Loaded;
}

} // namespace mws::plugin
