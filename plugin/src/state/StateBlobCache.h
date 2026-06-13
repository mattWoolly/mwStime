// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// StateBlobCache (task 032) — embedded-audio session persistence.
//
// The loaded sample is FLAC-encoded ON THE FILE-LOADER THREAD when it
// loads/changes and the resulting juce::MemoryBlock is cached, so
// getStateInformation only memcpys the cached blob — it NEVER encodes on the
// message thread (the host-autosave reality the design avoids;
// architecture.md §6, testing-strategy.md §6 Logic row "no message-thread
// stall"). A reload decodes the embedded FLAC back to a SourceSample (published
// via the task-030/031 RCU paths), or — when no blob is embedded — resolves the
// sourceFile path and verifies its content hash; then it fires the deterministic
// re-render request carrying the saved ParamSnapshot (render metadata from 029).
//
// Embedding policy (architecture.md §6, dsp-engine.md §2):
//   - embedAudio default ON; when OFF the blob is never cached and persistence
//     is path + hash only.
//   - The encoded FLAC is capped at 16 MB. Over the cap ⇒ path-only persistence
//     and a UI flag (overCap) so the editor can surface it (editor wiring is a
//     later task — out of scope here).
//
// Threading:
//   - encode() runs only on the loader thread (FileLoader::onSampleChanged); a
//     debug assert proves it never runs on the JUCE message thread.
//   - getStateInformation reads the cached blob via cachedBlob() — a plain
//     atomic load + memcpy, no encode, debug-asserted off the audio thread and
//     intended for the message thread.
//   - setStateInformation arrives on the message thread; the decode of an
//     embedded blob is dispatched to the loader thread (decodeAndPublish runs
//     there) so the message thread never decodes. State stays coherent if the
//     host immediately getState()s again: the just-restored blob is re-cached
//     synchronously from the embedded bytes (already encoded — no encode), so a
//     re-save round-trips the same bytes even before the async decode lands.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "mws/engine/Params.h"

#include "../FileLoader.h" // SourceSample
#include "StateTree.h"

namespace mws::plugin::state {

/// Encodes a decoded SourceSample to a FLAC juce::MemoryBlock and caches it for
/// getStateInformation. Owns nothing it cannot free on the message thread; the
/// cache itself is an atomically-swapped shared_ptr<const juce::MemoryBlock> so
/// the loader thread (producer) and message thread (consumer) never tear.
class StateBlobCache
{
public:
    /// The encoded-FLAC embed cap (architecture.md §6: default on ≤ 16 MB
    /// encoded). Over this ⇒ path-only persistence + the overCap flag.
    static constexpr std::size_t kMaxEmbeddedBytes = 16u * 1024u * 1024u;

    /// A dispatcher that runs `job` on the file-loader thread (off the message
    /// thread). Wired by the processor to FileLoader so setStateInformation's
    /// decode happens there. Synchronous dispatchers are allowed (tests use
    /// one); the contract is only "not the message thread for a real decode".
    using LoaderDispatch = std::function<void(std::function<void()>)>;

    /// Publishes a decoded SourceSample to the rest of the plugin (the 030/031
    /// RCU path — in the processor this forwards into the same Published<>
    /// the FileLoader feeds). Called on the loader thread.
    using PublishFn = std::function<void(std::shared_ptr<const SourceSample>)>;

    StateBlobCache() = default;

    StateBlobCache(const StateBlobCache&) = delete;
    StateBlobCache& operator=(const StateBlobCache&) = delete;

    // --- Configuration (message thread, before use) ---------------------------

    /// Whether embedding is enabled (state-tree `embedAudio`). When false the
    /// cache stays empty and getStateInformation embeds nothing (path + hash
    /// only). Changing this clears any cached blob so it cannot leak into a
    /// later save. Message thread.
    void setEmbedEnabled(bool enabled) noexcept;
    [[nodiscard]] bool embedEnabled() const noexcept
    {
        return embedEnabled_.load(std::memory_order_acquire);
    }

    /// The loader-thread dispatcher used by restore() to decode off the message
    /// thread. Message thread, before use.
    void setLoaderDispatch(LoaderDispatch dispatch) { dispatch_ = std::move(dispatch); }

    /// The publish callback restore() uses after decoding an embedded blob.
    /// Message thread, before use.
    void setPublish(PublishFn publish) { publish_ = std::move(publish); }

    // --- Loader thread (FileLoader::onSampleChanged) --------------------------

    /// FLAC-encode `sample` and cache the resulting blob, respecting the embed
    /// flag and the 16 MB cap. Runs on the LOADER thread (asserted: never the
    /// message thread — the acceptance criterion). A null/empty sample, or
    /// embedding disabled, clears the cache. Over the cap clears the cache and
    /// raises the overCap flag. Returns true iff a blob was cached.
    bool encodeAndCache(const std::shared_ptr<const SourceSample>& sample);

    /// Convenience adaptor matching FileLoader::SampleChangedFn so the cache can
    /// be installed directly via FileLoader::onSampleChanged.
    [[nodiscard]] FileLoader::SampleChangedFn sampleChangedHook()
    {
        return [this](std::shared_ptr<const SourceSample> s) { encodeAndCache(s); };
    }

    // --- getStateInformation (message thread) ---------------------------------

    /// The currently cached FLAC blob, or an empty shared_ptr when nothing is
    /// embedded (embedding off, no sample, or over-cap). NEVER encodes — a plain
    /// atomic load. getStateInformation passes its ->data()/->getSize() straight
    /// to writePluginState (a memcpy, architecture.md §6). Message thread.
    [[nodiscard]] std::shared_ptr<const juce::MemoryBlock> cachedBlob() const noexcept
    {
        return std::atomic_load_explicit(&blob_, std::memory_order_acquire);
    }

    /// True iff the last encodeAndCache refused to embed because the encoded
    /// FLAC exceeded kMaxEmbeddedBytes (path-only persistence; a UI flag —
    /// editor surfacing is out of scope here). Message thread / tests.
    [[nodiscard]] bool overCap() const noexcept
    {
        return overCap_.load(std::memory_order_acquire);
    }

    // --- setStateInformation (message thread) ---------------------------------

    /// What restore() did, for the processor + tests.
    enum class RestoreSource : std::uint8_t {
        None,       ///< nothing to restore (no blob, no path)
        Embedded,   ///< an embedded FLAC blob was decoded + published
        PathRehash, ///< no blob; the sourceFile path resolved + hash verified
        PathMissing ///< no blob; the sourceFile path is gone or its hash changed
    };

    struct RestoreResult {
        RestoreSource source = RestoreSource::None;
        bool reRenderRequested = false;        ///< render metadata present ⇒ re-render fired
        mws::engine::ParamSnapshot params{};   ///< the saved snapshot handed to the re-render
    };

    /// Restore embedded audio (or resolve the path) for a just-read state.
    ///
    /// `embeddedBlob` is readPluginState()'s pass-through FLAC bytes (possibly
    /// empty). `stateTree` is the migrated non-parameter tree (sourceFile +
    /// render metadata). `savedParams` is the ParamSnapshot reconstructed by the
    /// processor from the restored APVTS (the render metadata's paramsUsed is an
    /// opaque echo; the live snapshot is the deterministic re-render input,
    /// architecture.md §6).
    ///
    /// On an embedded blob: the bytes are cached verbatim (so an immediate
    /// re-save round-trips them with no encode) and a decode-then-publish job is
    /// dispatched to the loader thread. With no blob: the path is resolved and
    /// its content hash compared to the stored hash. In both cases, if render
    /// metadata exists, `reRenderRequest(savedParams)` is invoked. Message
    /// thread. Returns what happened.
    RestoreResult restore(const juce::MemoryBlock& embeddedBlob,
                          const juce::ValueTree& stateTree,
                          const mws::engine::ParamSnapshot& savedParams,
                          const std::function<void(const mws::engine::ParamSnapshot&)>&
                              reRenderRequest);

    /// Clear the cache (e.g. embedding turned off). Message/loader thread.
    void clear() noexcept;

private:
    /// FLAC-encode a SourceSample into `dest`. Returns false on encode failure.
    /// Pure function of the sample; no member state touched. Static so the
    /// loader-thread decode job can reuse it without aliasing `this`.
    static bool encodeFlac(const SourceSample& sample, juce::MemoryBlock& dest);

    /// Decode FLAC bytes into a SourceSample (loader thread). Returns nullptr on
    /// failure. The decoded sample keeps the original rate/channels; name/path/
    /// hash come from `meta` so the restored sample matches the saved identity.
    static std::shared_ptr<const SourceSample>
    decodeFlac(const juce::MemoryBlock& flac, const juce::String& name,
               const juce::String& path, const juce::String& contentHash);

    void store(std::shared_ptr<const juce::MemoryBlock> blob) noexcept
    {
        std::atomic_store_explicit(&blob_, std::move(blob), std::memory_order_release);
    }

    std::shared_ptr<const juce::MemoryBlock> blob_{}; ///< atomically swapped
    std::atomic<bool> embedEnabled_{ defaults::embedAudio };
    std::atomic<bool> overCap_{ false };

    LoaderDispatch dispatch_{};
    PublishFn publish_{};
};

} // namespace mws::plugin::state
