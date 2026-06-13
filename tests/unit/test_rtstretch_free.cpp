// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// RealtimeStretcher FREE-mode contract tests (plan/backlog/022) — the ADR-006
// causality contract made executable BEFORE the implementation exists (TDD is
// mandatory for this task). Spec sources:
//   - plan/decisions/006-fx-vs-sample-mode.md (contract table + latency terms)
//   - docs/design/dsp-engine.md §3.5 (engine summary), §6 (S900 FX rate
//     clamp), §7.4 (latency formula)
//   - docs/design/architecture.md §5.2 (FREE column; 30 s history (PI);
//     stereo = dual-mono with ONE shared hop schedule)
//   - docs/design/testing-strategy.md §3 items 4a/4c(FREE)/4d(FREE)/4e and
//     §3.5 (FX stereo coherence half)
//
// Test-case names begin with "rtstretch" so `ctest -R rtstretch` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mws/core/Resampler.h"
#include "mws/engine/RealtimeStretcher.h"
#include "mws/model/CharacterChain.h"
#include "mws/model/ModelSpec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// Allocation accounting (acceptance criterion: `process()` does not allocate
// after `prepare()`): the test binary's single global operator-new
// replacement lives in test_butterworth.cpp; this test snapshots its shared
// counter around the streaming region under test.
#include "TestAllocationCounter.h"

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::core::ConstAudioView;
using mws::core::SincResampler;
using mws::engine::HopMode;
using mws::engine::ParamSnapshot;
using mws::engine::RealtimeStretcher;
using mws::engine::SampleRateSel;
using mws::model::CharacterChain;
using mws::model::ModelId;
using mws::stretch::CyclicEngine;
using mws::stretch::GrainLaunch;
using mws::stretch::GrainLaunchObserver;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Deterministic signal generators (testing-strategy.md §3.6 — fixed content,
// no platform-dependent randomness; mirrors test_cyclic_properties.cpp).
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

AudioBuffer makeSine(std::int64_t numFrames, double freqHz, double sampleRate,
                     float amplitude = 0.5f)
{
    AudioBuffer buffer(1, static_cast<std::size_t>(numFrames));
    auto view = buffer.channel(0);
    for (std::int64_t n = 0; n < numFrames; ++n)
        view[static_cast<std::size_t>(n)] = amplitude
            * static_cast<float>(
                std::sin(2.0 * kPi * freqHz * static_cast<double>(n) / sampleRate));
    return buffer;
}

bool bitIdentical(ConstAudioView a, ConstAudioView b)
{
    return a.size() == b.size()
           && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// ---------------------------------------------------------------------------
// Streaming harness: feeds `in` through `process` in blocks following
// `blockPattern` (cycled; clipped at the stream end) and collects the output.
// ---------------------------------------------------------------------------
AudioBuffer streamThrough(RealtimeStretcher& rt, const AudioBuffer& in,
                          const std::vector<std::size_t>& blockPattern)
{
    const std::size_t channels = in.numChannels();
    const std::size_t frames = in.numFrames();
    AudioBuffer out(channels, frames);

    std::vector<ConstAudioView> ins(channels);
    std::vector<AudioView> outs(channels);

    std::size_t pos = 0;
    std::size_t blockIdx = 0;
    while (pos < frames)
    {
        const std::size_t len =
            std::min(blockPattern[blockIdx % blockPattern.size()], frames - pos);
        ++blockIdx;
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            ins[ch] = ConstAudioView{ in.channel(ch).data() + pos, len };
            outs[ch] = AudioView{ out.channel(ch).data() + pos, len };
        }
        rt.process(ins.data(), outs.data(), channels);
        pos += len;
    }
    return out;
}

/// First index where `out` differs from `in` delayed by `delay` (zeros before
/// the delay elapses), or -1 when the null holds sample-exactly.
std::int64_t firstNullMismatch(ConstAudioView in, ConstAudioView out,
                               std::int64_t delay)
{
    const auto n = static_cast<std::int64_t>(out.size());
    for (std::int64_t i = 0; i < n; ++i)
    {
        const float expected =
            (i >= delay) ? in[static_cast<std::size_t>(i - delay)] : 0.0f;
        if (out[static_cast<std::size_t>(i)] != expected)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Independent latency-formula derivation (dsp-engine.md §7.4; ADR-006) —
// computed from the docs and the public Resampler/CharacterChain APIs, never
// from RealtimeStretcher's own code:
//   L = ceil(2000 * hostRate / modelRate)   worst-case cycle at host rate
//     + crossfadeLen                        fade of the worst-case cycle, host
//     + SRC group delay                     host<->model round trip, 0 when
//                                           modelRate == hostRate
// ---------------------------------------------------------------------------
int expectedLatencySamples(double hostRate, const ParamSnapshot& params)
{
    const double modelRate =
        CharacterChain::modelRateFor(params.model, params, hostRate);
    REQUIRE(modelRate > 0.0);
    const double r = hostRate / modelRate;

    // Worst-case cycle geometry from the §3.1 derivation (independent of the
    // engine, mirrors test_cyclic_classic.cpp / test_cyclic_properties.cpp).
    constexpr std::int64_t cMax = 2000;
    const double overlapF = static_cast<double>(CyclicEngine::SpliceCal{}.overlapF);
    std::int64_t ovStart = std::llround(static_cast<double>(cMax) * (1.0 - overlapF));
    ovStart = std::clamp<std::int64_t>(ovStart, 1, cMax);
    const std::int64_t fadeMax = std::max<std::int64_t>(1, cMax - ovStart);

    double latency = std::ceil(static_cast<double>(cMax) * r)
                     + std::ceil(static_cast<double>(fadeMax) * r);
    if (modelRate != hostRate)
        latency += std::ceil(
            SincResampler::groupDelaySamples(modelRate / hostRate) * r // ingest
            + SincResampler::groupDelaySamples(hostRate / modelRate)); // playback
    return static_cast<int>(latency);
}

ParamSnapshot freeParams(ModelId model, double timeFactor, int cycleLen,
                         HopMode hopMode, bool character)
{
    ParamSnapshot params;
    params.model = model;
    params.timeFactor = timeFactor;
    params.cycleLen = cycleLen;
    params.hopMode = hopMode;
    params.character = character;
    return params;
}

/// Captured grain launch + the engine's write position at observation time
/// (needed by the 4d exhaustion assertions).
struct ObservedLaunch
{
    std::int64_t outIndex = 0;
    double srcOffset = 0.0;
    std::int64_t written = 0;
};

} // namespace

// ===========================================================================
// 4a — T = 100%, character OFF: pure delay of exactly latencySamples(),
// sample-exact over >= 5 s of streamed blocks of varying sizes (ADR-006 row 1;
// testing-strategy.md §3.4a).
// ===========================================================================
TEST_CASE("rtstretch: free T=100% null — output equals input delayed by exactly "
          "latencySamples()",
          "[rtstretch][free]")
{
    constexpr double hostRate = 44100.0;
    constexpr std::int64_t numFrames = 5 * 44100 + 30000; // > 5 s
    const AudioBuffer in = makeNoise(numFrames, 0x5EEDF00Du);

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    RealtimeStretcher rt;
    rt.prepare(hostRate, 1024, 1,
               freeParams(ModelId::S1000, 100.0, 1000, mode, /*character*/ false));

    // Character OFF => model rate == host rate (CharacterChain §8.4), so the
    // reported latency and the internal model-domain delay coincide.
    REQUIRE(rt.modelRate() == hostRate);
    const int latency = rt.latencySamples();
    REQUIRE(latency > 0);
    REQUIRE(latency == expectedLatencySamples(hostRate, freeParams(
        ModelId::S1000, 100.0, 1000, mode, false)));
    REQUIRE_FALSE(rt.clampActive());

    // Varying block sizes: the schedule must be partition-invariant.
    const AudioBuffer out =
        streamThrough(rt, in, { 1, 7, 64, 333, 512, 1024, 91 });

    REQUIRE(firstNullMismatch(in.channel(0), out.channel(0), latency) == -1);
}

// ===========================================================================
// 4c — FREE clamp: T < 100% is engine-clamped to 100% (output identical) and
// clampActive() reports the LCD `FX MIN 100%` condition (ADR-006 row 3 FREE).
// ===========================================================================
TEST_CASE("rtstretch: free clamp — T<100% output identical to T=100% with "
          "clampActive()",
          "[rtstretch][free]")
{
    constexpr double hostRate = 44100.0;
    constexpr std::int64_t numFrames = 44100;
    const AudioBuffer in = makeNoise(numFrames, 0xCAFED00Du);
    const std::vector<std::size_t> blocks{ 512 };

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    RealtimeStretcher reference;
    reference.prepare(hostRate, 1024, 1,
                      freeParams(ModelId::S1000, 100.0, 1000, mode, false));
    const AudioBuffer refOut = streamThrough(reference, in, blocks);
    REQUIRE_FALSE(reference.clampActive());

    for (const double timeFactor : { 80.0, 25.0 })
    {
        CAPTURE(timeFactor);
        RealtimeStretcher clamped;
        clamped.prepare(hostRate, 1024, 1,
                        freeParams(ModelId::S1000, timeFactor, 1000, mode, false));
        REQUIRE(clamped.clampActive());
        // Latency is independent of timeFactor (automation never changes PDC).
        REQUIRE(clamped.latencySamples() == reference.latencySamples());

        const AudioBuffer out = streamThrough(clamped, in, blocks);
        REQUIRE(bitIdentical(refOut.channel(0), out.channel(0)));
    }
}

// ===========================================================================
// 4d — FREE resync: with T = 400% the read head lags 1 - 100/T per output
// sample; when the lag exhausts the 30 s history the read head jumps to
// writePos - latency, and ONLY at a grain boundary, ONLY at exhaustion
// (ADR-006 row 2 FREE). Instrumented via the task-012 schedule hook.
// ===========================================================================
TEST_CASE("rtstretch: free resync — T=400% jumps to writePos-latency only at a "
          "grain boundary at history exhaustion",
          "[rtstretch][free]")
{
    // Low host rate keeps the 30 s history (and the drive to exhaustion)
    // cheap; character OFF makes model rate == host rate.
    constexpr double hostRate = 8000.0;
    constexpr int cycleLen = 1000;
    constexpr double timeFactor = 400.0;
    constexpr std::size_t blockLen = 512;

    // Independent §3.1 hop derivation (CLASSIC, integer %):
    // ovStart = round(1000 * 0.8) = 800 = hop_out; fadeLen = 200;
    // hop_in = round(hop_out * 100 / 400) = 200.
    constexpr std::int64_t hopOut = 800;
    constexpr std::int64_t fadeLen = 200;
    constexpr std::int64_t hopIn = 200;

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               freeParams(ModelId::S1000, timeFactor, cycleLen, HopMode::Classic,
                          false));
    const auto latency = static_cast<std::int64_t>(rt.latencySamples());
    const std::int64_t history = rt.historyLengthSamples();
    REQUIRE(history == static_cast<std::int64_t>(
                std::ceil(RealtimeStretcher::kHistorySeconds * hostRate)));

    std::vector<ObservedLaunch> launches;
    launches.reserve(1 << 14);
    const GrainLaunchObserver observer = [&](const GrainLaunch& launch)
    { launches.push_back({ launch.outIndex, launch.srcOffset, rt.samplesWritten() }); };
    rt.setGrainLaunchObserver(&observer);

    // Lag grows at (1 - 100/T) = 0.75 per output sample: exhaustion of the
    // 240000-sample history needs ~320 k samples; drive 400 k so at least one
    // resync must occur, with margin.
    constexpr std::int64_t numFrames = 400000;
    const AudioBuffer in = makeNoise(numFrames, 0x0D15EA5Eu);
    (void) streamThrough(rt, in, { blockLen });
    rt.setGrainLaunchObserver(nullptr);

    REQUIRE(launches.size() > 2);
    REQUIRE(launches[0].outIndex == 0);

    std::size_t jumpCount = 0;
    for (std::size_t i = 1; i < launches.size(); ++i)
    {
        CAPTURE(i, launches[i].outIndex, launches[i].srcOffset);
        const std::int64_t outDelta = launches[i].outIndex - launches[i - 1].outIndex;
        const double offDelta = launches[i].srcOffset - launches[i - 1].srcOffset;

        if (offDelta == static_cast<double>(hopIn))
        {
            // Normal grain launch: locked to the hop grid.
            REQUIRE(outDelta == hopOut);
            continue;
        }

        // Anything else must be THE documented resync jump:
        ++jumpCount;
        // (a) only at a grain boundary: the swap retiring the previous grain
        //     happens exactly fadeLen output samples after its launch.
        REQUIRE(outDelta == fadeLen);
        // (b) lands exactly at writePos - latency (model domain == host
        //     domain here, character OFF).
        REQUIRE(launches[i].srcOffset
                == static_cast<double>(launches[i].written - latency));
        // (c) only at exhaustion: the abandoned read position had actually
        //     fallen to the history bound (within the engine's resync margin).
        REQUIRE(static_cast<double>(launches[i].written - history)
                    - launches[i - 1].srcOffset
                > -2.0 * static_cast<double>(latency));
        // (d) the jump moves the read head FORWARD toward the write head.
        REQUIRE(offDelta > static_cast<double>(hopIn));
    }
    REQUIRE(jumpCount >= 1);
}

// ===========================================================================
// 4e — latency formula: `latencySamples()` matches
// ceil(2000 * hostRate / modelRate) + crossfadeLen + SRC group delay for each
// model, S950 bandwidth 3.0/19.2, S1000 FS 22.05 (dsp-engine.md §7.4), and
// changes only with model/bandwidth/FS — never with cycleLen/timeFactor.
// ===========================================================================
TEST_CASE("rtstretch: free latency — formula matches per model, bandwidth and FS",
          "[rtstretch][free]")
{
    constexpr double hostRate = 48000.0;

    struct Case
    {
        const char* name;
        ModelId model;
        double bandwidth;
        SampleRateSel fs;
        double expectedModelRate;
    };
    const Case cases[] = {
        { "S1000 FS 44.1", ModelId::S1000, 19.2, SampleRateSel::Fs44100, 44100.0 },
        { "S1000 FS 22.05", ModelId::S1000, 19.2, SampleRateSel::Fs22050, 22050.0 },
        { "S1100 FS 44.1", ModelId::S1100, 19.2, SampleRateSel::Fs44100, 44100.0 },
        { "S950 BW 19.2", ModelId::S950, 19.2, SampleRateSel::Fs44100, 48000.0 },
        { "S950 BW 3.0", ModelId::S950, 3.0, SampleRateSel::Fs44100, 7500.0 },
        { "S900 BW max(16)", ModelId::S900, 19.2, SampleRateSel::Fs44100, 40000.0 },
    };

    for (const Case& c : cases)
    {
        CAPTURE(c.name);
        ParamSnapshot params = freeParams(c.model, 100.0, 1000, HopMode::Classic,
                                          /*character*/ true);
        params.bandwidth = c.bandwidth;
        params.sampleRateSel = c.fs;

        RealtimeStretcher rt;
        rt.prepare(hostRate, 512, 2, params);
        REQUIRE(rt.modelRate() == c.expectedModelRate);
        REQUIRE(rt.latencySamples() == expectedLatencySamples(hostRate, params));

        // Latency never changes with the automatable stretch parameters.
        ParamSnapshot automated = params;
        automated.timeFactor = 2000.0;
        automated.cycleLen = 20;
        RealtimeStretcher rt2;
        rt2.prepare(hostRate, 512, 2, automated);
        REQUIRE(rt2.latencySamples() == rt.latencySamples());
    }

    // The documented extreme (architecture.md §5.2): S950 bandwidth 3.0 kHz
    // (7.5 kHz model rate) at a 48 kHz host — the worst-case-cycle term alone
    // is ceil(2000 * 48000 / 7500) = 12800 samples.
    REQUIRE(static_cast<int>(std::ceil(2000.0 * hostRate / 7500.0)) == 12800);
}

// ===========================================================================
// Allocation freedom: after prepare(), process() never allocates — across
// grain launches, swaps, a grain-boundary parameter change and an observer
// left unset (acceptance criterion; architecture.md §4 audio-thread rules).
// ===========================================================================
TEST_CASE("rtstretch: free process is allocation-free after prepare",
          "[rtstretch][free]")
{
    constexpr double hostRate = 8000.0;
    constexpr std::size_t blockLen = 256;
    constexpr std::int64_t numFrames = 16000;

    const AudioBuffer in = makeNoise(numFrames, 0xA110C8EDu);
    AudioBuffer out(2, numFrames);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 2,
               freeParams(ModelId::S1000, 250.0, 600, HopMode::Classic, false));

    ParamSnapshot newParams =
        freeParams(ModelId::S1000, 400.0, 600, HopMode::Classic, false);

    ConstAudioView ins[2];
    AudioView outs[2];

    const std::size_t allocationsBefore =
        mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    std::size_t pos = 0;
    bool paramsChanged = false;
    while (pos < static_cast<std::size_t>(numFrames))
    {
        const std::size_t len =
            std::min(blockLen, static_cast<std::size_t>(numFrames) - pos);
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            ins[ch] = ConstAudioView{ in.channel(0).data() + pos, len };
            outs[ch] = AudioView{ out.channel(ch).data() + pos, len };
        }
        rt.process(ins, outs, 2);
        pos += len;
        if (!paramsChanged && pos >= 8000)
        {
            rt.setParams(newParams); // applied at a grain boundary, no alloc
            paramsChanged = true;
        }
    }

    const std::size_t allocationsAfter =
        mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    REQUIRE(paramsChanged);
    REQUIRE(allocationsAfter == allocationsBefore);
}

// ===========================================================================
// FX stereo coherence (testing-strategy.md §3.5, FX half): dual-mono with ONE
// shared grain/hop schedule (architecture.md §5.2). Identical params +
// identical content => per-channel identical output; differing content => the
// grain-launch schedule is unchanged (content-blind, shared across channels).
// ===========================================================================
TEST_CASE("rtstretch: free stereo coherence — identical content gives identical "
          "channels, schedule is content-blind",
          "[rtstretch][free]")
{
    constexpr double hostRate = 44100.0;
    constexpr std::int64_t numFrames = 44100;
    constexpr int cycleLen = 1000;
    constexpr double timeFactor = 300.0;
    const std::vector<std::size_t> blocks{ 480, 37, 1024 };

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    CAPTURE(mode == HopMode::Classic ? "CLASSIC" : "REVISED");

    const AudioBuffer sine = makeSine(numFrames, 440.0, hostRate);
    const AudioBuffer noise = makeNoise(numFrames, 0xBADC0DE5u);

    // (1) Identical content on both channels through ONE process stream.
    {
        AudioBuffer stereo(2, numFrames);
        for (std::size_t ch = 0; ch < 2; ++ch)
            std::memcpy(stereo.channel(ch).data(), noise.channel(0).data(),
                        static_cast<std::size_t>(numFrames) * sizeof(float));

        RealtimeStretcher rt;
        rt.prepare(hostRate, 1024, 2,
                   freeParams(ModelId::S1000, timeFactor, cycleLen, mode, false));
        const AudioBuffer out = streamThrough(rt, stereo, blocks);
        REQUIRE(bitIdentical(out.channel(0), out.channel(1)));
    }

    // (2) Differing content: the launch schedule equals the schedule of a
    //     mono run with the same params/blocks — content-blind and shared.
    const auto capture = [&](const AudioBuffer& input)
    {
        std::vector<GrainLaunch> schedule;
        RealtimeStretcher rt;
        rt.prepare(hostRate, 1024, input.numChannels(),
                   freeParams(ModelId::S1000, timeFactor, cycleLen, mode, false));
        const GrainLaunchObserver observer = [&](const GrainLaunch& launch)
        { schedule.push_back(launch); };
        rt.setGrainLaunchObserver(&observer);
        (void) streamThrough(rt, input, blocks);
        return schedule;
    };

    AudioBuffer stereoDiffering(2, numFrames);
    std::memcpy(stereoDiffering.channel(0).data(), sine.channel(0).data(),
                static_cast<std::size_t>(numFrames) * sizeof(float));
    std::memcpy(stereoDiffering.channel(1).data(), noise.channel(0).data(),
                static_cast<std::size_t>(numFrames) * sizeof(float));

    const std::vector<GrainLaunch> stereoSchedule = capture(stereoDiffering);
    const std::vector<GrainLaunch> monoSchedule = capture(sine);

    REQUIRE(!stereoSchedule.empty());
    REQUIRE(stereoSchedule.size() == monoSchedule.size());
    for (std::size_t i = 0; i < stereoSchedule.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(stereoSchedule[i].outIndex == monoSchedule[i].outIndex);
        REQUIRE(std::memcmp(&stereoSchedule[i].srcOffset,
                            &monoSchedule[i].srcOffset, sizeof(double))
                == 0);
    }
}

// ===========================================================================
// S900 FX (dsp-engine.md §6; ADR-003): variable-rate ZOH read head with
// rate <= 1 enforced in FREE — T=100% nulls exactly, T<100% is clamped to the
// T=100% behavior with clampActive() raised.
// ===========================================================================
TEST_CASE("rtstretch: free S900 — T=100% null holds and varispeed rate is "
          "clamped <= 1",
          "[rtstretch][free]")
{
    constexpr double hostRate = 44100.0;
    constexpr std::int64_t numFrames = 5 * 44100 + 20000; // > 5 s
    const AudioBuffer in = makeNoise(numFrames, 0x900900u);

    RealtimeStretcher rt;
    rt.prepare(hostRate, 1024, 1,
               freeParams(ModelId::S900, 100.0, 1000, HopMode::Classic, false));
    const int latency = rt.latencySamples();
    REQUIRE(latency == expectedLatencySamples(
                hostRate,
                freeParams(ModelId::S900, 100.0, 1000, HopMode::Classic, false)));
    REQUIRE_FALSE(rt.clampActive());

    const AudioBuffer refOut = streamThrough(rt, in, { 1, 64, 480, 1024, 7 });
    REQUIRE(firstNullMismatch(in.channel(0), refOut.channel(0), latency) == -1);

    // rate = 100/T > 1 for every T < 100% — clamped to exactly 1 (a pure
    // delay), so the output is identical to the T=100% stream.
    for (const double timeFactor : { 80.0, 50.0, 25.0 })
    {
        CAPTURE(timeFactor);
        RealtimeStretcher clamped;
        clamped.prepare(
            hostRate, 1024, 1,
            freeParams(ModelId::S900, timeFactor, 1000, HopMode::Classic, false));
        REQUIRE(clamped.clampActive());
        const AudioBuffer out = streamThrough(clamped, in, { 1, 64, 480, 1024, 7 });
        REQUIRE(bitIdentical(refOut.channel(0), out.channel(0)));
    }

    // T > 100% actually slows playback (rate < 1): no longer a pure delay.
    RealtimeStretcher slowed;
    slowed.prepare(hostRate, 1024, 1,
                   freeParams(ModelId::S900, 200.0, 1000, HopMode::Classic, false));
    REQUIRE_FALSE(slowed.clampActive());
    const AudioBuffer slowOut = streamThrough(slowed, in, { 512 });
    REQUIRE(firstNullMismatch(in.channel(0), slowOut.channel(0), latency) != -1);
}

// ===========================================================================
// Parameter changes apply at grain boundaries only (ADR-006: no smoothing, no
// mid-grain morphing): after setParams the hop changes exactly once, the
// schedule stays on the hop grid throughout.
// ===========================================================================
TEST_CASE("rtstretch: free parameter changes apply at grain boundaries only",
          "[rtstretch][free]")
{
    constexpr double hostRate = 8000.0;
    constexpr int cycleLen = 1000;
    constexpr std::int64_t hopOut = 800; // independent §3.1 derivation
    constexpr std::int64_t hopInBefore = 400; // T=200%: round(800*100/200)
    constexpr std::int64_t hopInAfter = 200;  // T=400%: round(800*100/400)
    constexpr std::int64_t numFrames = 64000;
    constexpr std::size_t blockLen = 64;

    const AudioBuffer in = makeNoise(numFrames, 0xB0DA0B0Au);
    AudioBuffer out(1, numFrames);

    RealtimeStretcher rt;
    rt.prepare(hostRate, blockLen, 1,
               freeParams(ModelId::S1000, 200.0, cycleLen, HopMode::Classic, false));

    std::vector<GrainLaunch> launches;
    const GrainLaunchObserver observer = [&](const GrainLaunch& launch)
    { launches.push_back(launch); };
    rt.setGrainLaunchObserver(&observer);

    std::size_t pos = 0;
    bool changed = false;
    while (pos < static_cast<std::size_t>(numFrames))
    {
        const std::size_t len =
            std::min(blockLen, static_cast<std::size_t>(numFrames) - pos);
        ConstAudioView inView{ in.channel(0).data() + pos, len };
        AudioView outView{ out.channel(0).data() + pos, len };
        rt.process(&inView, &outView, 1);
        pos += len;
        if (!changed && pos >= 32000)
        {
            // Mid-stream (and mid-grain: 32000 = 40 * 800 is a launch point,
            // not a swap point) — must take effect at a boundary only.
            rt.setParams(freeParams(ModelId::S1000, 400.0, cycleLen,
                                    HopMode::Classic, false));
            changed = true;
        }
    }
    rt.setGrainLaunchObserver(nullptr);
    REQUIRE(changed);
    REQUIRE(launches.size() > 4);

    // Output spacing stays on the hop grid; the input hop takes exactly the
    // two legal values and switches exactly once (at a boundary, never back).
    std::size_t switchCount = 0;
    bool seenAfter = false;
    for (std::size_t i = 1; i < launches.size(); ++i)
    {
        CAPTURE(i, launches[i].outIndex, launches[i].srcOffset);
        REQUIRE(launches[i].outIndex - launches[i - 1].outIndex == hopOut);
        const double offDelta = launches[i].srcOffset - launches[i - 1].srcOffset;
        const bool isBefore = offDelta == static_cast<double>(hopInBefore);
        const bool isAfter = offDelta == static_cast<double>(hopInAfter);
        REQUIRE((isBefore || isAfter));
        if (isAfter && !seenAfter)
        {
            seenAfter = true;
            ++switchCount;
        }
        if (isBefore)
            REQUIRE_FALSE(seenAfter); // never switches back
    }
    REQUIRE(switchCount == 1);
}
