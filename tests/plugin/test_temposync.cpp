// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Host tempo sync (plan/backlog/037): when tempoSync == HOST in FX mode the
// effective time factor is derived from sourceBPM + hostBPM via TempoMap and
// applied AT THE ENGINE — the timeFactor automation value is NEVER overwritten
// (clamp/override at the engine, architecture.md §6 pattern). sourceBPM has a
// setter API plus a filename `_174bpm`-style auto-guess that never clobbers a
// user-set value. The computed sync readout (source, host, resulting %) is
// exposed for the LCD model (task 041).
//
// Every value asserted here traces to docs/design/dsp-engine.md §2 (`tempoSync`
// row, formula + CLASSIC integer quantization) and docs/design/ui-design.md
// §6.2 step 4 (SYNC interaction + filename auto-guess).
//
// Test-case names begin with "temposync" so `ctest -R temposync` matches them
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "EngineHost.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;
using mws::engine::FxWindow;
using mws::engine::HopMode;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::RealtimeStretcher;
using mws::engine::SampleRateSel;
using mws::engine::TempoSync;
using mws::model::ModelId;
using mws::plugin::EngineHost;

namespace
{
constexpr double kHostRate = 48000.0;
constexpr int kBlock = 256;

// A clean FX snapshot with CHARACTER OFF (so the model rate is the host rate
// and the synced factor lands on the engine 1:1, the null-test path).
ParamSnapshot syncParams(double timeFactor = 100.0, HopMode hop = HopMode::Revised)
{
    ParamSnapshot p;
    p.model = ModelId::S1000;
    p.bandwidth = 19.2;
    p.sampleRateSel = SampleRateSel::Fs44100;
    p.character = false;
    p.pluginMode = PluginMode::Fx;
    p.fxWindow = FxWindow::OneBar;
    p.tempoSync = TempoSync::Host;
    p.hopMode = hop;
    p.timeFactor = timeFactor;
    return p;
}

RealtimeStretcher::TransportInfo transport(double bpm, double ppq = 0.0,
                                           bool playing = true)
{
    RealtimeStretcher::TransportInfo t;
    t.playing = playing;
    t.bpm = bpm;
    t.ppqPosition = ppq;
    t.timeSigNumerator = 4;
    t.timeSigDenominator = 4;
    return t;
}

// Drive `numBlocks` of silence through the FX path so the engine adopts the
// staged params at a grain boundary and refreshes the sync readout.
void pump(EngineHost& host, const ParamSnapshot& params,
          const RealtimeStretcher::TransportInfo& t, int numBlocks)
{
    std::vector<float> bufL(static_cast<std::size_t>(kBlock), 0.0f);
    std::vector<float> bufR(static_cast<std::size_t>(kBlock), 0.0f);
    for (int b = 0; b < numBlocks; ++b)
    {
        std::fill(bufL.begin(), bufL.end(), 0.0f);
        std::fill(bufR.begin(), bufR.end(), 0.0f);
        float* chans[2] = { bufL.data(), bufR.data() };
        host.processFxBlock(chans, 2, kBlock, params, t);
    }
}

float nextNoise(std::uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(state)) * (0.5f / 2147483648.0f);
}

// Render `kFrames` of identical deterministic noise through one EngineHost and
// return the mono output, advancing the host transport ppq per block at
// `bpm` so SYNC window boundaries are actually crossed (a real playing host).
std::vector<float> renderFx(EngineHost& host, const ParamSnapshot& params,
                            double bpm, std::int64_t kFrames)
{
    std::vector<float> in(static_cast<std::size_t>(kFrames));
    std::uint32_t seed = 7u;
    for (std::int64_t i = 0; i < kFrames; ++i)
        in[static_cast<std::size_t>(i)] = nextNoise(seed);

    const double ppqPerFrame = bpm / 60.0 / kHostRate;

    std::vector<float> out(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> buf(static_cast<std::size_t>(kBlock));
    std::int64_t pos = 0;
    while (pos < kFrames)
    {
        const auto len = std::min<std::int64_t>(kBlock, kFrames - pos);
        for (std::int64_t i = 0; i < len; ++i)
            buf[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(pos + i)];
        float* chans[1] = { buf.data() };
        auto t = transport(bpm, /*ppq=*/static_cast<double>(pos) * ppqPerFrame);
        host.processFxBlock(chans, 1, static_cast<int>(len), params, t);
        for (std::int64_t i = 0; i < len; ++i)
            out[static_cast<std::size_t>(pos + i)] = buf[static_cast<std::size_t>(i)];
        pos += len;
    }
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// The documented example: 174 source / 87 host ⇒ engine sees 200%; the
// timeFactor automation value is NEVER overwritten (dsp-engine.md §2).
// ---------------------------------------------------------------------------

TEST_CASE("temposync: 174 source into 87 host yields effective 200% at the engine",
          "[temposync]")
{
    EngineHost host;
    auto params = syncParams(/*timeFactor=*/100.0); // automation value (HOST overrides it)
    host.setSourceBpm(174.0, /*userSet=*/true);
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);

    // Process at least one grain boundary so the staged sync factor is adopted.
    pump(host, params, transport(/*bpm=*/87.0), /*numBlocks=*/64);

    const auto readout = host.fxSyncReadout();
    REQUIRE(readout.active);
    REQUIRE_THAT(readout.sourceBpm, WithinAbs(174.0, 1e-9));
    REQUIRE_THAT(readout.hostBpm, WithinAbs(87.0, 1e-9));
    // The documented 174 -> 87 = 200% example, asserted verbatim.
    REQUIRE_THAT(readout.resultPercent, WithinAbs(200.0, 1e-9));

    // The timeFactor AUTOMATION value is never rewritten by sync: the snapshot
    // the caller owns is untouched (the override is applied to a copy at the
    // engine — architecture.md §6 clamp/override pattern).
    REQUIRE_THAT(params.timeFactor, WithinAbs(100.0, 1e-9));

    // The engine REALLY consumes the synced factor (not just the readout):
    // holding the transport identical (host 87 BPM, 1-bar window, ppq advancing
    // so a window boundary is actually crossed), a source of 174 BPM (=> 200%
    // stretch) must produce DIFFERENT FX output than a source of 87 BPM (=>
    // 100%, a 1:1 window replay). If the override were dropped on the floor the
    // two renders would be identical. 4 s clears the ~2.76 s 1-bar window @ 87
    // BPM so the synced factor takes effect at the resync.
    constexpr std::int64_t kFrames = 192000; // 4 s

    EngineHost stretch200;
    auto p200 = syncParams(/*timeFactor=*/100.0); // automation stays 100%
    stretch200.setSourceBpm(174.0, /*userSet=*/true); // 174/87 => 200%
    stretch200.prepareFx(kHostRate, kBlock, /*channels=*/1, p200);
    const auto out200 = renderFx(stretch200, p200, /*bpm=*/87.0, kFrames);
    REQUIRE_THAT(stretch200.fxSyncReadout().resultPercent, WithinAbs(200.0, 1e-9));

    EngineHost stretch100;
    auto p100 = syncParams(/*timeFactor=*/100.0);
    stretch100.setSourceBpm(87.0, /*userSet=*/true); // 87/87 => 100%
    stretch100.prepareFx(kHostRate, kBlock, /*channels=*/1, p100);
    const auto out100 = renderFx(stretch100, p100, /*bpm=*/87.0, kFrames);
    REQUIRE_THAT(stretch100.fxSyncReadout().resultPercent, WithinAbs(100.0, 1e-9));

    REQUIRE(std::memcmp(out200.data(), out100.data(), out200.size() * sizeof(float))
            != 0);
}

// ---------------------------------------------------------------------------
// Host BPM change mid-stream updates the effective time factor at the next
// grain boundary (dsp-engine.md §3.5 grain-boundary param application).
// ---------------------------------------------------------------------------

TEST_CASE("temposync: host BPM change mid-stream updates the effective factor",
          "[temposync]")
{
    EngineHost host;
    auto params = syncParams();
    host.setSourceBpm(174.0, /*userSet=*/true);
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);

    // 174 source / 174 host ⇒ 100%.
    pump(host, params, transport(/*bpm=*/174.0), /*numBlocks=*/64);
    REQUIRE_THAT(host.fxSyncReadout().resultPercent, WithinAbs(100.0, 1e-9));

    // Host slows to 87 BPM ⇒ the effective factor doubles at the next grain
    // boundary (174 / 87 ⇒ 200%).
    pump(host, params, transport(/*bpm=*/87.0), /*numBlocks=*/64);
    REQUIRE_THAT(host.fxSyncReadout().resultPercent, WithinAbs(200.0, 1e-9));
    REQUIRE_THAT(host.fxSyncReadout().hostBpm, WithinAbs(87.0, 1e-9));

    // Automation value still pristine through the change.
    REQUIRE_THAT(params.timeFactor, WithinAbs(100.0, 1e-9));
}

// ---------------------------------------------------------------------------
// Filename auto-guess: `amen_174bpm.wav` ⇒ sourceBPM 174 unless previously
// user-set (overridable, never clobbers — ui-design §6.2 step 4).
// ---------------------------------------------------------------------------

TEST_CASE("temposync: filename amen_174bpm.wav guesses sourceBPM 174", "[temposync]")
{
    EngineHost host;
    REQUIRE(host.guessSourceBpmFromFilename("amen_174bpm.wav"));
    REQUIRE_THAT(host.sourceBpm(), WithinAbs(174.0, 1e-9));
}

TEST_CASE("temposync: filename guess is case-insensitive and reads decimals",
          "[temposync]")
{
    EngineHost a;
    REQUIRE(a.guessSourceBpmFromFilename("Break_93.5BPM.aif"));
    REQUIRE_THAT(a.sourceBpm(), WithinAbs(93.5, 1e-9));

    EngineHost b;
    REQUIRE(b.guessSourceBpmFromFilename("LOOP-128BPM-FINAL.WAV"));
    REQUIRE_THAT(b.sourceBpm(), WithinAbs(128.0, 1e-9));

    // No bpm tag in the name ⇒ no guess, value unchanged (still unknown).
    EngineHost c;
    REQUIRE_FALSE(c.guessSourceBpmFromFilename("kick.wav"));
    REQUIRE_THAT(c.sourceBpm(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("temposync: filename guess never clobbers a user-set sourceBPM",
          "[temposync]")
{
    EngineHost host;
    host.setSourceBpm(160.0, /*userSet=*/true);

    // A subsequent filename carrying a tag must NOT overwrite the user value.
    REQUIRE_FALSE(host.guessSourceBpmFromFilename("amen_174bpm.wav"));
    REQUIRE_THAT(host.sourceBpm(), WithinAbs(160.0, 1e-9));

    // But a plain setter with userSet=false (e.g. a previous auto-guess) is
    // overridable by a later filename guess.
    EngineHost host2;
    host2.setSourceBpm(120.0, /*userSet=*/false); // an earlier non-user value
    REQUIRE(host2.guessSourceBpmFromFilename("track_140bpm.wav"));
    REQUIRE_THAT(host2.sourceBpm(), WithinAbs(140.0, 1e-9));
}

// ---------------------------------------------------------------------------
// CLASSIC: the effective synced factor is integer-quantized; the readout
// exposes the achieved value (dsp-engine.md §2 CLASSIC quantization).
// ---------------------------------------------------------------------------

TEST_CASE("temposync: CLASSIC integer-quantizes the synced factor in the readout",
          "[temposync]")
{
    EngineHost host;
    auto params = syncParams(/*timeFactor=*/100.0, /*hop=*/HopMode::Classic);
    // 100 source / 87 host ⇒ 114.94...%, which CLASSIC coerces to an integer %.
    host.setSourceBpm(100.0, /*userSet=*/true);
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);
    pump(host, params, transport(/*bpm=*/87.0), /*numBlocks=*/64);

    const auto readout = host.fxSyncReadout();
    REQUIRE(readout.active);
    // CLASSIC achieved value = round(100 * 100 / 87) = 115%.
    REQUIRE_THAT(readout.resultPercent, WithinAbs(115.0, 1e-9));
    // It really IS an integer in CLASSIC.
    REQUIRE_THAT(readout.resultPercent - std::round(readout.resultPercent),
                 WithinAbs(0.0, 1e-9));
}

TEST_CASE("temposync: SYNC OFF leaves the readout inactive and the factor untouched",
          "[temposync]")
{
    EngineHost host;
    auto params = syncParams();
    params.tempoSync = TempoSync::Off; // SYNC disabled
    host.setSourceBpm(174.0, /*userSet=*/true);
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);
    pump(host, params, transport(/*bpm=*/87.0), /*numBlocks=*/64);

    REQUIRE_FALSE(host.fxSyncReadout().active);
}
