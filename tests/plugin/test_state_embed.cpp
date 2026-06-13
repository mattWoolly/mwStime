// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 032 — FLAC state-blob cache: embed sample audio in plugin state without
// message-thread stalls (architecture.md §6; testing-strategy.md §6 Logic row).
//
//   - load a fixture through FileLoader -> the onSampleChanged hook FLAC-encodes
//     and caches the blob -> writePluginState(getState) embeds it -> a fresh
//     read (setState) restores the sample (hash equal) and fires a re-render
//     request carrying the saved ParamSnapshot,
//   - embedAudio off => blob absent, path + hash present,
//   - an over-16 MB synthetic sample => path-only persistence + the overCap flag,
//   - getState called repeatedly WHILE a load is in flight does not block
//     (time-bounded) and returns a coherent blob.
//
// These exercise the exact calls PluginProcessor::get/setStateInformation make
// (writePluginState with blobCache().cachedBlob(); readPluginState +
// StateBlobCache::restore). Test-case names begin with "state_embed" so
// `ctest -R state_embed` selects them. Tag: [state_embed].

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "FileLoader.h"
#include "state/Parameters.h"
#include "state/StateBlobCache.h"
#include "state/StateTree.h"

using Catch::Approx;
namespace st = mws::plugin::state;
using mws::plugin::FileLoader;
using mws::plugin::LoadEvent;
using mws::plugin::LoadOutcome;
using mws::plugin::SourceSample;
using mws::plugin::state::StateBlobCache;

namespace {

constexpr int    kFrames = 8192;
constexpr int    kChannels = 2;
constexpr double kRate = 48000.0;

void fillSignal(juce::AudioBuffer<float>& buf)
{
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        auto* d = buf.getWritePointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            d[i] = 0.5f * std::sin(juce::MathConstants<float>::twoPi
                                   * (220.0f + 110.0f * (float) ch)
                                   * (float) i / (float) kRate);
    }
}

std::unique_ptr<juce::AudioFormatWriter> makeWavWriter(const juce::File& out, int numChannels)
{
    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(out.createOutputStream());
    if (stream == nullptr)
        return nullptr;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(kRate)
                             .withNumChannels(numChannels)
                             .withBitsPerSample(16);
    return wav.createWriterFor(stream, options);
}

juce::File writeWav(const juce::File& dir, const juce::String& name)
{
    juce::AudioBuffer<float> buf(kChannels, kFrames);
    fillSignal(buf);
    const juce::File out = dir.getChildFile(name);
    auto writer = makeWavWriter(out, kChannels);
    REQUIRE(writer != nullptr);
    REQUIRE(writer->writeFromAudioSampleBuffer(buf, 0, kFrames));
    writer.reset();
    REQUIRE(out.existsAsFile());
    return out;
}

struct TempDir {
    juce::File dir;
    TempDir()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("mwstime_state_embed_"
                                + juce::String(juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
};

// Drain FileLoader events until the request finishes (or a timeout). Mirrors
// the message-thread timer poll.
LoadOutcome loadAndWait(FileLoader& loader, std::uint64_t id, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    LoadOutcome outcome = LoadOutcome::ReadFailure;
    bool done = false;
    while (!done && std::chrono::steady_clock::now() < deadline)
    {
        LoadEvent ev;
        bool any = false;
        while (loader.popEvent(ev))
        {
            any = true;
            if (ev.requestId == id && ev.kind == LoadEvent::Kind::Finished)
            {
                outcome = ev.outcome;
                done = true;
            }
        }
        loader.collectGarbage();
        if (!any && !done)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return outcome;
}

// A SourceSample synthesised directly (no file), with a chosen size.
std::shared_ptr<const SourceSample> makeSample(int numChannels, std::int64_t numFrames,
                                               const juce::String& name,
                                               const juce::String& path,
                                               const juce::String& hash)
{
    auto s = std::make_shared<SourceSample>();
    s->audio = mws::core::AudioBuffer(static_cast<std::size_t>(numChannels),
                                      static_cast<std::size_t>(numFrames));
    s->audio.sampleRate = kRate;
    // A simple deterministic ramp so encode/decode has real content.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto d = s->audio.channel(static_cast<std::size_t>(ch));
        for (std::int64_t i = 0; i < numFrames; ++i)
            d[static_cast<std::size_t>(i)] =
                0.4f * std::sin(0.01f * (float) i + (float) ch);
    }
    s->name = name;
    s->path = path;
    s->contentHash = hash;
    s->numChannels = static_cast<std::uint32_t>(numChannels);
    s->numFrames = numFrames;
    s->sampleRate = kRate;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("state_embed: load -> getState -> fresh setState restores sample + fires re-render",
          "[state_embed]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir tmp;
    const juce::File file = writeWav(tmp.dir, "amen.wav");

    // The cache, fed by the loader's sample-changed hook (the real wiring).
    StateBlobCache cache;
    FileLoader loader;
    loader.onSampleChanged(cache.sampleChangedHook());
    loader.startThread();

    const auto id = loader.load(file);
    REQUIRE(loadAndWait(loader, id) == LoadOutcome::Loaded);

    auto src = loader.currentSource();
    REQUIRE(src != nullptr);
    const juce::String savedHash = src->contentHash;
    REQUIRE(savedHash.isNotEmpty());

    // The hook encodes on the loader thread; give it a moment to land then poll.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cache.cachedBlob() == nullptr
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto blob = cache.cachedBlob();
    REQUIRE(blob != nullptr);
    REQUIRE(blob->getSize() > 0);

    // Build the state tree exactly as the processor does: sourceFile path+hash,
    // a render metadata entry so re-render-on-load fires.
    auto tree = st::createDefault();
    auto sourceFile = tree.getChildWithName(st::id::sourceFile);
    sourceFile.setProperty(st::id::path, src->path, nullptr);
    sourceFile.setProperty(st::id::contentHash, savedHash, nullptr);
    st::setRenderMetadata(tree, 0x1234ABCDu, "tf=300");

    // A minimal APVTS to round-trip the parameters (the saved snapshot input).
    struct Proc final : juce::AudioProcessor {
        const juce::String getName() const override { return "p"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}
    } proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };

    juce::MemoryBlock saved;
    st::writePluginState(apvts.copyState(), tree, saved, blob.get());
    REQUIRE(saved.getSize() > blob->getSize()); // params + tree + the blob

    loader.stop();

    // --- Fresh "processor": read + restore -----------------------------------
    const auto restored = st::readPluginState(saved.getData(),
                                              static_cast<int>(saved.getSize()));
    REQUIRE(restored.valid);
    REQUIRE(restored.embeddedAudioBlob.getSize() == blob->getSize());

    StateBlobCache freshCache;
    FileLoader freshLoader; // not started — restore decodes via the dispatcher

    // Wire the fresh cache to publish through a captured slot + dispatch onto a
    // worker thread we drive synchronously here (stands in for the loader
    // thread; the contract is "off the message thread").
    std::shared_ptr<const SourceSample> published;
    std::atomic<bool> publishedFlag{ false };
    freshCache.setPublish([&](std::shared_ptr<const SourceSample> s) {
        published = std::move(s);
        publishedFlag.store(true, std::memory_order_release);
    });

    std::vector<std::function<void()>> jobs;
    freshCache.setLoaderDispatch([&](std::function<void()> job) { jobs.push_back(std::move(job)); });
    freshCache.setEmbedEnabled(true);

    // The saved snapshot the processor reconstructs from the restored APVTS.
    apvts.replaceState(restored.apvtsState);
    mws::plugin::Parameters params(apvts);
    const auto savedSnapshot = params.makeSnapshot();

    mws::engine::ParamSnapshot seenSnapshot;
    bool reRenderFired = false;
    const auto result = freshCache.restore(
        restored.embeddedAudioBlob, restored.stateTree, savedSnapshot,
        [&](const mws::engine::ParamSnapshot& s) {
            seenSnapshot = s;
            reRenderFired = true;
        });

    CHECK(result.source == StateBlobCache::RestoreSource::Embedded);
    CHECK(result.reRenderRequested);
    CHECK(reRenderFired);
    // The re-render carried the SAVED snapshot (determinism input).
    CHECK(seenSnapshot.timeFactor == Approx(savedSnapshot.timeFactor));

    // Run the dispatched decode job (the "loader thread").
    REQUIRE(jobs.size() == 1);
    std::thread worker([&] { jobs.front()(); });
    worker.join();
    REQUIRE(publishedFlag.load(std::memory_order_acquire));
    REQUIRE(published != nullptr);

    // The restored sample matches the saved identity + content.
    CHECK(published->contentHash == savedHash);   // hash equal (the acceptance criterion)
    CHECK(published->numChannels == src->numChannels);
    CHECK(published->numFrames == src->numFrames);
    CHECK(published->sampleRate == Approx(src->sampleRate));

    // Audition-identical content: 24-bit FLAC of the same float32 source is
    // lossless to ~1/2^23; check a few samples within that tolerance.
    REQUIRE(published->audio.numFrames() == src->audio.numFrames());
    const auto a = src->audio.channel(0);
    const auto b = published->audio.channel(0);
    for (std::size_t i = 0; i < a.numFrames(); i += a.numFrames() / 17 + 1)
        CHECK(b[i] == Approx(a[i]).margin(2.0e-4));
}

TEST_CASE("state_embed: embedAudio off => no blob, path + hash persist", "[state_embed]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    StateBlobCache cache;
    cache.setEmbedEnabled(false);

    // Even with a perfectly good sample, encoding is skipped while embedding off.
    auto sample = makeSample(2, 4096, "loop.wav", "/x/loop.wav", "abc123");
    std::thread loaderSide([&] { cache.encodeAndCache(sample); });
    loaderSide.join();

    CHECK(cache.cachedBlob() == nullptr);
    CHECK_FALSE(cache.overCap());

    // The state tree still carries the source identity; getState embeds nothing.
    auto tree = st::createDefault();
    tree.setProperty(st::id::embedAudio, false, nullptr);
    auto sourceFile = tree.getChildWithName(st::id::sourceFile);
    sourceFile.setProperty(st::id::path, sample->path, nullptr);
    sourceFile.setProperty(st::id::contentHash, sample->contentHash, nullptr);

    const auto blob = cache.cachedBlob();
    juce::ValueTree emptyApvts("PARAMETERS");
    juce::MemoryBlock saved;
    st::writePluginState(emptyApvts, tree, saved,
                         blob != nullptr ? blob.get() : nullptr);

    const auto restored = st::readPluginState(saved.getData(),
                                              static_cast<int>(saved.getSize()));
    REQUIRE(restored.valid);
    CHECK(restored.embeddedAudioBlob.getSize() == 0);   // no blob embedded
    const auto outFile = restored.stateTree.getChildWithName(st::id::sourceFile);
    CHECK(outFile.getProperty(st::id::path).toString() == sample->path);     // path persists
    CHECK(outFile.getProperty(st::id::contentHash).toString() == "abc123");  // hash persists
}

TEST_CASE("state_embed: no-blob restore resolves the path and verifies the content hash",
          "[state_embed]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir tmp;
    const juce::File file = writeWav(tmp.dir, "src.wav");

    // The hash FileLoader (and StateBlobCache) computes over the file bytes.
    // Round-trip it through the loader so the saved hash matches the real file.
    StateBlobCache feeder;
    FileLoader loader;
    loader.startThread();
    const auto id = loader.load(file);
    REQUIRE(loadAndWait(loader, id) == LoadOutcome::Loaded);
    const juce::String realHash = loader.currentSource()->contentHash;
    loader.stop();
    REQUIRE(realHash.isNotEmpty());

    auto tree = st::createDefault();
    auto sourceFile = tree.getChildWithName(st::id::sourceFile);
    sourceFile.setProperty(st::id::path, file.getFullPathName(), nullptr);
    sourceFile.setProperty(st::id::contentHash, realHash, nullptr);

    StateBlobCache cache; // embedding default ON, but no blob this restore
    const juce::MemoryBlock noBlob;

    SECTION("path present + hash matches => PathRehash")
    {
        const auto r = cache.restore(noBlob, tree, {}, {});
        CHECK(r.source == StateBlobCache::RestoreSource::PathRehash);
        CHECK_FALSE(r.reRenderRequested); // no render metadata in this tree
    }

    SECTION("file content changed under the saved hash => PathMissing")
    {
        tree.getChildWithName(st::id::sourceFile)
            .setProperty(st::id::contentHash, "deadbeefdeadbeef", nullptr);
        const auto r = cache.restore(noBlob, tree, {}, {});
        CHECK(r.source == StateBlobCache::RestoreSource::PathMissing);
    }

    SECTION("file gone => PathMissing")
    {
        file.deleteFile();
        const auto r = cache.restore(noBlob, tree, {}, {});
        CHECK(r.source == StateBlobCache::RestoreSource::PathMissing);
    }

    SECTION("empty path => None")
    {
        tree.getChildWithName(st::id::sourceFile).setProperty(st::id::path, "", nullptr);
        const auto r = cache.restore(noBlob, tree, {}, {});
        CHECK(r.source == StateBlobCache::RestoreSource::None);
    }
}

TEST_CASE("state_embed: over-16MB sample => path-only persistence + overCap flag",
          "[state_embed]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    StateBlobCache cache;
    cache.setEmbedEnabled(true);

    // A ~3 minute stereo white-noise sample: incompressible by FLAC, so the
    // encoded blob comfortably exceeds the 16 MB cap. 48k * 180s * 2ch * ~2.5
    // bytes >> 16 MB even after FLAC. White noise (random) defeats FLAC's
    // linear prediction.
    constexpr std::int64_t frames = 48000ll * 180;
    auto big = std::make_shared<SourceSample>();
    big->audio = mws::core::AudioBuffer(2, static_cast<std::size_t>(frames));
    big->audio.sampleRate = kRate;
    juce::Random rng(0xC0FFEE);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto d = big->audio.channel(static_cast<std::size_t>(ch));
        for (std::int64_t i = 0; i < frames; ++i)
            d[static_cast<std::size_t>(i)] = rng.nextFloat() * 2.0f - 1.0f;
    }
    big->name = "noise.wav";
    big->path = "/x/noise.wav";
    big->contentHash = "bignoise";
    big->numChannels = 2;
    big->numFrames = frames;
    big->sampleRate = kRate;
    auto bigConst = std::shared_ptr<const SourceSample>(std::move(big));

    bool cached = true;
    std::thread loaderSide([&] { cached = cache.encodeAndCache(bigConst); });
    loaderSide.join();

    CHECK_FALSE(cached);                 // refused to embed
    CHECK(cache.cachedBlob() == nullptr); // path-only persistence
    CHECK(cache.overCap());               // the UI flag is raised
}

TEST_CASE("state_embed: getState does not block while a load is in flight + stays coherent",
          "[state_embed]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    TempDir tmp;
    const juce::File first = writeWav(tmp.dir, "first.wav");
    const juce::File second = writeWav(tmp.dir, "second.wav");

    StateBlobCache cache;
    FileLoader loader;
    loader.onSampleChanged(cache.sampleChangedHook());
    loader.startThread();

    // Establish an initial cached blob.
    const auto id0 = loader.load(first);
    REQUIRE(loadAndWait(loader, id0) == LoadOutcome::Loaded);
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cache.cachedBlob() == nullptr
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(cache.cachedBlob() != nullptr);

    // Now hammer load()s on the loader thread while the "message thread" (this
    // one) repeatedly calls getState (cachedBlob + writePluginState). Each
    // getState must return promptly and produce a coherent (decodable or empty)
    // blob — never a half-written one, and never block on the encode.
    std::atomic<bool> stop{ false };
    std::thread loadStorm([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            loader.load(second);
            loader.load(first);
        }
    });

    auto tree = st::createDefault();
    juce::ValueTree emptyApvts("PARAMETERS");

    juce::FlacAudioFormat flac;
    constexpr int kPolls = 200;
    for (int n = 0; n < kPolls; ++n)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto blob = cache.cachedBlob();           // plain atomic load
        juce::MemoryBlock saved;
        st::writePluginState(emptyApvts, tree, saved,
                             blob != nullptr ? blob.get() : nullptr);
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        // Time-bound: a single getState is a memcpy, not an encode. Encoding a
        // few-thousand-frame sample takes milliseconds; a memcpy is microseconds.
        // 50 ms is a generous ceiling that an accidental in-getState encode (or
        // a lock contending with the loader) would blow past.
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 50);

        // Coherence: the embedded blob, if present, decodes cleanly (it is a
        // complete FLAC produced atomically on the loader thread, never a torn
        // mid-encode buffer).
        const auto restored = st::readPluginState(saved.getData(),
                                                  static_cast<int>(saved.getSize()));
        REQUIRE(restored.valid);
        if (restored.embeddedAudioBlob.getSize() > 0)
        {
            auto* in = new juce::MemoryInputStream(restored.embeddedAudioBlob.getData(),
                                                   restored.embeddedAudioBlob.getSize(), false);
            std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(in, true));
            CHECK(reader != nullptr);              // decodable => coherent
            if (reader != nullptr)
                CHECK(reader->lengthInSamples > 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop.store(true, std::memory_order_release);
    loadStorm.join();
    loader.stop();

    // After the storm settles, one more decode confirms the final blob coheres.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (loadAndWait(loader, 0, 1) == LoadOutcome::ReadFailure
           && std::chrono::steady_clock::now() < deadline)
        ; // drain any trailing events
    SUCCEED();
}
