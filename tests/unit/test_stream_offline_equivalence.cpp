// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Stream/offline equivalence (plan/backlog/024) — THE hard test that the FX
// path and the authentic SAMPLE path share ONE scheduler: with parameters
// frozen from a known start state, RealtimeStretcher's streamed output equals
// OfflineRenderer's render of the same history, sample-for-sample over an
// explicitly defined comparable region.
//
// Spec sources:
//   - docs/design/testing-strategy.md §3 item 4b (the property statement),
//     §3.5 (render/FX stereo requirement)
//   - docs/design/architecture.md §4.1 / §5.2 (the equivalence demand)
//   - plan/decisions/006-fx-vs-sample-mode.md ("stream/offline equivalence is a
//     hard test"; option C "two thin front-ends over ONE grain scheduler")
//   - docs/design/dsp-engine.md §3.5 (equivalence test contract paragraph:
//     "with parameters frozen from a known start state, the streamed output
//     over a window equals the offline render of the same history
//     sample-for-sample"; host-rate / model-rate invariance: "the sound
//     matches the offline render at any host rate")
//
// CONFORMANCE DIRECTION (plan/backlog/024): if equivalence fails the fix
// belongs in 022 — the streaming front-end conforms to the offline reference,
// never the reverse (the goldens pin the offline path, task 026).
//
// =========================================================================
// THE ALIGNMENT DERIVATION (the trickiest part — DO NOT loosen alignment to
// "fix" a failure; a failure here is a real conformance bug in 022).
// =========================================================================
//
// Both front-ends drive the identical two-grain scheduler
// (stretch/detail/TwoGrainScheduler.h). With parameters frozen, the scheduler
// is a deterministic function of the OUTPUT index alone: at output index i it
// has launched the same number of grains, sits at the same in-grain phase, and
// applies the same crossfade fraction in BOTH front-ends. The ONLY difference
// is the source offset of grain A:
//
//   - OfflineRenderer / CyclicEngine starts grain A at source offset 0 at
//     output index 0 (CyclicEngine.h; the initial launch is reported at
//     {0, 0}).
//   - RealtimeStretcher starts the read head at -D, where D = delayModel_ =
//     2000 + fadeMax is the internal MODEL-domain read delay (RealtimeStretcher
//     prepare(): `sched_.reset(geom_, -delayModel_)`). D equals
//     latencySamples() exactly when character is OFF (model rate == host rate),
//     per the header contract "D = L when character is OFF".
//
// Therefore, with frozen params and BEFORE any history-exhaustion resync, the
// stream's grain-A offset at output index i is exactly the offline grain-A
// offset at the same i, shifted DOWN by D model samples:
//
//   off_stream(i) = off_offline(i) - D            (i identical on both sides)
//
// So the streamed output sample out_stream[i] reads history at
// off_offline(i) - D + phase(i). If we feed the OFFLINE renderer a source with
// exactly D zeros prepended (srcPadded = [0]*D ++ source), the offline grain at
// output i reads srcPadded[off_offline(i) + phase(i)] = source[off_offline(i)
// - D + phase(i)] — the SAME source sample the stream read. Hence, feeding the
// stream the raw `source` and the offline the D-zero-prepended `source`:
//
//   out_stream[i] == out_offline[i]   for every i in the comparable region,
//
// with NO output-index shift. (Empirically: bit-exact over the full streamed
// length for CLASSIC; identical for the S950 CLASSIC path; equal to <= 1e-6 for
// the REVISED float-interpolated path.)
//
// COMPARABLE REGION (dsp-engine.md §3.5 "over a window", explicit per the task):
// the stream is a 1:1 insert FX, so it emits exactly `kWindow` output samples
// for `kWindow` input samples; the offline render of the longer (D-prepended)
// source emits more. We compare indices [0, kWindow). Two causality bounds keep
// this region exact and resync-free with FROZEN params:
//   (1) No resync: the read head lags at rate (1 - 100/T) per output sample, so
//       after kWindow outputs it has lagged kWindow*(1 - 100/T). With T=300%
//       and kWindow = 60 000 the lag is 40 000 samples — far inside the 30 s
//       history ring, so the documented exhaustion jump never fires inside the
//       window (verified: samplesWritten() - read pos stays < historyLen()).
//   (2) No tail truncation: the last grain in the window reads source offset
//       ~ kWindow * 100/T = 20 000 < kWindow, i.e. strictly behind the input
//       end, so the stream never reads past its written input (which would
//       return pre-roll zeros and diverge from the offline content).
//
// HOST-RATE / MODEL-RATE INVARIANCE (dsp-engine.md §3.5): the engine runs at
// MODEL rate; cycleLen is in model-rate samples. The invariance claim "the
// sound matches the offline render at any host rate" is about the engine
// (model) domain — the playback SRC reconstruction (model->host) is a separate,
// continuous-block stage (CharacterChain::playback, task 019) whose zero->
// signal boundary phase differs between a one-shot offline resample and the
// FX-chain resample, so it is NOT sample-for-sample comparable by zero-padding.
// We therefore assert invariance in the model domain where it is exact and
// where 022 actually lives: at 44.1/48/96 kHz host rates the streamed
// model-domain output (ingest -> stretcher) equals the offline model-domain
// stretch (CyclicEngine on the D-prepended ingested history) to <= 1e-6. Both
// sides share the IDENTICAL ingest, so any divergence is a stretcher bug.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mws/core/Buffer.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/engine/Params.h"
#include "mws/engine/RealtimeStretcher.h"
#include "mws/model/CharacterChain.h"
#include "mws/model/ModelId.h"
#include "mws/stretch/CyclicEngine.h"
#include "mws/stretch/detail/TwoGrainScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::AudioView;
using mws::core::ConstAudioView;
using mws::engine::HopMode;
using mws::engine::Material;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::engine::RealtimeStretcher;
using mws::engine::SampleRateSel;
using mws::model::CharacterChain;
using mws::model::ModelId;
using mws::stretch::CyclicEngine;

// Internal model-domain read delay D = c + fadeMax for the worst-case cycle
// (RealtimeStretcher prepare(): delayModel_ = GrainGeometry::fromCycle(2000,
// overlapF).c + .fadeLen). Derived here from the SAME geometry, never copied
// from the engine's reported figure (the §3.5 latency formula is pinned in
// test_rtstretch_free.cpp; this test reuses the geometry derivation directly so
// a latency regression cannot silently align this test away).
std::int64_t internalReadDelayModel(float overlapF = CyclicEngine::SpliceCal{}.overlapF)
{
    const auto worst =
        mws::stretch::detail::GrainGeometry::fromCycle(2000, overlapF);
    return worst.c + worst.fadeLen; // 2000 + 400 = 2400 at the default cal
}

// Deterministic LCG noise (mirrors the other DSP tests; testing-strategy.md
// §3.6 — fixed content, no platform-dependent randomness). Per-channel
// decorrelated so the stereo case exercises genuinely differing content.
AudioBuffer makeNoise(std::int64_t numFrames, std::uint32_t seed,
                      std::size_t numChannels = 1, double sampleRate = 44100.0)
{
    AudioBuffer buffer(numChannels, static_cast<std::size_t>(numFrames));
    buffer.sampleRate = sampleRate;
    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        std::uint32_t state = seed + 0x9E3779B9u * static_cast<std::uint32_t>(ch);
        auto view = buffer.channel(ch);
        for (std::int64_t n = 0; n < numFrames; ++n)
        {
            state = state * 1664525u + 1013904223u;
            view[static_cast<std::size_t>(n)] =
                static_cast<float>(static_cast<std::int32_t>(state))
                * (0.5f / 2147483648.0f);
        }
    }
    return buffer;
}

ParamSnapshot equivParams(ModelId model, double timeFactor, int cycleLen,
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

// Streams `in` through `process` in `blockLen` chunks (multichannel, one shared
// schedule) and collects the streamed output (1:1 insert FX — `in.numFrames()`
// out for `in.numFrames()` in).
AudioBuffer streamThrough(RealtimeStretcher& rt, const AudioBuffer& in,
                          std::size_t blockLen)
{
    const std::size_t channels = in.numChannels();
    const std::size_t frames = in.numFrames();
    AudioBuffer out(channels, frames);

    std::vector<ConstAudioView> ins(channels);
    std::vector<AudioView> outs(channels);

    std::size_t pos = 0;
    while (pos < frames)
    {
        const std::size_t len = std::min(blockLen, frames - pos);
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

// Prepends `pad` zeros to every channel of `source` (the D-zero offline source
// of the alignment derivation), preserving the sample rate.
AudioBuffer prependZeros(const AudioBuffer& source, std::int64_t pad)
{
    const std::size_t channels = source.numChannels();
    const auto n = static_cast<std::int64_t>(source.numFrames());
    AudioBuffer padded(channels, static_cast<std::size_t>(n + pad));
    padded.sampleRate = source.sampleRate;
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
        const ConstAudioView src = source.channel(ch);
        AudioView dst = padded.channel(ch);
        for (std::int64_t i = 0; i < n; ++i)
            dst[static_cast<std::size_t>(pad + i)] = src[static_cast<std::size_t>(i)];
    }
    return padded;
}

// Max |a[i] - b[i]| over [0, window) per channel; `a` may have more frames than
// the window (the offline render of the padded source) — only the comparable
// region is examined. Asserts both buffers cover the window.
double maxAbsErrorOverWindow(const AudioBuffer& a, const AudioBuffer& b,
                             std::int64_t window)
{
    REQUIRE(static_cast<std::int64_t>(a.numFrames()) >= window);
    REQUIRE(static_cast<std::int64_t>(b.numFrames()) >= window);
    REQUIRE(a.numChannels() == b.numChannels());
    double maxErr = 0.0;
    for (std::size_t ch = 0; ch < a.numChannels(); ++ch)
    {
        const ConstAudioView va = a.channel(ch);
        const ConstAudioView vb = b.channel(ch);
        for (std::int64_t i = 0; i < window; ++i)
            maxErr = std::max(maxErr,
                              std::fabs(static_cast<double>(va[static_cast<std::size_t>(i)])
                                        - static_cast<double>(vb[static_cast<std::size_t>(i)])));
    }
    return maxErr;
}

// Confirms the comparable-region causality bounds (no resync, no tail read past
// the written input) actually hold for the chosen (window, T) — so a future
// change to those constants cannot silently invalidate the region.
void assertWindowIsResyncFree(const RealtimeStretcher& rt, std::int64_t window,
                              double timeFactor)
{
    // (1) No exhaustion: total lag after the window < history ring length.
    const double lag = static_cast<double>(window) * (1.0 - 100.0 / timeFactor);
    REQUIRE(lag < static_cast<double>(rt.historyLengthSamples()));
    // (2) No tail truncation: the deepest source read in the window stays
    //     behind the written input.
    const double deepestRead = static_cast<double>(window) * (100.0 / timeFactor);
    REQUIRE(deepestRead < static_cast<double>(rt.samplesWritten()));
}

constexpr std::int64_t kWindow = 60000; // comparable region length (samples)
constexpr double kTimeFactor = 300.0;   // T=300% (FREE, > 100% so never clamped)
constexpr int kCycleLen = 1000;
constexpr std::size_t kBlock = 512; // streamed block size (partition is irrelevant)

} // namespace

// ===========================================================================
// CLASSIC + REVISED, character OFF (the engine-only equivalence — model rate ==
// host rate, no SRC, no quantize). CLASSIC is bit-exact (integer path);
// REVISED matches to <= 1e-6 (the float 2-point interpolation, identical
// arithmetic on both sides via lerpSamples, accumulates a sub-1e-7 rounding
// difference). testing-strategy.md §3 item 4b.
// ===========================================================================
TEST_CASE("equivalence: CLASSIC/REVISED stream equals offline over the comparable "
          "region (character OFF)",
          "[equivalence]")
{
    constexpr double hostRate = 44100.0;

    const auto mode = GENERATE(HopMode::Classic, HopMode::Revised);
    const bool classic = (mode == HopMode::Classic);
    CAPTURE(classic ? "CLASSIC" : "REVISED");

    const ParamSnapshot params =
        equivParams(ModelId::S1000, kTimeFactor, kCycleLen, mode, /*character*/ false);

    // --- streamed front-end: feed the raw source ---------------------------
    const AudioBuffer source = makeNoise(kWindow, 0x5EEDF00Du);
    RealtimeStretcher rt;
    rt.prepare(hostRate, 1024, 1, params);
    REQUIRE(rt.modelRate() == hostRate);       // character OFF => no SRC
    REQUIRE_FALSE(rt.clampActive());           // T=300% is never clamped
    const std::int64_t D = internalReadDelayModel();
    REQUIRE(static_cast<std::int64_t>(rt.latencySamples()) == D); // D == L here

    const AudioBuffer streamed = streamThrough(rt, source, kBlock);
    assertWindowIsResyncFree(rt, kWindow, kTimeFactor);

    // --- offline reference: render the D-zero-prepended source -------------
    const AudioBuffer padded = prependZeros(source, D);
    const OfflineRenderer renderer;
    const auto offline = renderer.render(padded, params);
    REQUIRE(offline.ok());

    const double err = maxAbsErrorOverWindow(offline.out, streamed, kWindow);
    if (classic)
        REQUIRE(err == 0.0); // integer path: bit-exact
    else
        REQUIRE(err <= 1e-6); // REVISED float-interpolation tolerance
}

// ===========================================================================
// S950 (POL2, fixed cycle), character OFF — the S950 stretch is the cyclic
// scheduler with S950 D-TIME mapping (dsp-engine.md §5); CLASSIC hop is the
// integer path, so the equivalence is bit-exact.
// ===========================================================================
TEST_CASE("equivalence: S950 stream equals offline over the comparable region "
          "(character OFF)",
          "[equivalence]")
{
    constexpr double hostRate = 44100.0;

    ParamSnapshot params =
        equivParams(ModelId::S950, kTimeFactor, kCycleLen, HopMode::Classic,
                    /*character*/ false);
    params.material = Material::Pol2; // fixed cycle (the classic drum-loop sound)

    const AudioBuffer source = makeNoise(kWindow, 0x595005u);
    RealtimeStretcher rt;
    rt.prepare(hostRate, 1024, 1, params);
    REQUIRE(rt.modelRate() == hostRate);
    REQUIRE_FALSE(rt.clampActive());
    const std::int64_t D = internalReadDelayModel();
    REQUIRE(static_cast<std::int64_t>(rt.latencySamples()) == D);

    const AudioBuffer streamed = streamThrough(rt, source, kBlock);
    assertWindowIsResyncFree(rt, kWindow, kTimeFactor);

    const AudioBuffer padded = prependZeros(source, D);
    const OfflineRenderer renderer;
    const auto offline = renderer.render(padded, params);
    REQUIRE(offline.ok());
    REQUIRE_FALSE(offline.info.monoSummed); // mono source: no sum happens

    REQUIRE(maxAbsErrorOverWindow(offline.out, streamed, kWindow) == 0.0);
}

// ===========================================================================
// Character ON at the matching rate (host == model == 44.1 kHz): ingest and
// playback are rate-PRESERVING (no SRC group delay) but DO quantize to the
// model lattice. Replicating the task-033 FX chain (ingest -> stretcher ->
// playback) must reproduce the full offline render of the D-prepended source,
// sample-for-sample to <= 1e-6 (float stages: ingest/playback reconstruction).
// ===========================================================================
TEST_CASE("equivalence: character ON at host==model rate — full FX pass equals "
          "offline",
          "[equivalence]")
{
    constexpr double hostRate = 44100.0;

    ParamSnapshot params =
        equivParams(ModelId::S1000, kTimeFactor, kCycleLen, HopMode::Classic,
                    /*character*/ true);
    params.sampleRateSel = SampleRateSel::Fs44100; // model rate 44.1k == host

    const AudioBuffer source = makeNoise(kWindow, 0xC4A0Eu);

    // FX pass (the task-033 wiring, replicated here): ingest at model rate ->
    // stream the model-domain audio -> playback back to host rate.
    const auto ingested = CharacterChain::ingest(source, params.model, params);
    RealtimeStretcher rt;
    rt.prepare(hostRate, 2048, ingested.audio.numChannels(), params);
    REQUIRE(rt.modelRate() == hostRate); // FS44.1 at a 44.1k host: no SRC
    const std::int64_t D = internalReadDelayModel();
    REQUIRE(static_cast<std::int64_t>(rt.latencySamples()) == D);

    AudioBuffer streamedModel = streamThrough(rt, ingested.audio, kBlock);
    streamedModel.sampleRate = rt.modelRate();
    const AudioBuffer streamed =
        CharacterChain::playback(streamedModel, params.model, params, 1.0, hostRate);
    assertWindowIsResyncFree(rt, kWindow, kTimeFactor);

    const AudioBuffer padded = prependZeros(source, D);
    const OfflineRenderer renderer;
    const auto offline = renderer.render(padded, params);
    REQUIRE(offline.ok());

    REQUIRE(maxAbsErrorOverWindow(offline.out, streamed, kWindow) <= 1e-6);
}

// ===========================================================================
// Host-rate matrix / model-rate invariance (dsp-engine.md §3.5: "the sound
// matches the offline render at any host rate"). The engine runs at MODEL rate,
// so invariance is exact in the model domain — that is where 022 lives and
// where this asserts. CLASSIC, character ON, at 44.1/48/96 kHz host: the
// streamed model-domain output (ingest -> stretcher) equals the offline
// model-domain stretch (CyclicEngine on the D-prepended ingested history) to
// <= 1e-6. Both sides share the identical ingest, so a divergence is a
// stretcher conformance gap (see the header note on why the post-playback host
// domain is not zero-pad-comparable at host != model).
// ===========================================================================
TEST_CASE("equivalence: host-rate matrix — model-domain stretch is host-rate "
          "invariant",
          "[equivalence]")
{
    const double hostRate = GENERATE(44100.0, 48000.0, 96000.0);
    CAPTURE(hostRate);

    const ParamSnapshot params =
        equivParams(ModelId::S1000, kTimeFactor, kCycleLen, HopMode::Classic,
                    /*character*/ true);

    // The comparable region is in the MODEL OUTPUT domain here (the stream
    // emits nModel model samples for nModel ingested model samples). Downsampling
    // hosts (48/96 kHz -> 44.1 kHz model) ingest FEWER model samples than the
    // host-rate source, so the window must fit the smallest nModel: a 60 000
    // host-sample source ingests to ~27 563 model samples at 96 kHz. A 20 000
    // model-sample window fits all three rates and stays resync-free (lag
    // 20000*(1-100/300) ~ 13 333 < 30 s history; deepest read ~ 6 667 model
    // samples < nModel at every rate).
    constexpr std::int64_t kModelWindow = 20000;

    const AudioBuffer source = makeNoise(kWindow, 0xEE5Cu, 1, hostRate);
    const auto ingested = CharacterChain::ingest(source, params.model, params);
    const auto nModel = static_cast<std::int64_t>(ingested.audio.numFrames());
    REQUIRE(nModel > kModelWindow); // the window fits the ingested history

    RealtimeStretcher rt;
    rt.prepare(hostRate, 2048, 1, params);
    REQUIRE(rt.modelRate() == 44100.0); // FS44.1: model rate fixed at every host
    const std::int64_t D = internalReadDelayModel();

    const AudioBuffer streamedModel = streamThrough(rt, ingested.audio, kBlock);
    assertWindowIsResyncFree(rt, kModelWindow, kTimeFactor);

    // Offline model-domain reference: the SAME ingested history, D-prepended,
    // through the shared scheduler (CyclicEngine). The offline renderer's own
    // model-domain intermediate is not exposed, so we run the documented
    // model-rate stage directly here (CharacterChain::ingest is shared above).
    const AudioBuffer paddedModel = prependZeros(ingested.audio, D);
    const CyclicEngine cyclic;
    const AudioBuffer refModel =
        cyclic.render(paddedModel.channel(0), params.cycleLen, params.timeFactor,
                      params.hopMode);

    REQUIRE(maxAbsErrorOverWindow(refModel, streamedModel, kModelWindow) <= 1e-6);
}

// ===========================================================================
// Stereo over the FX pass (testing-strategy.md §3.5 render/FX requirement):
// differing per-channel content through ONE process stream equals the offline
// render per channel, and the hop schedule is identical across channels (the
// shared, content-blind schedule of architecture.md §5.2 — implied by per-
// channel equivalence to the SAME D-prepended offline render that itself uses
// one shared schedule). Character OFF keeps it the exact integer path.
// ===========================================================================
TEST_CASE("equivalence: stereo FX pass — stream equals offline per channel with "
          "an identical cross-channel schedule",
          "[equivalence]")
{
    constexpr double hostRate = 44100.0;

    const ParamSnapshot params =
        equivParams(ModelId::S1000, kTimeFactor, kCycleLen, HopMode::Classic,
                    /*character*/ false);

    // Decorrelated channels: a coherent (content-blind) schedule is the only
    // way both channels can match the same offline render.
    const AudioBuffer source = makeNoise(kWindow, 0x57E2E0u, /*channels*/ 2);

    RealtimeStretcher rt;
    rt.prepare(hostRate, 1024, 2, params);
    const std::int64_t D = internalReadDelayModel();
    REQUIRE(static_cast<std::int64_t>(rt.latencySamples()) == D);

    const AudioBuffer streamed = streamThrough(rt, source, kBlock);
    assertWindowIsResyncFree(rt, kWindow, kTimeFactor);

    const AudioBuffer padded = prependZeros(source, D);
    const OfflineRenderer renderer;
    const auto offline = renderer.render(padded, params);
    REQUIRE(offline.ok());
    REQUIRE(offline.out.numChannels() == 2); // S1000 preserves stereo

    // Per-channel sample-for-sample equality (the integer path: bit-exact).
    REQUIRE(maxAbsErrorOverWindow(offline.out, streamed, kWindow) == 0.0);

    // Cross-channel schedule identity: the streamed channels are produced by
    // ONE shared schedule, so for ANY i where the source channels happen to
    // carry the same value the outputs coincide — but the strong, content-
    // independent statement is that each channel matched the SAME offline
    // render (whose per-channel renders share one hop schedule by construction,
    // OfflineRenderer.cpp S1000 case). Equivalently: had the schedule differed
    // across channels, one of the two per-channel comparisons above would have
    // failed against the single-schedule offline reference. Pinned explicitly:
    // the two channels' streamed lengths are identical (1:1 insert FX).
    REQUIRE(streamed.numChannels() == 2);
    REQUIRE(streamed.numFrames() == static_cast<std::size_t>(kWindow));
}
