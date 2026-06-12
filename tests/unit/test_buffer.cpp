// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"

#include <type_traits>

// Test-case names begin with the tag word so `ctest -R buffer` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::core::ConstAudioView;
} // namespace

TEST_CASE("buffer: construction zero-initialises all channels", "[buffer]")
{
    const AudioBuffer buffer(2, 64);

    REQUIRE(buffer.numChannels() == 2);
    REQUIRE(buffer.numFrames() == 64);

    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
    {
        const ConstAudioView view = buffer.channel(ch);
        REQUIRE(view.size() == 64);
        REQUIRE(view.data() != nullptr);
        for (std::size_t i = 0; i < view.size(); ++i)
            REQUIRE(view[i] == 0.0f);
    }
}

TEST_CASE("buffer: default-constructed buffer is empty", "[buffer]")
{
    const AudioBuffer buffer;
    REQUIRE(buffer.numChannels() == 0);
    REQUIRE(buffer.numFrames() == 0);
}

TEST_CASE("buffer: channel views alias the buffer's own memory", "[buffer]")
{
    AudioBuffer buffer(2, 16);

    AudioView ch0 = buffer.channel(0);
    ch0[3] = 0.5f;

    // A fresh view over the same channel sees the write — same memory.
    AudioView again = buffer.channel(0);
    REQUIRE(again.data() == ch0.data());
    REQUIRE(again[3] == 0.5f);

    // Channels are distinct, contiguous, non-overlapping spans.
    AudioView ch1 = buffer.channel(1);
    REQUIRE(ch1.data() != ch0.data());
    REQUIRE(ch1[3] == 0.0f);
    REQUIRE(ch1.data() == ch0.data() + buffer.numFrames()); // contiguous layout

    // Const view over the mutated channel reads through to the same memory.
    const AudioBuffer& constRef = buffer;
    ConstAudioView constView = constRef.channel(0);
    REQUIRE(constView.data() == ch0.data());
    REQUIRE(constView[3] == 0.5f);
}

TEST_CASE("buffer: resize reshapes and zero-initialises", "[buffer]")
{
    AudioBuffer buffer(1, 8);
    buffer.channel(0)[0] = 1.0f;

    buffer.resize(3, 32);
    REQUIRE(buffer.numChannels() == 3);
    REQUIRE(buffer.numFrames() == 32);

    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
    {
        ConstAudioView view = std::as_const(buffer).channel(ch);
        REQUIRE(view.size() == 32);
        for (std::size_t i = 0; i < view.size(); ++i)
            REQUIRE(view[i] == 0.0f);
    }

    buffer.resize(0, 0);
    REQUIRE(buffer.numChannels() == 0);
    REQUIRE(buffer.numFrames() == 0);
}

TEST_CASE("buffer: sampleRate field round-trips", "[buffer]")
{
    AudioBuffer buffer(1, 4);
    REQUIRE(buffer.sampleRate == 0.0); // unset by default

    buffer.sampleRate = 44100.0;
    REQUIRE(buffer.sampleRate == 44100.0);

    buffer.resize(2, 8); // resize must not clobber the sample rate
    REQUIRE(buffer.sampleRate == 44100.0);
}

TEST_CASE("buffer: views support iteration and frame counts", "[buffer]")
{
    AudioBuffer buffer(1, 5);
    AudioView view = buffer.channel(0);
    REQUIRE(view.numFrames() == 5);

    float fill = 1.0f;
    for (float& sample : view)
        sample = fill++;

    ConstAudioView constView = view; // mutable -> const conversion
    float expected = 1.0f;
    for (const float sample : constView)
        REQUIRE(sample == expected++);

    // Empty view is well-formed.
    const AudioView empty;
    REQUIRE(empty.size() == 0);
    REQUIRE(empty.empty());
}

TEST_CASE("buffer: const-correctness compiles as expected", "[buffer]")
{
    // const buffer yields a const view; mutable buffer yields a mutable view.
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const AudioBuffer&>().channel(0)),
                                  ConstAudioView>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<AudioBuffer&>().channel(0)),
                                  AudioView>);

    // Mutable view converts to const view, never the other way round.
    STATIC_REQUIRE(std::is_convertible_v<AudioView, ConstAudioView>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<ConstAudioView, AudioView>);

    // Element access types.
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<AudioView>()[0]), float&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<ConstAudioView>()[0]),
                                  const float&>);
}
