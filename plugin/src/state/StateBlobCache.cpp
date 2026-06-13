// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// StateBlobCache implementation (task 032). See StateBlobCache.h and
// docs/design/architecture.md §6 (embedded-audio persistence; getState never
// encodes); testing-strategy.md §6 Logic row (autosave under 16 MB, no
// message-thread stall).

#include "StateBlobCache.h"

#include <limits>
#include <utility>

namespace mws::plugin::state {

namespace {

/// FLAC frames are int-indexed by JUCE's reader/writer API. A SourceSample with
/// more frames than INT_MAX cannot round-trip through the FLAC path; such a
/// monster falls back to path-only persistence the same as an over-cap blob.
[[nodiscard]] bool framesFitInt(std::int64_t frames) noexcept
{
    return frames > 0
        && frames <= static_cast<std::int64_t>(std::numeric_limits<int>::max());
}

/// Hex of a 64-bit FNV-1a hash over the file's raw bytes — byte-for-byte the
/// same scheme FileLoader uses for sourceFile.contentHash (architecture.md §6),
/// so a path resolved at restore time can be verified against the saved hash.
/// Returns "" when the file cannot be opened.
[[nodiscard]] juce::String hashFileContents(const juce::File& file)
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

// ---------------------------------------------------------------------------

void StateBlobCache::setEmbedEnabled(bool enabled) noexcept
{
    embedEnabled_.store(enabled, std::memory_order_release);
    if (!enabled)
        clear();
}

void StateBlobCache::clear() noexcept
{
    store(nullptr);
    overCap_.store(false, std::memory_order_release);
}

// --- Loader thread -----------------------------------------------------------

bool StateBlobCache::encodeAndCache(const std::shared_ptr<const SourceSample>& sample)
{
    // ACCEPTANCE CRITERION: encoding NEVER runs on the message thread. The
    // FileLoader fires onSampleChanged on the loader thread; this assert proves
    // it in debug builds (the failure mode the design avoids: encode stalling
    // host autosave on the message thread — architecture.md §6).
    jassert(!juce::MessageManager::existsAndIsCurrentThread());

    if (!embedEnabled_.load(std::memory_order_acquire))
    {
        clear();
        return false;
    }

    if (sample == nullptr || sample->numFrames <= 0 || sample->numChannels == 0)
    {
        clear();
        return false;
    }

    if (!framesFitInt(sample->numFrames))
    {
        // Too large for the FLAC int frame API ⇒ path-only persistence + flag.
        store(nullptr);
        overCap_.store(true, std::memory_order_release);
        return false;
    }

    auto encoded = std::make_shared<juce::MemoryBlock>();
    if (!encodeFlac(*sample, *encoded))
    {
        // Encode failure ⇒ fall back to path-only persistence (no flag: the
        // path still works; only over-cap is a user-surfaced condition).
        store(nullptr);
        overCap_.store(false, std::memory_order_release);
        return false;
    }

    if (encoded->getSize() > kMaxEmbeddedBytes)
    {
        // Over the 16 MB encoded cap ⇒ path-only persistence + the UI flag
        // (architecture.md §6). The big blob is dropped here, on the loader
        // thread, never on the message/audio thread.
        store(nullptr);
        overCap_.store(true, std::memory_order_release);
        return false;
    }

    overCap_.store(false, std::memory_order_release);
    store(std::const_pointer_cast<const juce::MemoryBlock>(encoded));
    return true;
}

bool StateBlobCache::encodeFlac(const SourceSample& sample, juce::MemoryBlock& dest)
{
    const int numChannels = static_cast<int>(sample.numChannels);
    const int numFrames = static_cast<int>(sample.numFrames);
    const double rate = sample.sampleRate > 0.0 ? sample.sampleRate : 44100.0;

    // Copy the immutable channel-major core buffer into a JUCE float buffer the
    // writer consumes. (The source is immutable; we never mutate it.)
    juce::AudioBuffer<float> juceBuf(numChannels, numFrames);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto src = sample.audio.channel(static_cast<std::size_t>(ch));
        float* d = juceBuf.getWritePointer(ch);
        for (int i = 0; i < numFrames; ++i)
            d[i] = src[static_cast<std::size_t>(i)];
    }

    juce::FlacAudioFormat flac;

    // A MemoryOutputStream writing into `dest` (false: start empty, not append).
    // The modern JUCE 8 AudioFormatWriterOptions overload of createWriterFor
    // takes a std::unique_ptr<OutputStream>& and CONSUMES it on success.
    std::unique_ptr<juce::OutputStream> stream =
        std::make_unique<juce::MemoryOutputStream>(dest, /*appendToExistingBlockContent*/ false);

    // 24-bit FLAC: lossless to ~1/2^23 of the float32 source — audition-identical
    // (the SourceSample is decoded float32; the file's own depth is already baked
    // in). FLAC's encoder supports 16/24-bit.
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(rate)
                             .withNumChannels(numChannels)
                             .withBitsPerSample(24);

    std::unique_ptr<juce::AudioFormatWriter> writer(
        flac.createWriterFor(stream, options)); // consumes `stream` on success
    if (writer == nullptr)
        return false; // unsupported config; `dest` left empty

    const bool ok = writer->writeFromAudioSampleBuffer(juceBuf, 0, numFrames);
    writer.reset();   // flush + close (destroys the owned stream)
    return ok && dest.getSize() > 0;
}

// --- setStateInformation (message thread) ------------------------------------

std::shared_ptr<const SourceSample>
StateBlobCache::decodeFlac(const juce::MemoryBlock& flac, const juce::String& name,
                           const juce::String& path, const juce::String& contentHash)
{
    if (flac.getSize() == 0)
        return nullptr;

    juce::FlacAudioFormat flacFormat;
    // A non-owning input stream over the cached bytes (the MemoryBlock outlives
    // the decode — it is held by the dispatched job's captured copy).
    auto input = std::make_unique<juce::MemoryInputStream>(flac.getData(), flac.getSize(),
                                                           /*keepInternalCopy*/ false);
    std::unique_ptr<juce::AudioFormatReader> reader(
        flacFormat.createReaderFor(input.get(), /*deleteStreamIfOpeningFails*/ false));
    if (reader == nullptr)
        return nullptr;
    input.release(); // the reader now owns the stream

    const int numChannels = static_cast<int>(reader->numChannels);
    const auto numFrames = static_cast<std::int64_t>(reader->lengthInSamples);
    if (numChannels <= 0 || numFrames <= 0)
        return nullptr;

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

    sample->name = name;
    sample->path = path;
    sample->contentHash = contentHash; // the SAVED identity (re-render determinism)
    sample->numChannels = static_cast<std::uint32_t>(numChannels);
    sample->numFrames = numFrames;
    sample->sampleRate = reader->sampleRate;
    sample->requestId = 0; // not from a load() request

    return std::shared_ptr<const SourceSample>(std::move(sample));
}

StateBlobCache::RestoreResult
StateBlobCache::restore(const juce::MemoryBlock& embeddedBlob,
                        const juce::ValueTree& stateTree,
                        const mws::engine::ParamSnapshot& savedParams,
                        const std::function<void(const mws::engine::ParamSnapshot&)>&
                            reRenderRequest)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread()
            || !juce::MessageManager::getInstanceWithoutCreating());

    RestoreResult result;
    result.params = savedParams;

    const auto sourceFile = stateTree.getChildWithName(id::sourceFile);
    const juce::String savedPath = sourceFile.getProperty(id::path).toString();
    const juce::String savedHash = sourceFile.getProperty(id::contentHash).toString();
    const juce::String name = juce::File(savedPath).getFileName();

    if (embeddedBlob.getSize() > 0 && embedEnabled_.load(std::memory_order_acquire))
    {
        // Re-cache the embedded bytes verbatim NOW (no encode) so that if the
        // host immediately getState()s again before the async decode lands, the
        // re-save round-trips the same blob — state stays coherent (task scope:
        // "state remains consistent if the host immediately getStates again").
        store(std::make_shared<const juce::MemoryBlock>(embeddedBlob));
        overCap_.store(false, std::memory_order_release);

        // Decode-then-publish OFF the message thread. The blob is copied into
        // the job so it owns its bytes regardless of caller lifetime.
        if (dispatch_ && publish_)
        {
            auto blobCopy = std::make_shared<juce::MemoryBlock>(embeddedBlob);
            auto publish = publish_;
            dispatch_([blobCopy, publish, name, savedPath, savedHash]() {
                if (auto decoded = decodeFlac(*blobCopy, name, savedPath, savedHash))
                    publish(decoded);
            });
        }

        result.source = RestoreSource::Embedded;
    }
    else
    {
        // No embedded blob (or embedding off): resolve the path and verify the
        // content hash (architecture.md §6 — recognise the source without
        // embedding it). The actual reload is the FileLoader's job, kicked by
        // the processor; here we classify whether the path resolves to the SAME
        // bytes that were saved (hash match) so re-rendering stays deterministic.
        const juce::File file(savedPath);
        if (savedPath.isEmpty())
            result.source = RestoreSource::None;
        else if (file.existsAsFile()
                 && (savedHash.isEmpty() || hashFileContents(file) == savedHash))
            result.source = RestoreSource::PathRehash; // same source; processor reloads it
        else
            result.source = RestoreSource::PathMissing; // gone or content changed

        // A stale cache must not survive into the next save when nothing was
        // embedded this restore.
        if (!embedEnabled_.load(std::memory_order_acquire) || embeddedBlob.getSize() == 0)
            store(nullptr);
    }

    // Re-render-on-load (architecture.md §6 determinism rule): render metadata
    // present ⇒ request a deterministic re-render with the SAVED snapshot
    // instead of restoring stored output.
    if (hasRenderMetadata(stateTree) && reRenderRequest)
    {
        reRenderRequest(savedParams);
        result.reRenderRequested = true;
    }

    return result;
}

} // namespace mws::plugin::state
