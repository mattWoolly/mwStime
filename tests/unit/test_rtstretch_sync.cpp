// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// RealtimeStretcher SYNC-mode contract tests (plan/backlog/023) — the ADR-006
// SYNC column made executable BEFORE the implementation exists (TDD is
// mandatory for this task). Spec sources:
//   - plan/decisions/006-fx-vs-sample-mode.md (contract table SYNC column +
//     no-transport term)
//   - docs/design/architecture.md §5.2 (SYNC semantics; WINDOW 1/4…8 bars,
//     default 1 bar; wall-clock fallback)
//   - docs/design/dsp-engine.md §3.5 (SYNC bullet: hard resync to the window
//     start at every transport-aligned `fxWindow` boundary; T<100% SYNC plays
//     compressed then silence to the boundary), §2 `fxWindow` row
//   - docs/design/testing-strategy.md §3 items 4c(SYNC)/4d(SYNC), §6 REAPER row
//     (tempo change mid-play, SYNC window follows)
//
// SYNC consumes TempoMap's boundary math (task 021): resync events must land on
// transport-aligned window boundaries. The transport is a scripted FAKE here
// (plain data via setTransport) — no JUCE host (task 033/037).
//
// Test-case names begin with "rtstretch" so `ctest -R rtstretch` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mws/engine/RealtimeStretcher.h"
#include "mws/engine/TempoMap.h"
#include "mws/model/CharacterChain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::core::ConstAudioView;
using mws::engine::FxWindow;
using mws::engine::HopMode;
using mws::engine::ParamSnapshot;
using mws::engine::RealtimeStretcher;
using mws::engine::TempoMap;
using mws::engine::TempoSync;
using mws::model::ModelId;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Deterministic generators (testing-strategy.md §3.6 — fixed content).
// ---------------------------------------------------------------------------
AudioBuffer makeNoise(std::int64_t numFrames, std::uint32_t seed)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    std::uint32_t state = seed;
    for (std::int64_t n = 0; n < numFrames; ++n)
    {
        state = state * 1664525u + 1013904223u;
        view[static_cast<std::size_t>(n)] =
            static_cast<float>(static_cast<std::int32_t>(state))
            * (0.5f / 2147483648.0f);
    }
    return buffer;
}

AudioBuffer makeSine(std::int64_t numFrames, double freqHz, double sampleRate)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] = 0.5f
            * static_cast<float>(
                std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
    return buffer;
}

ParamSnapshot syncParams(ModelId model, double timeFactor, int cycleLen,
                         HopMode hopMode, FxWindow window, bool character)
{
    ParamSnapshot params;
    params.model = model;
    params.timeFactor = timeFactor;
    params.cycleLen = cycleLen;
    params.hopMode = hopMode;
    params.tempoSync = TempoSync::Host;
    params.fxWindow = window;
    params.character = character;
    return params;
}

/// A captured SYNC resync event (window-boundary hard resync).
struct ObservedResync
{
    std::int64_t outIndex = 0;
    std::int64_t windowLenModel = 0;
};

// ---------------------------------------------------------------------------
// Scripted-transport streaming harness. Feeds `in` through `process` in
// fixed-size blocks, advancing a fake transport (ppq from elapsed host samples
// at a per-stage bpm). `tempoChangeAt`/`bpm2` simulate a mid-stream tempo
// change. Character is OFF in every test here, so model rate == host rate and
// the boundary distances are 1:1 host samples.
// ---------------------------------------------------------------------------
struct TransportScript
{
    double bpm = 120.0;
    int tsNum = 4;
    int tsDen = 4;
    double startPpq = 0.0;
    // Optional mid-stream tempo change.
    std::int64_t tempoChangeAtFrame = -1; // < 0 disables
    double bpm2 = 0.0;
    bool playing = true;
};

std::vector<ObservedResync> streamSync(RealtimeStretcher& rt, const AudioBuffer& in,
                                       double hostRate, std::size_t blockLen,
                                       const TransportScript& script)
{
    std::vector<ObservedResync> resyncs;
    const RealtimeStretcher::SyncResyncObserver observer =
        [&](const RealtimeStretcher::SyncResync& r)
    { resyncs.push_back({ r.outIndex, r.windowLenModel }); };
    rt.setSyncResyncObserver(&observer);

    const std::size_t channels = in.numChannels();
    const std::size_t frames = in.numFrames();
    AudioBuffer out(channels, frames);
    std::vector<ConstAudioView> ins(channels);
    std::vector<AudioView> outs(channels);

    std::int64_t pos = 0;
    while (pos < static_cast<std::int64_t>(frames))
    {
        const std::size_t len = std::min(
            blockLen, static_cast<std::size_t>(static_cast<std::int64_t>(frames) - pos));

        // Per-block scripted transport (plain data; host glue is task 033/037).
        double bpm = script.bpm;
        if (script.tempoChangeAtFrame >= 0 && pos >= script.tempoChangeAtFrame)
            bpm = script.bpm2;
        // ppq advances with elapsed host samples at the CURRENT block's bpm
        // anchor — quarter notes = seconds * bpm / 60.
        const double seconds = static_cast<double>(pos) / hostRate;
        const double ppq = script.startPpq + seconds * bpm / 60.0;

        RealtimeStretcher::TransportInfo tr;
        tr.playing = script.playing;
        tr.ppqPosition = ppq;
        tr.bpm = bpm;
        tr.timeSigNumerator = script.tsNum;
        tr.timeSigDenominator = script.tsDen;
        rt.setTransport(tr);

        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            ins[ch] = ConstAudioView{ in.channel(ch).data() + pos, len };
            outs[ch] = AudioView{ out.channel(ch).data() + pos, len };
        }
        rt.process(ins.data(), outs.data(), channels);
        pos += static_cast<std::int64_t>(len);
    }
    rt.setSyncResyncObserver(nullptr);
    return resyncs;
}

} // namespace

// ===========================================================================
// 4d SYNC — scripted transport at 120 BPM, 1-bar window, T=200%: resync events
// land EXACTLY on bar boundaries (architecture.md §5.2 "hard resync to the
// window start at every transport-aligned window boundary"). The boundary
// spacing equals TempoMap's 1-bar length and resyncs never occur mid-window.
// ===========================================================================
TEST_CASE("rtstretch: sync resync lands exactly on transport-aligned bar "
          "boundaries at 120 BPM",
          "[rtstretch][sync]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 512;
    constexpr std::int64_t numFrames = 48000 * 8; // 8 s ⇒ 4 one-bar windows
    const AudioBuffer in = makeNoise(numFrames, 0x5717C001u);

    // 1 bar @ 120 BPM 4/4 = 2 s = 96000 host samples (TempoMap, task 021).
    const double winSamplesD =
        TempoMap::windowBoundary(0.0, 120.0, 4, 4, FxWindow::OneBar, hostRate)
            .samplesToNextBoundary;
    const auto winSamples = static_cast<std::int64_t>(std::llround(winSamplesD));
    REQUIRE(winSamples == 96000);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               syncParams(ModelId::S1000, 200.0, 1000, HopMode::Classic,
                          FxWindow::OneBar, /*character*/ false));
    REQUIRE(rt.modelRate() == hostRate); // character OFF ⇒ model == host

    TransportScript script; // 120 BPM 4/4, transport starts at ppq 0
    const std::vector<ObservedResync> resyncs =
        streamSync(rt, in, hostRate, blockLen, script);

    // Several windows elapse ⇒ several resyncs.
    REQUIRE(resyncs.size() >= 3);
    for (const ObservedResync& r : resyncs)
        REQUIRE(r.windowLenModel == winSamples);

    // Resyncs land on the bar grid: every outIndex is a multiple of the window
    // length (transport starts on a bar line at ppq 0).
    for (const ObservedResync& r : resyncs)
    {
        CAPTURE(r.outIndex);
        REQUIRE(r.outIndex % winSamples == 0);
        REQUIRE(r.outIndex > 0);
    }
    // Consecutive boundaries are spaced exactly one window apart (never
    // mid-window).
    for (std::size_t i = 1; i < resyncs.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(resyncs[i].outIndex - resyncs[i - 1].outIndex == winSamples);
    }
}

// ===========================================================================
// 4c SYNC — T = 50%: the captured window plays COMPRESSED (first half of the
// window), then EXACTLY silence to the boundary (architecture.md §5.2 / the
// dsp-engine.md §3.5 SYNC compression bullet). The audible content occupies
// T/100 of each window; the tail is bit-zero.
// ===========================================================================
TEST_CASE("rtstretch: sync T<100% plays compressed then silence to the window "
          "boundary",
          "[rtstretch][sync]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 480;
    constexpr std::int64_t numFrames = 48000 * 8;
    const AudioBuffer in = makeSine(numFrames, 220.0, hostRate);

    const auto winSamples = static_cast<std::int64_t>(std::llround(
        TempoMap::windowBoundary(0.0, 120.0, 4, 4, FxWindow::OneBar, hostRate)
            .samplesToNextBoundary));
    REQUIRE(winSamples == 96000);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               syncParams(ModelId::S1000, 50.0, 1000, HopMode::Classic,
                          FxWindow::OneBar, /*character*/ false));
    // SYNC does NOT clamp T<100% — compression is allowed (ADR-006 SYNC col).
    REQUIRE_FALSE(rt.clampActive());

    std::vector<ObservedResync> resyncs;
    const RealtimeStretcher::SyncResyncObserver obs =
        [&](const RealtimeStretcher::SyncResync& r)
    { resyncs.push_back({ r.outIndex, r.windowLenModel }); };
    rt.setSyncResyncObserver(&obs);

    AudioBuffer out(1, numFrames);
    std::int64_t pos = 0;
    while (pos < numFrames)
    {
        const std::size_t len = std::min(
            blockLen, static_cast<std::size_t>(numFrames - pos));
        const double seconds = static_cast<double>(pos) / hostRate;
        RealtimeStretcher::TransportInfo tr;
        tr.playing = true;
        tr.ppqPosition = seconds * 120.0 / 60.0;
        tr.bpm = 120.0;
        tr.timeSigNumerator = 4;
        tr.timeSigDenominator = 4;
        rt.setTransport(tr);
        ConstAudioView inView{ in.channel(0).data() + pos, len };
        AudioView outView{ out.channel(0).data() + pos, len };
        rt.process(&inView, &outView, 1);
        pos += static_cast<std::int64_t>(len);
    }
    rt.setSyncResyncObserver(nullptr);
    REQUIRE(resyncs.size() >= 3);

    // For a window that begins at resync index `b` and ends at `b + winSamples`
    // the compressed content occupies the first T/100 of the window; the rest
    // is exactly silence. Check the SECOND full window (the first may include
    // pre-roll/initial behavior before the first boundary).
    const std::int64_t b = resyncs[1].outIndex;
    const std::int64_t contentLen =
        static_cast<std::int64_t>(std::llround(winSamples * 50.0 / 100.0));
    const auto* o = out.channel(0).data();

    // The silence portion [b + contentLen + guard, b + winSamples) is exactly
    // bit-zero. A small guard skips the crossfade tail where the last grain
    // reads partly-past-end material.
    constexpr std::int64_t guard = 2048;
    std::int64_t silentSamples = 0;
    for (std::int64_t i = b + contentLen + guard; i < b + winSamples; ++i)
    {
        REQUIRE(o[static_cast<std::size_t>(i)] == 0.0f);
        ++silentSamples;
    }
    REQUIRE(silentSamples > winSamples / 4); // a substantial silent tail exists

    // The content portion is NOT all silence (the window actually played).
    bool sawContent = false;
    for (std::int64_t i = b; i < b + contentLen; ++i)
        if (o[static_cast<std::size_t>(i)] != 0.0f)
        {
            sawContent = true;
            break;
        }
    REQUIRE(sawContent);
}

// ===========================================================================
// §6 REAPER row — tempo change mid-play: after the host tempo changes
// 120 → 90 BPM the SYNC window follows, so subsequent boundary spacing matches
// the 90 BPM bar length (testing-strategy.md §6 "tempo change mid-play (SYNC
// window follows)").
// ===========================================================================
TEST_CASE("rtstretch: sync boundary spacing follows a mid-stream tempo change "
          "120 to 90 BPM",
          "[rtstretch][sync]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 512;
    constexpr std::int64_t numFrames = 48000 * 16; // long enough for both tempi
    const AudioBuffer in = makeNoise(numFrames, 0x7E3C0u);

    // 1 bar @ 120 = 2.0 s = 96000; @ 90 = 8/3 s = 128000 host samples.
    const auto win120 = static_cast<std::int64_t>(std::llround(
        TempoMap::windowBoundary(0.0, 120.0, 4, 4, FxWindow::OneBar, hostRate)
            .samplesToNextBoundary));
    const auto win90 = static_cast<std::int64_t>(std::llround(
        TempoMap::windowBoundary(0.0, 90.0, 4, 4, FxWindow::OneBar, hostRate)
            .samplesToNextBoundary));
    REQUIRE(win120 == 96000);
    REQUIRE(win90 == 128000);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               syncParams(ModelId::S1000, 200.0, 1000, HopMode::Classic,
                          FxWindow::OneBar, /*character*/ false));

    TransportScript script;
    script.bpm = 120.0;
    script.tempoChangeAtFrame = 48000 * 6; // change after ~3 bars at 120
    script.bpm2 = 90.0;

    const std::vector<ObservedResync> resyncs =
        streamSync(rt, in, hostRate, blockLen, script);
    REQUIRE(resyncs.size() >= 4);

    // Spacings early in the stream are the 120 BPM window; spacings late in the
    // stream are the 90 BPM window. (At least one of each must appear.)
    bool saw120 = false;
    bool saw90 = false;
    for (std::size_t i = 1; i < resyncs.size(); ++i)
    {
        const std::int64_t spacing = resyncs[i].outIndex - resyncs[i - 1].outIndex;
        CAPTURE(i, spacing);
        if (spacing == win120)
            saw120 = true;
        else if (spacing == win90)
            saw90 = true;
        // After the change settles, the reported window length tracks the
        // active tempo's bar length.
        REQUIRE((resyncs[i].windowLenModel == win120
                 || resyncs[i].windowLenModel == win90));
    }
    REQUIRE(saw120);
    REQUIRE(saw90);
    // The last few boundaries are spaced at the 90 BPM window.
    const std::int64_t lastSpacing =
        resyncs.back().outIndex - resyncs[resyncs.size() - 2].outIndex;
    REQUIRE(lastSpacing == win90);
}

// ===========================================================================
// No transport / Standalone fallback (ADR-006): with the transport ABSENT
// (not playing, no tempo), SYNC falls back to a wall-clock window at the
// last-known tempo or 120 BPM (PI). Boundaries are spaced at the 120 BPM
// wall-clock window.
// ===========================================================================
TEST_CASE("rtstretch: sync without transport spaces boundaries at the 120 BPM "
          "wall-clock fallback",
          "[rtstretch][sync]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 512;
    constexpr std::int64_t numFrames = 48000 * 8;
    const AudioBuffer in = makeNoise(numFrames, 0x57A11D0u);

    const auto winSamples = static_cast<std::int64_t>(std::llround(
        TempoMap::windowFromWallClock(0.0, hostRate, FxWindow::OneBar)));
    REQUIRE(winSamples == 96000); // 1 bar @ 120 BPM 4/4

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               syncParams(ModelId::S1000, 200.0, 1000, HopMode::Classic,
                          FxWindow::OneBar, /*character*/ false));

    TransportScript script;
    script.playing = false; // transport absent ⇒ wall-clock fallback
    const std::vector<ObservedResync> resyncs =
        streamSync(rt, in, hostRate, blockLen, script);

    REQUIRE(resyncs.size() >= 3);
    for (const ObservedResync& r : resyncs)
        REQUIRE(r.windowLenModel == winSamples);
    for (std::size_t i = 1; i < resyncs.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(resyncs[i].outIndex - resyncs[i - 1].outIndex == winSamples);
    }
}

// ===========================================================================
// Window-size sweep (architecture.md §5.2: WINDOW = 1/4…8 bars): the boundary
// spacing scales with the window's bar length (TempoMap::windowBars).
// ===========================================================================
TEST_CASE("rtstretch: sync boundary spacing scales across the 1/4..8-bar window "
          "set",
          "[rtstretch][sync]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 512;

    struct Case { FxWindow window; double bars; };
    const auto c = GENERATE(
        Case{ FxWindow::QuarterBar, 0.25 }, Case{ FxWindow::HalfBar, 0.5 },
        Case{ FxWindow::OneBar, 1.0 }, Case{ FxWindow::TwoBars, 2.0 },
        Case{ FxWindow::FourBars, 4.0 }, Case{ FxWindow::EightBars, 8.0 });
    CAPTURE(c.bars);

    // 1 bar @ 120 BPM 4/4 = 2 s = 96000 samples ⇒ window = bars * 96000.
    const auto winSamples =
        static_cast<std::int64_t>(std::llround(c.bars * 96000.0));

    // Drive at least three full windows of the largest (8-bar) case so every
    // window size sees >= 3 boundaries.
    const std::int64_t numFrames = winSamples * 4 + 96000;
    const AudioBuffer in = makeNoise(numFrames, 0x5217E5u);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               syncParams(ModelId::S1000, 200.0, 1000, HopMode::Classic,
                          c.window, /*character*/ false));

    TransportScript script; // 120 BPM 4/4
    const std::vector<ObservedResync> resyncs =
        streamSync(rt, in, hostRate, blockLen, script);

    REQUIRE(resyncs.size() >= 3);
    for (const ObservedResync& r : resyncs)
        REQUIRE(r.windowLenModel == winSamples);
    for (std::size_t i = 1; i < resyncs.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(resyncs[i].outIndex - resyncs[i - 1].outIndex == winSamples);
    }
}
