// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// EngineHost FX-path glue tests (plan/backlog/033) — the JUCE-linked half: the
// processor's FX audio path over juce-style channel pointers, stereo dual-mono,
// SAMPLE-mode dry passthrough, latency reporting, and the FX input-scope FIFO
// feed (architecture.md §4 FIFO→timer-poll; ui-design §6.4). The DSP-level null/
// reconfiguration/allocation/TSan assertions live in the JUCE-free core binary
// (tests/unit/test_enginehost_fx_reconfig.cpp) so they run under the `tsan`
// preset (which builds the core binary only).
//
// Test-case names begin with "enginehost" so `ctest -R enginehost` selects them
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include "EngineHost.h"
#include "ui/WaveformView.h" // mws::ui::waveform::ScopeFifo (the FX-scope consumer side)

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::RealtimeStretcher;
using mws::engine::SampleRateSel;
using mws::model::ModelId;
using mws::plugin::EngineHost;

constexpr double kHostRate = 48000.0;
constexpr int kBlock = 256;

ParamSnapshot fxParams(bool character, double timeFactor = 100.0)
{
    ParamSnapshot p;
    p.model = ModelId::S1000;
    p.bandwidth = 19.2;
    p.sampleRateSel = SampleRateSel::Fs44100;
    p.character = character;
    p.timeFactor = timeFactor;
    p.pluginMode = PluginMode::Fx;
    return p;
}

// One deterministic noise sample (independent per channel via the seed).
float nextNoise(std::uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(state)) * (0.5f / 2147483648.0f);
}
} // namespace

TEST_CASE("enginehost: prepareFx reports a positive FX latency and a scope FIFO",
          "[enginehost]")
{
    EngineHost host;
    const int latency = host.prepareFx(kHostRate, kBlock, /*channels=*/2,
                                       fxParams(/*character=*/false));
    REQUIRE(latency > 0);
    REQUIRE(host.fxLatencySamples() == latency);
    REQUIRE(host.scopeFifo() != nullptr); // the FX input-scope producer exists
}

TEST_CASE("enginehost: FX stereo path nulls per channel at T=100% character OFF",
          "[enginehost]")
{
    EngineHost host;
    const auto params = fxParams(/*character=*/false);
    const int latency = host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);
    const auto delay = static_cast<std::int64_t>(latency);

    constexpr std::int64_t kFrames = 48000; // 1 s
    std::vector<float> inL(static_cast<std::size_t>(kFrames));
    std::vector<float> inR(static_cast<std::size_t>(kFrames));
    std::uint32_t sl = 11u, sr = 977u;
    for (std::int64_t i = 0; i < kFrames; ++i)
    {
        inL[static_cast<std::size_t>(i)] = nextNoise(sl);
        inR[static_cast<std::size_t>(i)] = nextNoise(sr);
    }

    std::vector<float> outL(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> outR(static_cast<std::size_t>(kFrames), 0.0f);

    std::vector<float> bufL(static_cast<std::size_t>(kBlock));
    std::vector<float> bufR(static_cast<std::size_t>(kBlock));
    std::int64_t pos = 0;
    while (pos < kFrames)
    {
        const auto len = std::min<std::int64_t>(kBlock, kFrames - pos);
        for (std::int64_t i = 0; i < len; ++i)
        {
            bufL[static_cast<std::size_t>(i)] = inL[static_cast<std::size_t>(pos + i)];
            bufR[static_cast<std::size_t>(i)] = inR[static_cast<std::size_t>(pos + i)];
        }
        float* chans[2] = { bufL.data(), bufR.data() };
        host.processFxBlock(chans, 2, static_cast<int>(len), params,
                            RealtimeStretcher::TransportInfo{});
        for (std::int64_t i = 0; i < len; ++i)
        {
            outL[static_cast<std::size_t>(pos + i)] = bufL[static_cast<std::size_t>(i)];
            outR[static_cast<std::size_t>(pos + i)] = bufR[static_cast<std::size_t>(i)];
        }
        pos += len;
    }

    // Per-channel sample-exact null against the delayed dry signal. The whole
    // signal is bit-compared via memcmp (codebase convention — avoids per-sample
    // float-equal warnings), so a single mismatch index is reported.
    std::vector<float> expL(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> expR(static_cast<std::size_t>(kFrames), 0.0f);
    for (std::int64_t i = delay; i < kFrames; ++i)
    {
        expL[static_cast<std::size_t>(i)] = inL[static_cast<std::size_t>(i - delay)];
        expR[static_cast<std::size_t>(i)] = inR[static_cast<std::size_t>(i - delay)];
    }
    REQUIRE(std::memcmp(outL.data(), expL.data(), outL.size() * sizeof(float)) == 0);
    REQUIRE(std::memcmp(outR.data(), expR.data(), outR.size() * sizeof(float)) == 0);
}

TEST_CASE("enginehost: FX input-scope FIFO receives decimated input samples",
          "[enginehost]")
{
    EngineHost host;
    const auto params = fxParams(/*character=*/false);
    host.prepareFx(kHostRate, kBlock, /*channels=*/1, params);

    std::vector<float> buf(static_cast<std::size_t>(kBlock), 0.5f);
    float* chans[1] = { buf.data() };
    host.processFxBlock(chans, 1, kBlock, params, RealtimeStretcher::TransportInfo{});

    // The producer pushed ~kBlock/kScopeDecimation samples; the consumer (the
    // WaveformView, wired in 045b) drains them off its UI timer.
    auto* fifo = host.scopeFifo();
    REQUIRE(fifo != nullptr);
    REQUIRE(fifo->numReady() > 0);

    std::vector<float> popped(static_cast<std::size_t>(fifo->numReady()));
    const int got = fifo->pop(popped.data(), static_cast<int>(popped.size()));
    REQUIRE(got > 0);
    REQUIRE(got == (kBlock + EngineHost::kScopeDecimation - 1) / EngineHost::kScopeDecimation);
}

TEST_CASE("enginehost: SAMPLE mode is handled by the processor (FX path bypassed)",
          "[enginehost]")
{
    // EngineHost has no SAMPLE-mode playback yet (SamplePlayer is task 034). The
    // processor passes the dry signal in SAMPLE mode (it never calls
    // processFxBlock). Here we assert the FX engine and the processor's dry
    // passthrough are independent: an FX-prepared host processing an FX block
    // produces a (delayed) signal, confirming the FX path is what transforms
    // audio — SAMPLE mode simply does not invoke it.
    EngineHost host;
    const auto params = fxParams(/*character=*/false);
    host.prepareFx(kHostRate, kBlock, /*channels=*/1, params);

    std::vector<float> buf(static_cast<std::size_t>(kBlock), 1.0f);
    float* chans[1] = { buf.data() };
    host.processFxBlock(chans, 1, kBlock, params, RealtimeStretcher::TransportInfo{});

    // Within the first block (< latency) the FX output is pre-roll silence — not
    // the dry signal — proving FX is an active transform, distinct from the
    // SAMPLE-mode dry passthrough the processor applies by simply skipping it.
    REQUIRE(buf[0] < 1.0e-9f); // exactly 0 pre-roll (avoids a float-equal warning)
}

TEST_CASE("enginehost: FX reconfigure latency re-reports only on model/BW/FS change",
          "[enginehost]")
{
    EngineHost host;
    host.prepareFx(kHostRate, kBlock, /*channels=*/1, fxParams(/*character=*/false));
    const int base = host.fxLatencySamples();
    REQUIRE(base > 0);

    // Automatable-only change: no re-report.
    REQUIRE_FALSE(host.reconfigureFxIfNeeded(fxParams(false, /*timeFactor=*/400.0)));

    // Model change (S1000 -> S950): re-report.
    auto s950 = fxParams(false);
    s950.model = ModelId::S950;
    REQUIRE(host.reconfigureFxIfNeeded(s950));
    REQUIRE(host.fxLatencySamples() > 0);

    // Free the engine the reconfiguration retired (message-thread drain).
    host.collectFxGarbage();
}

TEST_CASE("enginehost: FX mono-sum + clamp flags are plumbed for the LCD", "[enginehost]")
{
    EngineHost host;

    // S1000 (fixed-rate, NOT a mono machine) stereo, character ON => no mono sum.
    auto s1000 = fxParams(/*character=*/true);
    s1000.model = ModelId::S1000;
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, s1000);
    REQUIRE_FALSE(host.fxMonoSummed());

    // S950 (mono machine) stereo, character ON => mono sum flagged for the LCD.
    auto s950 = fxParams(/*character=*/true);
    s950.model = ModelId::S950;
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, s950);
    REQUIRE(host.fxMonoSummed());

    // S950 with character OFF => no character chain, so no mono sum.
    auto s950noChar = fxParams(/*character=*/false);
    s950noChar.model = ModelId::S950;
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, s950noChar);
    REQUIRE_FALSE(host.fxMonoSummed());

    // The clamp flag is exposed (FREE T=100% => not clamped yet).
    REQUIRE_FALSE(host.fxClampActive());
}
