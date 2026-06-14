// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Host smoke matrix — the ONE row that must actually EXECUTE headlessly on this
// box (plan/backlog/048b-host-smoke-matrix.md): the **Standalone no-transport
// SYNC fallback** (testing-strategy.md §6 Standalone row; ADR-006 "No transport
// / Standalone" term; architecture.md §5.2 wall-clock fallback).
//
// WHY this is a host-smoke check and not just a unit test: the Standalone build
// is a host with no bar grid — there is no AudioPlayHead, so a SYNC-mode FX
// insert must NOT stall or free-run; it must fall back to a wall-clock window at
// the last-known tempo (else 120 BPM (PI)) and keep producing audio with the
// window/resync behaviour intact. We can prove that headlessly by driving the
// JUCE-free RealtimeStretcher (the exact engine the Standalone app runs) with NO
// transport supplied — no window, no GUI, no DAW (the task's headless-safe
// constraint: "drive the audio processor directly … never a visible app
// window"). The matrix's other DAW rows (REAPER/Logic/Bitwig/Ardour) are
// scripted-but-self-skipping (run_reaper_smoke.sh, exit 77) or manual.
//
// This file lives in the JUCE-free core test binary (tests/CMakeLists.txt) and
// is registered as the labelled CTest `host-smoke: Standalone no-transport SYNC
// fallback (headless)` (tests/CMakeLists.txt), which is what
// `ctest -L host-smoke` selects on macOS alongside the REAPER self-skip.
//
// Test-case names begin with "host-smoke" so `ctest -R host-smoke` matches too
// (plan/backlog/README.md test-selection rules), and they carry the
// `[host-smoke]` tag so the labelled CTest entry selects exactly this set.

#include <catch2/catch_test_macros.hpp>

#include "mws/engine/RealtimeStretcher.h"
#include "mws/engine/TempoMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// Deterministic noise (testing-strategy.md §3.6 — fixed content, no rng dep).
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
        view[static_cast<std::size_t>(n)] =
            0.5f
            * static_cast<float>(
                std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
    return buffer;
}

ParamSnapshot syncParams(double timeFactor, FxWindow window)
{
    ParamSnapshot params;
    params.model = ModelId::S1000;
    params.pluginMode = mws::engine::PluginMode::Fx;
    params.timeFactor = timeFactor;
    params.cycleLen = 1000;
    params.hopMode = HopMode::Classic;
    params.tempoSync = TempoSync::Host; // SYNC requested...
    params.fxWindow = window;
    params.character = false; // model rate == host rate (1:1 boundaries)
    return params;
}

struct ObservedResync
{
    std::int64_t outIndex = 0;
    std::int64_t windowLenModel = 0;
};

// Streams `in` through `rt` in fixed blocks, supplying a transport ONLY when
// `withTransport` is true. With it false the engine is a Standalone insert: NO
// setTransport() ever fires (or, for the "stopped" variant, only a not-playing
// transport with no tempo) — the wall-clock fallback must engage.
std::vector<ObservedResync> streamStandalone(RealtimeStretcher& rt,
                                             const AudioBuffer& in, double hostRate,
                                             std::size_t blockLen,
                                             bool supplyStoppedTransport)
{
    std::vector<ObservedResync> resyncs;
    const RealtimeStretcher::SyncResyncObserver observer =
        [&](const RealtimeStretcher::SyncResync& r)
    { resyncs.push_back({ r.outIndex, r.windowLenModel }); };
    rt.setSyncResyncObserver(&observer);

    const std::size_t frames = in.numFrames();
    AudioBuffer out(1, frames);
    std::int64_t pos = 0;
    while (pos < static_cast<std::int64_t>(frames))
    {
        const std::size_t len = std::min(
            blockLen, static_cast<std::size_t>(static_cast<std::int64_t>(frames) - pos));

        if (supplyStoppedTransport)
        {
            // A Standalone host that DOES expose a play head but is not running:
            // playing == false, no tempo. ADR-006 says SYNC still falls back to
            // the wall-clock window (last-known tempo or 120 BPM).
            RealtimeStretcher::TransportInfo tr;
            tr.playing = false;
            tr.bpm = 0.0;
            rt.setTransport(tr);
        }
        // else: NEVER call setTransport — the bare-Standalone "no play head" case.

        ConstAudioView inView{ in.channel(0).data() + pos, len };
        AudioView outView{ out.channel(0).data() + pos, len };
        rt.process(&inView, &outView, 1);
        pos += static_cast<std::int64_t>(len);
    }
    rt.setSyncResyncObserver(nullptr);
    return resyncs;
}

} // namespace

// ===========================================================================
// Standalone row, primary check: SYNC requested but NO transport ever supplied
// (the bare Standalone app — no AudioPlayHead at all). The engine must fall
// back to the TempoMap wall-clock window at 120 BPM (PI) and keep resyncing on
// that grid — it must NOT stall and must NOT free-run without windows.
// ===========================================================================
TEST_CASE("host-smoke: Standalone SYNC with no transport falls back to the "
          "120 BPM wall-clock window",
          "[host-smoke]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 512;
    constexpr std::int64_t numFrames = 48000 * 8; // 8 s ⇒ 4 one-bar windows
    const AudioBuffer in = makeNoise(numFrames, 0x57A11D0u);

    // 1 bar @ 120 BPM 4/4 = 2 s = 96000 host samples (TempoMap fallback math).
    const auto winSamples = static_cast<std::int64_t>(std::llround(
        TempoMap::windowFromWallClock(0.0, hostRate, FxWindow::OneBar)));
    REQUIRE(winSamples == 96000);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1, syncParams(200.0, FxWindow::OneBar));
    REQUIRE(rt.modelRate() == hostRate); // character OFF ⇒ model == host

    const std::vector<ObservedResync> resyncs =
        streamStandalone(rt, in, hostRate, blockLen, /*supplyStoppedTransport*/ false);

    // Several wall-clock windows elapse ⇒ several resyncs (NOT a stall).
    REQUIRE(resyncs.size() >= 3);
    for (const ObservedResync& r : resyncs)
        REQUIRE(r.windowLenModel == winSamples);
    // Boundaries spaced exactly one wall-clock window apart (window behaviour
    // is preserved even with no bar grid).
    for (std::size_t i = 1; i < resyncs.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(resyncs[i].outIndex - resyncs[i - 1].outIndex == winSamples);
    }
}

// ===========================================================================
// Standalone row, stopped-transport variant: a Standalone host that exposes a
// play head but is NOT playing (no tempo) must behave identically to the bare
// case — the wall-clock fallback, not a stall. (ADR-006: "No transport /
// Standalone: FREE rules apply; SYNC falls back to a wall-clock window".)
// ===========================================================================
TEST_CASE("host-smoke: Standalone SYNC with a stopped play head uses the same "
          "wall-clock fallback",
          "[host-smoke]")
{
    constexpr double hostRate = 44100.0; // a different host rate for good measure
    constexpr std::size_t blockLen = 480;
    constexpr std::int64_t numFrames = 44100 * 8;
    const AudioBuffer in = makeNoise(numFrames, 0x5717C001u);

    const auto winSamples = static_cast<std::int64_t>(std::llround(
        TempoMap::windowFromWallClock(0.0, hostRate, FxWindow::OneBar)));
    REQUIRE(winSamples == 88200); // 1 bar @ 120 BPM 4/4 @ 44.1 kHz

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1, syncParams(200.0, FxWindow::OneBar));

    const std::vector<ObservedResync> resyncs =
        streamStandalone(rt, in, hostRate, blockLen, /*supplyStoppedTransport*/ true);

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
// Standalone row, audio-liveness check: the no-transport fallback actually
// PASSES AUDIO (an FX insert that produces silence in Standalone would be a
// bug). With T=200% the read head advances at < 1 sample/sample so every
// wall-clock window is fully filled with non-zero content.
// ===========================================================================
TEST_CASE("host-smoke: Standalone no-transport SYNC keeps producing audio",
          "[host-smoke]")
{
    constexpr double hostRate = 48000.0;
    constexpr std::size_t blockLen = 256;
    constexpr std::int64_t numFrames = 48000 * 6;
    const AudioBuffer in = makeSine(numFrames, 220.0, hostRate);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1, syncParams(200.0, FxWindow::OneBar));

    AudioBuffer out(1, numFrames);
    std::int64_t pos = 0;
    while (pos < numFrames)
    {
        const std::size_t len =
            std::min(blockLen, static_cast<std::size_t>(numFrames - pos));
        // No transport supplied at all (bare Standalone).
        ConstAudioView inView{ in.channel(0).data() + pos, len };
        AudioView outView{ out.channel(0).data() + pos, len };
        rt.process(&inView, &outView, 1);
        pos += static_cast<std::int64_t>(len);
    }

    // After the reported latency pre-roll, output energy is clearly non-zero.
    const auto* o = out.channel(0).data();
    const std::int64_t start = rt.latencySamples() + 4096;
    double energy = 0.0;
    bool sawNonZero = false;
    for (std::int64_t i = start; i < numFrames; ++i)
    {
        const double s = o[static_cast<std::size_t>(i)];
        energy += s * s;
        if (s != 0.0)
            sawNonZero = true;
    }
    REQUIRE(sawNonZero);
    REQUIRE(energy > 1.0); // a real, audible signal — not a silent insert

    // No NaN / inf escaped the no-transport path.
    for (std::int64_t i = 0; i < numFrames; ++i)
        REQUIRE(std::isfinite(o[static_cast<std::size_t>(i)]));
}
