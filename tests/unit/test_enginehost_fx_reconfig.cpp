// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// FX history-reconfiguration handoff test (plan/backlog/033). This is the
// ThreadSanitizer path testing-strategy.md §3.6 names explicitly: "TSan over
// the render-publish/swap path AND the FX history reconfiguration"
// (architecture.md §4 ownership/publication protocol).
//
// FxEngine is deliberately JUCE-free (it only uses Published.h's GraveyardFifo
// and the JUCE-free RealtimeStretcher), so this translation unit compiles into
// the JUCE-free core test binary and runs under the `tsan` preset — which has
// MWS_BUILD_PLUGIN=OFF, so the full JUCE processor tests are NOT built there.
//
// What is asserted (the reconfiguration protocol made falsifiable):
//   - The message thread builds a fresh prepared RealtimeStretcher on a
//     model/bandwidth/FS change and publishes it; the audio thread adopts it at
//     the next block and retires the old one to a graveyard — race-free (TSan
//     clean) and never freed on the audio thread.
//   - The audio thread keeps producing finite (no NaN/inf) output across a storm
//     of reconfigurations concurrent with block processing.
//   - The graveyard fully drains; deterministic shutdown; nothing leaks.
//   - Latency is re-reported on a model/bandwidth/FS change and matches the
//     newly prepared engine's latencySamples().
//
// Test-case names begin with "enginehost" so `ctest -R enginehost --no-tests=
// error` selects them (plan/backlog/README.md test-selection rules); the
// concurrency cases also carry the `[tsan]` tag and are registered as a
// tsan-LABELLED CTest in tests/CMakeLists.txt so `ctest -L tsan` picks them up.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FxEngine.h"

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"

// The test binary's single global operator-new replacement (test_butterworth.cpp)
// lets the automation-spam case assert the audio-thread FX path allocates
// NOTHING after prepare() (architecture.md §4; acceptance criterion).
#include "TestAllocationCounter.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
using mws::core::AudioView;
using mws::core::ConstAudioView;
using mws::engine::ParamSnapshot;
using mws::engine::RealtimeStretcher;
using mws::engine::SampleRateSel;
using mws::model::ModelId;
using mws::plugin::FxEngine;

constexpr double kHostRate = 48000.0;
constexpr int kMaxBlock = 512;

ParamSnapshot fxParams(ModelId model, double bandwidth, SampleRateSel fs,
                       bool character)
{
    ParamSnapshot p;
    p.model = model;
    p.bandwidth = bandwidth;
    p.sampleRateSel = fs;
    p.character = character;
    p.timeFactor = 100.0;
    return p;
}

// A deterministic input block (small ramp), reused every block.
std::vector<float> makeRamp(int n)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<float>((i % 101) - 50) / 50.0f;
    return v;
}

bool allFinite(const std::vector<float>& v)
{
    for (float s : v)
        if (!std::isfinite(s))
            return false;
    return true;
}
} // namespace

TEST_CASE("enginehost: FX reconfigure rebuilds the engine and re-reports latency",
          "[enginehost]")
{
    FxEngine fx;
    fx.prepare(kHostRate, kMaxBlock, /*channels=*/1,
               fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, true));
    REQUIRE(fx.isPrepared());

    // prepareToPlay reports unconditionally; the dirty flag is consumed there.
    REQUIRE_FALSE(fx.consumeLatencyDirty());
    const int latS1000 = fx.latencySamples();
    REQUIRE(latS1000 > 0);

    // An automatable-only change must NOT trigger a reconfiguration / re-report.
    auto sameConfig = fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, true);
    sameConfig.timeFactor = 350.0;
    sameConfig.cycleLen = 750;
    REQUIRE_FALSE(fx.requestReconfigure(sameConfig));
    REQUIRE_FALSE(fx.consumeLatencyDirty());

    // A model change (S1000 -> S950) IS latency-relevant: reconfigure + dirty.
    REQUIRE(fx.requestReconfigure(
        fxParams(ModelId::S950, 19.2, SampleRateSel::Fs44100, true)));
    REQUIRE(fx.consumeLatencyDirty());

    // A bandwidth change on the varclock model IS latency-relevant (it changes
    // the model rate, dsp-engine.md §7.4) so it re-reports — even though, under
    // the task-033 character-ON honest-PDC fix (review item 2a; FxEngine.h
    // DEVIATION + plan/backlog/053), the REPORTED value is the realized
    // read-head delay rather than the model-rate-scaled §7.4 formula: with no
    // host<->model resampler in the character-ON path, the cycle scaling and SRC
    // group delay the formula assumes are never realized.
    const int latS950BW192 = fx.latencySamples();
    REQUIRE(fx.requestReconfigure(
        fxParams(ModelId::S950, 3.0, SampleRateSel::Fs44100, true)));
    REQUIRE(fx.consumeLatencyDirty());
    const int latS950BW30 = fx.latencySamples();
    // The realized read-head delay (D = 2000 + fadeMax model samples, fed 1:1 as
    // host samples because nothing resamples) does NOT scale with the model rate,
    // so the two character-ON figures are equal — the honest PDC the path
    // actually applies. (Once 053's streaming chain resamples for real, the
    // model-rate scaling and SRC term ARE realized and this becomes a strict
    // inequality; the character-OFF magnitude relationship is pinned below.)
    REQUIRE(latS950BW30 == latS950BW192);

    // An FS change on a fixed-rate model is latency-relevant too.
    REQUIRE(fx.requestReconfigure(
        fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs22050, true)));
    REQUIRE(fx.consumeLatencyDirty());
}

TEST_CASE("enginehost: FX character-ON reports the realized PDC (review item 2a)",
          "[enginehost]")
{
    // The task-033 honest-PDC fix at the JUCE-free FX-engine level: with
    // CHARACTER ON and modelRate != hostRate the FX glue feeds host-rate audio
    // straight into the stretcher (no resampler exists yet — FxEngine.h
    // DEVIATION, follow-up plan/backlog/053). RealtimeStretcher::latencySamples()
    // is the dsp-engine.md §7.4 formula for the INTENDED streaming chain and
    // includes a host<->model SRC group-delay term + model-rate cycle scaling
    // that this path never realizes; reporting it would over-report PDC. So
    // FxEngine reports the scheduler's actually-realized read-head delay instead.
    FxEngine fx;
    const auto charOn =
        fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, /*character=*/true);
    fx.prepare(kHostRate, kMaxBlock, /*channels=*/1, charOn);

    // Reference engine prepared with the SAME inputs: the §7.4 formula vs the
    // realized read-head delay (they diverge here precisely because 44.1k != 48k).
    mws::engine::RealtimeStretcher ref;
    ref.prepare(kHostRate, kMaxBlock, /*channels=*/1, charOn);
    REQUIRE(ref.modelRate() < kHostRate);                        // 44100 < 48000
    REQUIRE(ref.latencySamples() > ref.realizedDelaySamples());  // SRC term present
    REQUIRE(fx.latencySamples() == ref.realizedDelaySamples());  // honest PDC

    // Companion invariant: character OFF (modelRate == hostRate) has no SRC term,
    // so the realized delay equals the formula and the honest branch is a no-op
    // on the null-tested path.
    FxEngine fxOff;
    const auto charOff =
        fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, /*character=*/false);
    fxOff.prepare(kHostRate, kMaxBlock, /*channels=*/1, charOff);
    mws::engine::RealtimeStretcher refOff;
    refOff.prepare(kHostRate, kMaxBlock, /*channels=*/1, charOff);
    REQUIRE(refOff.latencySamples() == refOff.realizedDelaySamples());
    REQUIRE(fxOff.latencySamples() == refOff.latencySamples());
}

TEST_CASE("enginehost: FX reconfiguration handoff is race-free under processing",
          "[enginehost][tsan]")
{
    FxEngine fx;
    fx.prepare(kHostRate, kMaxBlock, /*channels=*/1,
               fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, true));

    const auto ramp = makeRamp(kMaxBlock);

    std::atomic<bool> done{ false };
    std::atomic<std::uint64_t> blocks{ 0 };
    std::atomic<std::uint64_t> nonFinite{ 0 };
    std::atomic<std::uint64_t> lcdPolls{ 0 };

    // Audio thread: process blocks in place forever, adopting whatever the
    // message thread has published. Never allocates, never frees.
    std::thread audio([&] {
        std::vector<float> work(static_cast<std::size_t>(kMaxBlock));
        while (!done.load(std::memory_order_acquire))
        {
            work = ramp;
            ConstAudioView in{ work.data(), work.size() };
            AudioView out{ work.data(), work.size() };
            ParamSnapshot p; // snapshot taken per block (automatable values vary)
            p.timeFactor = 100.0 + static_cast<double>(blocks.load() % 400);
            fx.processBlock(&in, &out, 1, p, RealtimeStretcher::TransportInfo{});
            if (!allFinite(work))
                nonFinite.fetch_add(1, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Message thread: storm of latency-relevant reconfigurations (model /
    // bandwidth / FS / character toggles) — exactly the handoff under test.
    std::thread message([&] {
        const ParamSnapshot configs[] = {
            fxParams(ModelId::S950, 19.2, SampleRateSel::Fs44100, true),
            fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs22050, true),
            fxParams(ModelId::S1100, 19.2, SampleRateSel::Fs44100, true),
            fxParams(ModelId::S900, 16.0, SampleRateSel::Fs44100, true),
            fxParams(ModelId::S950, 3.0, SampleRateSel::Fs44100, false),
            fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, true),
        };
        // 300 latency-relevant reconfigurations: each rebuilds and publishes a
        // fresh 30 s history ring against the running audio thread — enough to
        // exercise the adopt/retire handoff thoroughly while keeping the
        // (heavily instrumented) TSan run bounded.
        for (int i = 0; i < 300; ++i)
        {
            fx.requestReconfigure(configs[static_cast<std::size_t>(i) % 6]);
            (void) fx.consumeLatencyDirty();
            fx.collectGarbage(); // drain retired engines on the message thread
        }
    });

    // LCD-poll thread: the 30 Hz UI timer path (PluginEditor::pollEngine ->
    // EngineHost::fxClampActive/fxMonoSummed + modelRate) reads the live engine
    // state from the MESSAGE side WHILE the audio thread reassigns active_ and
    // mutates clampActive_ every block. This is the exact accessor pattern QA
    // finding F2 (HIGH) races: a plain shared_ptr read of active_ + a torn bool
    // read. Without the atomic snapshot handoff this thread makes the [tsan] run
    // report races; with it the run is clean (acceptance criterion).
    std::thread lcd([&] {
        bool sink = false;
        double rateSink = 0.0;
        while (!done.load(std::memory_order_acquire))
        {
            sink ^= fx.clampActive();
            sink ^= fx.monoSummed();
            rateSink += fx.modelRate();
            lcdPolls.fetch_add(1, std::memory_order_relaxed);
        }
        // Keep the reads observable so the optimizer cannot elide them.
        if (sink && rateSink < 0.0)
            nonFinite.fetch_add(1, std::memory_order_relaxed);
    });

    message.join();
    done.store(true, std::memory_order_release);
    audio.join();
    lcd.join();

    // Deterministic shutdown: drain the graveyard fully.
    fx.collectGarbage();

    REQUIRE(blocks.load() > 0);
    REQUIRE(lcdPolls.load() > 0);   // the LCD accessors actually ran concurrently
    REQUIRE(nonFinite.load() == 0); // no NaN/inf ever produced
    REQUIRE_FALSE(fx.hasGarbage()); // every retired engine was freed
}

TEST_CASE("enginehost: FX T=100% character OFF nulls against the reported latency",
          "[enginehost]")
{
    // testing-strategy.md §3.4a at the processor FX-engine level: with character
    // OFF the model rate IS the host rate, so the stretcher degenerates to a pure
    // delay of exactly the reported latency (RealtimeStretcher.h §56-63). The FX
    // engine wrapper must preserve that null end-to-end (T=100%, outTrim 0 dB).
    FxEngine fx;
    const auto params =
        fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, /*character=*/false);
    fx.prepare(kHostRate, kMaxBlock, /*channels=*/1, params);

    // Character OFF => model rate == host rate.
    REQUIRE(fx.modelRate() == kHostRate);
    const auto delay = static_cast<std::int64_t>(fx.latencySamples());
    REQUIRE(delay > 0);

    // Stream ~3 s of deterministic noise through the FX engine in varying block
    // sizes and collect the output.
    constexpr std::int64_t kFrames = 48000 * 3;
    std::vector<float> input(static_cast<std::size_t>(kFrames));
    std::uint32_t state = 0x1234567u;
    for (auto& s : input)
    {
        state = state * 1664525u + 1013904223u;
        s = static_cast<float>(static_cast<std::int32_t>(state)) * (0.5f / 2147483648.0f);
    }
    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);

    const int pattern[] = { 512, 128, 480, 64, 333, 512 };
    std::size_t patIdx = 0;
    std::int64_t pos = 0;
    std::vector<float> work(static_cast<std::size_t>(kMaxBlock));
    while (pos < kFrames)
    {
        const auto len = std::min<std::int64_t>(
            pattern[patIdx++ % 6], kFrames - pos);
        for (std::int64_t i = 0; i < len; ++i)
            work[static_cast<std::size_t>(i)] = input[static_cast<std::size_t>(pos + i)];

        ConstAudioView in{ work.data(), static_cast<std::size_t>(len) };
        AudioView out{ work.data(), static_cast<std::size_t>(len) };
        fx.processBlock(&in, &out, 1, params, RealtimeStretcher::TransportInfo{});

        for (std::int64_t i = 0; i < len; ++i)
            output[static_cast<std::size_t>(pos + i)] = work[static_cast<std::size_t>(i)];
        pos += len;
    }

    // Sample-exact null: output[i] == input[i - delay] (zeros before the delay).
    std::int64_t firstMismatch = -1;
    for (std::int64_t i = 0; i < kFrames; ++i)
    {
        const float expected =
            (i >= delay) ? input[static_cast<std::size_t>(i - delay)] : 0.0f;
        if (output[static_cast<std::size_t>(i)] != expected)
        {
            firstMismatch = i;
            break;
        }
    }
    REQUIRE(firstMismatch == -1);
}

TEST_CASE("enginehost: FX modelRate is exact across all supported rates (task 058)",
          "[enginehost]")
{
    // Task 058 shrinks LcdSnapshot::modelRate from double to float so the
    // published snapshot is <= 8 bytes and std::atomic<LcdSnapshot> is always
    // lock-free (the Linux/GCC build fix). Every supported model/host rate is an
    // integer <= 2^24, exactly representable in float, so the float round-trip
    // is LOSSLESS — the model-rate equality/null-test path
    // ((s.modelRate() < hostRate_) || (hostRate_ < s.modelRate())) must still
    // treat modelRate == hostRate as EQUAL on every supported rate. This pins
    // that exactness so the float change can never silently break the null path.

    // (a) Character OFF: modelRate == hostRate exactly (CharacterChain §8.4) for
    // every supported host sample rate. This is the contract null path (T=100%,
    // character OFF) — the equality must hold bit-for-bit through the float store.
    const double hostRates[] = { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 };
    for (double host : hostRates)
    {
        FxEngine fx;
        const auto off =
            fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, /*character=*/false);
        fx.prepare(host, kMaxBlock, /*channels=*/1, off);
        // Exact: float(host) widened back to double equals host for these
        // integer rates, so the null/equality path sees modelRate == hostRate.
        REQUIRE(fx.modelRate() == host);
    }

    // (b) Fixed-rate model rates (S1000 44.1k / 22.05k per sampleRateSel,
    // character ON) survive the float round-trip exactly.
    {
        FxEngine fx;
        fx.prepare(48000.0, kMaxBlock, /*channels=*/1,
                   fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, true));
        REQUIRE(fx.modelRate() == 44100.0);
        fx.requestReconfigure(
            fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs22050, true));
        // Force the audio thread to adopt the pending engine so the LCD snapshot
        // reflects the new rate.
        const auto ramp = makeRamp(kMaxBlock);
        std::vector<float> work = ramp;
        ConstAudioView in{ work.data(), work.size() };
        AudioView out{ work.data(), work.size() };
        fx.processBlock(&in, &out, 1,
                        fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs22050, true),
                        RealtimeStretcher::TransportInfo{});
        REQUIRE(fx.modelRate() == 22050.0);
    }

    // (c) The S950 variable clock (model rate = 2500 x bandwidth, character ON)
    // at integer-Hz bandwidths is exact too: 19.2 kHz -> 48000 Hz, 16.0 -> 40000,
    // 3.0 -> 7500. All <= 2^24 and integral, so the float store is lossless and
    // the realizedLatencyOf rate compare against the host stays exact.
    struct VarCase { double bw; double expected; };
    const VarCase varCases[] = { { 19.2, 48000.0 }, { 16.0, 40000.0 }, { 3.0, 7500.0 } };
    for (const auto& vc : varCases)
    {
        FxEngine fx;
        fx.prepare(48000.0, kMaxBlock, /*channels=*/1,
                   fxParams(ModelId::S950, vc.bw, SampleRateSel::Fs44100, true));
        REQUIRE(fx.modelRate() == vc.expected);
    }
}

TEST_CASE("enginehost: FX automation spam is allocation-free and never NaN/inf",
          "[enginehost]")
{
    // 1000 timeFactor changes across the FX clamp boundary (T<100% clamps to
    // 100% in FREE — ADR-006) over ~1 s of blocks: the audio-thread FX path must
    // allocate nothing (debug allocator hook) and never produce NaN/inf.
    FxEngine fx;
    const auto cfg = fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, false);
    fx.prepare(kHostRate, kMaxBlock, /*channels=*/1, cfg);

    const auto ramp = makeRamp(kMaxBlock);
    std::vector<float> work(static_cast<std::size_t>(kMaxBlock));

    // Warm up: adopt the published engine + touch every code path once so any
    // first-touch lazy allocation is out of the way before we count.
    {
        work = ramp;
        ConstAudioView in{ work.data(), work.size() };
        AudioView out{ work.data(), work.size() };
        fx.processBlock(&in, &out, 1, cfg, RealtimeStretcher::TransportInfo{});
    }

    const auto before =
        mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    constexpr int kBlocks = 1000; // ~1 s of 48 kHz audio at ~48-frame steps
    bool anyNonFinite = false;
    for (int b = 0; b < kBlocks; ++b)
    {
        // Sweep T across the 25..2000 superset, crossing the 100% FREE clamp
        // boundary repeatedly (every block is a new timeFactor — automation spam).
        ParamSnapshot p = cfg;
        p.timeFactor = 25.0 + static_cast<double>((b * 37) % 1975);

        work = ramp;
        ConstAudioView in{ work.data(), work.size() };
        AudioView out{ work.data(), work.size() };
        fx.processBlock(&in, &out, 1, p, RealtimeStretcher::TransportInfo{});

        if (!allFinite(work))
            anyNonFinite = true;
    }

    const auto after =
        mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    REQUIRE(after == before); // no allocation on the FX audio-thread path
    REQUIRE_FALSE(anyNonFinite);
}

TEST_CASE("enginehost: FX outTrim scales output; 0 dB stays bit-exact", "[enginehost]")
{
    // outTrim is the last FX stage. At 0 dB the path is bit-exact (preserves the
    // null); at +6 dB it scales linearly.
    FxEngine fx;
    auto cfg = fxParams(ModelId::S1000, 19.2, SampleRateSel::Fs44100, false);
    fx.prepare(kHostRate, kMaxBlock, /*channels=*/1, cfg);

    const auto delay = static_cast<std::int64_t>(fx.latencySamples());

    // Push a constant 0.25 well past the latency so a steady value emerges.
    cfg.timeFactor = 100.0;
    cfg.outTrim = 6.0; // +6 dB ~= x1.9953
    const float gain = std::pow(10.0f, 6.0f / 20.0f);

    const std::int64_t total = delay + 4 * kMaxBlock;
    std::vector<float> work(static_cast<std::size_t>(kMaxBlock));
    float lastOut = 0.0f;
    std::int64_t pos = 0;
    while (pos < total)
    {
        const auto len = std::min<std::int64_t>(kMaxBlock, total - pos);
        for (std::int64_t i = 0; i < len; ++i)
            work[static_cast<std::size_t>(i)] = 0.25f;
        ConstAudioView in{ work.data(), static_cast<std::size_t>(len) };
        AudioView out{ work.data(), static_cast<std::size_t>(len) };
        fx.processBlock(&in, &out, 1, cfg, RealtimeStretcher::TransportInfo{});
        lastOut = work[static_cast<std::size_t>(len - 1)];
        pos += len;
    }

    // After the latency has elapsed, a steady 0.25 in becomes 0.25 * gain out.
    REQUIRE(lastOut == Catch::Approx(0.25f * gain).margin(1.0e-5));
}
