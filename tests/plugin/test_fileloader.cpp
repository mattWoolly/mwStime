// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// FileLoader off-thread decode tests (plan/backlog/031):
//   - decode a generated WAV and FLAC (fixtures written in-test with JUCE
//     writers) -> correct rate / length / channels / content hash,
//   - an unsupported extension yields the typed UnsupportedFormat error,
//   - a superseding load wins deterministically (latest request published).
//
// docs/design/architecture.md §4 (FILE LOADER THREAD + ownership/publication
// protocol), §5.1 (SourceSample in the data flow); ui-design.md §6.1 (load
// flow + error idiom). Test-case names begin with "fileloader" so
// `ctest -R fileloader` selects them.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FileLoader.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace
{
using mws::plugin::FileLoader;
using mws::plugin::LoadEvent;
using mws::plugin::LoadOutcome;
using mws::plugin::SourceSample;

constexpr int    kFrames = 4096;
constexpr int    kChannels = 2;
constexpr double kRate = 48000.0;

// A deterministic stereo sine pair so the decoded audio is verifiable and the
// content hash is stable across runs.
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

// Create a 16-bit PCM writer for `out` in `format` (modern JUCE 8
// AudioFormatWriterOptions API). Takes ownership of the file's output stream.
std::unique_ptr<juce::AudioFormatWriter> makeWriter(const juce::File& out,
                                                    juce::AudioFormat& format,
                                                    int numChannels)
{
    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(out.createOutputStream());
    if (stream == nullptr)
        return nullptr;

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(kRate)
                             .withNumChannels(numChannels)
                             .withBitsPerSample(16);
    return format.createWriterFor(stream, options); // consumes `stream` on success
}

// Write a fixture in `format` at `kRate`/`kChannels`/`kFrames`. 16-bit PCM
// (lossless for WAV/AIFF/FLAC). Returns the written file.
juce::File writeFixture(const juce::File& dir, const juce::String& name,
                        juce::AudioFormat& format)
{
    juce::AudioBuffer<float> buf(kChannels, kFrames);
    fillSignal(buf);

    const juce::File out = dir.getChildFile(name);
    auto writer = makeWriter(out, format, kChannels);
    REQUIRE(writer != nullptr);

    REQUIRE(writer->writeFromAudioSampleBuffer(buf, 0, kFrames));
    writer.reset(); // flush + close

    REQUIRE(out.existsAsFile());
    return out;
}

// Drain events for up to a timeout until a Finished event for `requestId`
// arrives. Returns the terminal outcome (or a sentinel on timeout).
struct DrainResult {
    bool sawStarted = false;
    bool sawFinished = false;
    LoadOutcome outcome = LoadOutcome::ReadFailure;
};

DrainResult drainUntilFinished(FileLoader& loader, std::uint64_t requestId,
                               int timeoutMs = 5000)
{
    DrainResult r;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        LoadEvent ev;
        bool drainedAny = false;
        while (loader.popEvent(ev))
        {
            drainedAny = true;
            if (ev.requestId != requestId)
                continue;
            if (ev.kind == LoadEvent::Kind::Started)
                r.sawStarted = true;
            else
            {
                r.sawFinished = true;
                r.outcome = ev.outcome;
            }
        }
        loader.collectGarbage();
        if (r.sawFinished)
            break;
        if (!drainedAny)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}

// A scratch directory for fixtures, cleaned up on destruction.
struct TempDir {
    juce::File dir;
    TempDir()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("mwstime_fileloader_"
                                + juce::String(juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("fileloader: decodes a WAV at the file's rate/length/channels", "[fileloader]")
{
    TempDir tmp;
    juce::WavAudioFormat wav;
    const juce::File file = writeFixture(tmp.dir, "tone.wav", wav);

    FileLoader loader;
    loader.startThread();

    const auto id = loader.load(file);
    REQUIRE(id != 0);

    const auto r = drainUntilFinished(loader, id);
    REQUIRE(r.sawStarted);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == LoadOutcome::Loaded);

    auto src = loader.currentSource();
    REQUIRE(src != nullptr);
    REQUIRE(src->requestId == id);
    REQUIRE(src->sampleRate == Catch::Approx(kRate));        // original SR kept (§5.1)
    REQUIRE(src->audio.sampleRate == Catch::Approx(kRate));
    REQUIRE(src->numFrames == kFrames);
    REQUIRE(src->numChannels == (std::uint32_t) kChannels);
    REQUIRE(src->audio.numFrames() == (std::size_t) kFrames);
    REQUIRE(src->audio.numChannels() == (std::size_t) kChannels);
    REQUIRE(src->name == juce::String("tone.wav"));
    REQUIRE(src->contentHash.isNotEmpty());

    loader.stop();
}

TEST_CASE("fileloader: decodes a FLAC at the file's rate/length/channels", "[fileloader]")
{
    TempDir tmp;
    juce::FlacAudioFormat flac;
    const juce::File file = writeFixture(tmp.dir, "tone.flac", flac);

    FileLoader loader;
    loader.startThread();

    const auto id = loader.load(file);
    const auto r = drainUntilFinished(loader, id);
    REQUIRE(r.outcome == LoadOutcome::Loaded);

    auto src = loader.currentSource();
    REQUIRE(src != nullptr);
    REQUIRE(src->sampleRate == Catch::Approx(kRate));
    REQUIRE(src->numFrames == kFrames);
    REQUIRE(src->numChannels == (std::uint32_t) kChannels);
    REQUIRE(src->contentHash.isNotEmpty());

    loader.stop();
}

TEST_CASE("fileloader: content hash is stable for identical bytes, differs across files",
          "[fileloader]")
{
    TempDir tmp;
    juce::WavAudioFormat wav;
    const juce::File a = writeFixture(tmp.dir, "a.wav", wav);

    // Byte-for-byte copy -> identical hash even though the name/path differ.
    const juce::File copy = tmp.dir.getChildFile("a_copy.wav");
    REQUIRE(a.copyFileTo(copy));

    // A different signal -> a different hash.
    const juce::File b = tmp.dir.getChildFile("b.wav");
    {
        juce::AudioBuffer<float> buf(kChannels, kFrames);
        for (int ch = 0; ch < kChannels; ++ch)
            buf.clear(ch, 0, kFrames); // silence: definitely different bytes
        auto w = makeWriter(b, wav, kChannels);
        REQUIRE(w != nullptr);
        REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, kFrames));
    }

    FileLoader loader;
    loader.startThread();

    const auto idA = loader.load(a);
    REQUIRE(drainUntilFinished(loader, idA).outcome == LoadOutcome::Loaded);
    const juce::String hashA = loader.currentSource()->contentHash;

    const auto idCopy = loader.load(copy);
    REQUIRE(drainUntilFinished(loader, idCopy).outcome == LoadOutcome::Loaded);
    const juce::String hashCopy = loader.currentSource()->contentHash;

    const auto idB = loader.load(b);
    REQUIRE(drainUntilFinished(loader, idB).outcome == LoadOutcome::Loaded);
    const juce::String hashB = loader.currentSource()->contentHash;

    REQUIRE(hashA == hashCopy);   // identical bytes ⇒ identical hash
    REQUIRE(hashA != hashB);      // different content ⇒ different hash

    loader.stop();
}

TEST_CASE("fileloader: unsupported extension yields the typed UnsupportedFormat error",
          "[fileloader]")
{
    TempDir tmp;
    // An .mp3 (no MP3 at v1 — ui-design §6.1). The bytes are irrelevant: the
    // extension gate rejects it before any read, with the typed outcome.
    const juce::File mp3 = tmp.dir.getChildFile("loop.mp3");
    mp3.replaceWithText("not really audio");

    FileLoader loader;
    loader.startThread();

    const auto id = loader.load(mp3);
    const auto r = drainUntilFinished(loader, id);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == LoadOutcome::UnsupportedFormat);

    // Nothing published for an unsupported format.
    REQUIRE(loader.currentSource() == nullptr);

    loader.stop();
}

TEST_CASE("fileloader: a missing file yields ReadFailure", "[fileloader]")
{
    TempDir tmp;
    const juce::File missing = tmp.dir.getChildFile("ghost.wav"); // never created

    FileLoader loader;
    loader.startThread();

    const auto id = loader.load(missing);
    const auto r = drainUntilFinished(loader, id);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == LoadOutcome::ReadFailure);
    REQUIRE(loader.currentSource() == nullptr);

    loader.stop();
}

TEST_CASE("fileloader: a superseding load wins deterministically", "[fileloader]")
{
    TempDir tmp;
    juce::WavAudioFormat wav;
    const juce::File first = writeFixture(tmp.dir, "first.wav", wav);

    // A distinct second fixture (different content ⇒ different hash).
    const juce::File second = tmp.dir.getChildFile("second.wav");
    {
        juce::AudioBuffer<float> buf(1, kFrames * 2); // mono, different length
        auto* d = buf.getWritePointer(0);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            d[i] = (i % 2 == 0) ? 0.25f : -0.25f;
        auto w = makeWriter(second, wav, /*numChannels*/ 1);
        REQUIRE(w != nullptr);
        REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
    }

    FileLoader loader;
    loader.startThread();

    // Fire both before the loader can finish the first: the LATEST (second)
    // request must be the one published. Whatever happens to the first request
    // (it may complete-then-be-dropped, be superseded mid-flight, or never run),
    // the published source is deterministically the second file.
    loader.load(first);
    const auto idSecond = loader.load(second);

    const auto r = drainUntilFinished(loader, idSecond);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == LoadOutcome::Loaded);

    auto src = loader.currentSource();
    REQUIRE(src != nullptr);
    REQUIRE(src->requestId == idSecond);      // the latest request won
    REQUIRE(src->name == juce::String("second.wav"));
    REQUIRE(src->numChannels == 1u);
    REQUIRE(src->numFrames == (std::int64_t) (kFrames * 2));

    loader.stop();
}

TEST_CASE("fileloader: onSampleChanged hook fires after a successful publish", "[fileloader]")
{
    TempDir tmp;
    juce::WavAudioFormat wav;
    const juce::File file = writeFixture(tmp.dir, "hook.wav", wav);

    FileLoader loader;
    std::atomic<int> hookCalls{ 0 };
    std::shared_ptr<const SourceSample> seen;
    loader.onSampleChanged([&](std::shared_ptr<const SourceSample> s) {
        seen = std::move(s);
        hookCalls.fetch_add(1, std::memory_order_release);
    });
    loader.startThread();

    const auto id = loader.load(file);
    const auto r = drainUntilFinished(loader, id);
    REQUIRE(r.outcome == LoadOutcome::Loaded);

    // The hook fires on the loader thread BEFORE Finished is posted, so by the
    // time we observe Finished it has already run at least once.
    REQUIRE(hookCalls.load(std::memory_order_acquire) >= 1);
    REQUIRE(seen != nullptr);
    REQUIRE(seen->requestId == id);

    loader.stop();
}

TEST_CASE("fileloader: audio-thread acquire/retire is RT-safe across a publish",
          "[fileloader]")
{
    TempDir tmp;
    juce::WavAudioFormat wav;
    const juce::File file = writeFixture(tmp.dir, "rt.wav", wav);

    FileLoader loader;
    loader.startThread();

    const auto id = loader.load(file);
    REQUIRE(drainUntilFinished(loader, id).outcome == LoadOutcome::Loaded);

    // Simulate one audio block: copy once, read, retire. Must not free inline.
    auto blockPtr = loader.acquireForAudioBlock();
    REQUIRE(blockPtr != nullptr);
    volatile float sink = 0.0f;
    if (blockPtr->audio.numFrames() > 0)
        sink += blockPtr->audio.channel(0)[0];
    (void) sink;
    loader.retireFromAudioBlock(blockPtr);
    REQUIRE(blockPtr == nullptr);             // moved into the graveyard

    REQUIRE(loader.collectGarbage() >= 1);    // freed on the message thread, here
    loader.stop();
}
