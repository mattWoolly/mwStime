// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Adversarial DSP QA — Wave 1 of the testing-strategy.md §7 adversarial fleet,
// made into committed, processor-level tests (plan/backlog/034b). Where task 024
// pins model-rate invariance at the *engine* (RealtimeStretcher/OfflineRenderer),
// this file pins the same contracts at the *processor* — driven through
// EngineHost's FX and SAMPLE audio paths exactly as PluginProcessor drives them:
//
//   1. NaN/inf/denormal hunt per model x mode: DC, +/-1 FS squares, and a
//      denormal-magnitude tail are streamed through both the FX path
//      (processFxBlock) and the SAMPLE GO->render->audition path
//      (OfflineRenderer + processSampleBlock). Output must be finite, and the
//      flush-to-zero CPU policy is verified (no denormal-driven slowdown).
//   2. Parameter extremes: cycle 20 @ 999% (S950 clamp), cycle 2000 @ 25%, the
//      transpose range ends, the documented 40 ms minimum input
//      (akaizer-analysis.md §2.1 input floor), and a zone of minimum length —
//      none crash, none produce NaN, and lengths stay schedule-derived.
//   3. Host sample-rate matrix: 44.1/48/88.2/96/192 kHz host rates with
//      CHARACTER OFF — the processor FX output is model-rate invariant (the same
//      underlying render within SRC tolerance). Extends the 024 engine-level
//      test to the plugin processor. (CHARACTER OFF is the path the invariant
//      holds on today — model rate == host rate; see the matrix case's header
//      for why the CHARACTER ON path is deferred to task 053.)
//   4. Render cap: 2000% on a synthetic long file is refused with the typed
//      NotEnoughMemory event and nothing is published (no over-cap allocation).
//
// A 30-minute streamed FX soak (flat memory, bounded CPU) lives in the
// `[.soak]`-tagged case at the foot of this file (HOW-TO-RUN is in that case's
// header comment). Catch2's `[.]` prefix only hides a case from the NO-FILTER
// default run; an explicit tag filter like `[qa-adversarial]` still matches it
// (the soak carries `[qa-adversarial]` too). So the `qa-adversarial`-labelled
// CTest entry uses the selector `[qa-adversarial]~[.soak]` to exclude the soak
// explicitly (see tests/plugin/CMakeLists.txt) — that is what keeps the labelled
// run to the 7 fast cases; the soak is invoked separately by tag.
//
// Context: docs/design/testing-strategy.md §7 Wave 1; docs/design/dsp-engine.md
// §3.4 (edge rules), §3.5 (host-rate processing / model-rate invariance);
// docs/research/akaizer-analysis.md §2.1 (40 ms input floor). Test-case names
// begin with "qa-adversarial" so `ctest -R qa-adversarial` selects them
// (plan/backlog/README.md test-selection rules); the CTest LABEL `qa-adversarial`
// is attached in tests/plugin/CMakeLists.txt.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "EngineHost.h"
#include "SamplePlayer.h"

#include "mws/core/AutoCorr.h"
#include "mws/engine/OfflineRenderer.h"
#include "mws/model/ModelSpec.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace
{
using mws::core::AudioBuffer;
using mws::engine::HopMode;
using mws::engine::OfflineRenderer;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::RealtimeStretcher;
using mws::engine::RenderResult;
using mws::engine::SampleRateSel;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::plugin::EngineHost;
using mws::plugin::RenderedSample;
using mws::plugin::RenderOutcome;
using mws::plugin::WorkerEvent;

constexpr double kPi = 3.14159265358979323846;

// The four v1 shipping models (the reserved S3000 slot has no behavior — ADR-004).
constexpr ModelId kModels[] = { ModelId::S900, ModelId::S950, ModelId::S1000,
                                ModelId::S1100 };

const char* modelName(ModelId m) noexcept
{
    switch (m)
    {
        case ModelId::S900: return "S900";
        case ModelId::S950: return "S950";
        case ModelId::S1000: return "S1000";
        case ModelId::S1100: return "S1100";
        case ModelId::S3000: return "S3000";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Finite-ness helpers. A denormal-magnitude value is finite (std::isfinite is
// true for denormals), so "no NaN/inf" and "denormals flushed" are distinct
// checks — the hunt asserts both.
// ---------------------------------------------------------------------------
bool allFinite(const float* p, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(p[i]))
            return false;
    return true;
}

bool bufferFinite(const AudioBuffer& b) noexcept
{
    for (std::size_t ch = 0; ch < b.numChannels(); ++ch)
        if (!allFinite(b.channel(ch).data(), b.numFrames()))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Adversarial input generators (deterministic). Each fills one channel.
//   DC: a full-scale constant (worst case for any integrator/filter).
//   square: +/-1 FS alternating runs (max slew, broadband — alias/click bait).
//   denormalTail: a short burst then a long tail at ~1e-30 magnitude (denormal
//     range for float32 is < ~1.18e-38; 1e-30 sits just above and decays into
//     it through filters/feedback — the classic denormal-CPU-cliff bait).
// ---------------------------------------------------------------------------
enum class Adversary { Dc, Square, DenormalTail };

const char* adversaryName(Adversary a) noexcept
{
    switch (a)
    {
        case Adversary::Dc: return "DC";
        case Adversary::Square: return "+/-1 FS square";
        case Adversary::DenormalTail: return "denormal tail";
    }
    return "?";
}

void fillAdversary(float* dst, std::size_t n, Adversary a, std::size_t phase) noexcept
{
    switch (a)
    {
        case Adversary::Dc:
            for (std::size_t i = 0; i < n; ++i)
                dst[i] = 1.0f;
            break;
        case Adversary::Square:
            for (std::size_t i = 0; i < n; ++i)
                dst[i] = ((phase + i) / 16u) % 2u == 0u ? 1.0f : -1.0f;
            break;
        case Adversary::DenormalTail:
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::size_t g = phase + i;
                // A loud transient in the first 64 frames, then a vanishing tail
                // that asymptotes into the denormal range.
                dst[i] = g < 64 ? (g % 2u == 0u ? 0.9f : -0.9f)
                                : static_cast<float>(1e-30 * std::exp(-0.0001 * static_cast<double>(g)));
            }
            break;
    }
}

AudioBuffer makeAdversaryBuffer(std::size_t channels, std::size_t frames, double rate,
                                Adversary a)
{
    AudioBuffer b(channels, frames);
    b.sampleRate = rate;
    for (std::size_t ch = 0; ch < channels; ++ch)
        fillAdversary(b.channel(ch).data(), frames, a,
                      /*phase=*/ch * 7u); // a small per-channel phase offset
    return b;
}

// ---------------------------------------------------------------------------
// FX-path driver: stream a buffer through EngineHost::processFxBlock block by
// block, in place (the processor's own pattern — test_enginehost_fx.cpp). The
// FX path mutates the block buffers; we keep a copy of the input first.
// Returns the captured output; `out.numChannels()/numFrames()` mirror `in`.
// ---------------------------------------------------------------------------
AudioBuffer runFxBlocks(EngineHost& host, const AudioBuffer& in, int block,
                        const ParamSnapshot& params,
                        const RealtimeStretcher::TransportInfo& transport = {})
{
    const std::size_t channels = in.numChannels();
    const std::int64_t frames = static_cast<std::int64_t>(in.numFrames());
    AudioBuffer out(channels, in.numFrames());
    out.sampleRate = in.sampleRate;

    std::vector<std::vector<float>> bufs(channels,
                                         std::vector<float>(static_cast<std::size_t>(block)));
    std::vector<float*> chans(channels);

    std::int64_t pos = 0;
    while (pos < frames)
    {
        const auto len = std::min<std::int64_t>(block, frames - pos);
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            const auto src = in.channel(ch);
            for (std::int64_t i = 0; i < len; ++i)
                bufs[ch][static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(pos + i)];
            chans[ch] = bufs[ch].data();
        }
        host.processFxBlock(chans.data(), static_cast<int>(channels),
                            static_cast<int>(len), params, transport);
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            auto dst = out.channel(ch);
            for (std::int64_t i = 0; i < len; ++i)
                dst[static_cast<std::size_t>(pos + i)] = bufs[ch][static_cast<std::size_t>(i)];
        }
        pos += len;
    }
    return out;
}

// SAMPLE-path driver: play the published render / source through
// processSampleBlock block by block (test_sample_mode.cpp pattern). Returns the
// captured interleaved-by-block output as a per-channel buffer.
AudioBuffer runSampleBlocks(EngineHost& host, std::int64_t frames, int block,
                            const ParamSnapshot& params, std::size_t channels)
{
    AudioBuffer out(channels, static_cast<std::size_t>(frames));
    std::vector<std::vector<float>> bufs(channels,
                                         std::vector<float>(static_cast<std::size_t>(block)));
    std::vector<float*> chans(channels);
    std::int64_t pos = 0;
    while (pos < frames)
    {
        const auto len = std::min<std::int64_t>(block, frames - pos);
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            std::fill(bufs[ch].begin(), bufs[ch].end(), 0.0f);
            chans[ch] = bufs[ch].data();
        }
        host.processSampleBlock(chans.data(), static_cast<int>(channels),
                                static_cast<int>(len), params);
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            auto dst = out.channel(ch);
            for (std::int64_t i = 0; i < len; ++i)
                dst[static_cast<std::size_t>(pos + i)] = bufs[ch][static_cast<std::size_t>(i)];
        }
        pos += len;
    }
    return out;
}

// Drain the worker FIFO until the given render request reports Finished.
struct DrainResult {
    bool sawFinished = false;
    RenderOutcome outcome = RenderOutcome::Completed;
};

DrainResult drainUntilFinished(EngineHost& host, std::uint64_t id, int timeoutMs = 10000)
{
    DrainResult r;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        WorkerEvent ev;
        bool any = false;
        while (host.popEvent(ev))
        {
            any = true;
            if (ev.requestId == id && ev.kind == WorkerEvent::Kind::Finished)
            {
                r.sawFinished = true;
                r.outcome = ev.outcome;
            }
        }
        host.collectGarbage();
        if (r.sawFinished)
            break;
        if (!any)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}

ParamSnapshot baseParams(ModelId model, PluginMode mode, bool character)
{
    ParamSnapshot p;
    p.model = model;
    p.pluginMode = mode;
    p.character = character;
    p.timeFactor = 100.0;
    p.cycleLen = 1000;
    p.hopMode = HopMode::Classic;
    // The S900 (varispeed model) has no stretch; the others run CYCLIC.
    return p;
}
} // namespace

// ===========================================================================
// 1. NaN / inf / denormal hunt — FX path, every model x adversary.
// ===========================================================================
TEST_CASE("qa-adversarial: FX path stays finite under DC / square / denormal-tail per model",
          "[qa-adversarial]")
{
    constexpr double kHostRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr std::size_t kFrames = 48000; // 1 s

    const ModelId model = GENERATE(from_range(std::begin(kModels), std::end(kModels)));
    const Adversary adv =
        GENERATE(Adversary::Dc, Adversary::Square, Adversary::DenormalTail);
    const bool character = GENERATE(false, true);

    INFO("model=" << modelName(model) << " adversary=" << adversaryName(adv)
                  << " character=" << (character ? "ON" : "OFF"));

    // T > 100% so the stretcher actually grain-schedules (FREE clamps T < 100%);
    // stereo so the dual-mono shared schedule and the S900/S950 mono-sum run.
    auto params = baseParams(model, PluginMode::Fx, character);
    params.timeFactor = 300.0;

    EngineHost host;
    const int latency = host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);
    REQUIRE(latency >= 0);

    const AudioBuffer in = makeAdversaryBuffer(2, kFrames, kHostRate, adv);
    const AudioBuffer out = runFxBlocks(host, in, kBlock, params);

    REQUIRE(bufferFinite(out));
    host.collectFxGarbage();
}

// ===========================================================================
// 1b. NaN / inf / denormal hunt — SAMPLE GO->render->audition path.
//     The offline render IS the authentic SAMPLE path (ADR-006); we render
//     each adversary, assert the render is finite, then audition it through
//     processSampleBlock and assert the played output is finite too.
// ===========================================================================
TEST_CASE("qa-adversarial: SAMPLE render+audition stays finite under adversarial input per model",
          "[qa-adversarial]")
{
    constexpr double kHostRate = 44100.0;
    constexpr int kBlock = 256;
    constexpr std::size_t kFrames = 22050; // 0.5 s

    const ModelId model = GENERATE(from_range(std::begin(kModels), std::end(kModels)));
    const Adversary adv =
        GENERATE(Adversary::Dc, Adversary::Square, Adversary::DenormalTail);
    const bool character = GENERATE(false, true);

    INFO("model=" << modelName(model) << " adversary=" << adversaryName(adv)
                  << " character=" << (character ? "ON" : "OFF"));

    auto params = baseParams(model, PluginMode::Sample, character);
    params.timeFactor = 200.0;

    auto source = std::make_shared<AudioBuffer>(
        makeAdversaryBuffer(2, kFrames, kHostRate, adv));

    // Direct offline render (off-thread material; the worker just wraps it).
    OfflineRenderer renderer;
    const RenderResult rr = renderer.render(*source, params);
    REQUIRE(rr.ok());
    REQUIRE(bufferFinite(rr.out));

    // Audition the render (B) through the processor SAMPLE path.
    EngineHost host;
    host.startWorker();
    host.setAuditionSource(source);
    host.prepareSample(kHostRate, kBlock, /*channels=*/2, params);

    const auto id = host.requestRender(params);
    REQUIRE(id != 0);
    REQUIRE(drainUntilFinished(host, id).outcome == RenderOutcome::Completed);

    host.startSamplePlayback();
    const AudioBuffer played =
        runSampleBlocks(host, static_cast<std::int64_t>(kFrames), kBlock, params, 2);
    REQUIRE(bufferFinite(played));

    host.collectGarbage();
    host.stopWorker();
}

// ===========================================================================
// 1c. Flush-to-zero policy: the denormal tail must not cost a CPU cliff.
//     We time the FX path on the denormal-tail adversary against the same path
//     on a loud (normal-magnitude) signal. A denormal slowdown shows up as a
//     multiple-times-slower run; with FTZ/DAZ (or denormals simply never
//     reaching the hot loops) the two are within a small factor. The bound is
//     generous (4x) so the test is not a flaky micro-benchmark — it catches the
//     order-of-magnitude denormal cliff, not normal jitter.
// ===========================================================================
TEST_CASE("qa-adversarial: FX denormal tail shows no denormal-driven CPU cliff",
          "[qa-adversarial]")
{
    constexpr double kHostRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr std::size_t kFrames = 48000 * 4; // 4 s — long enough to time

    auto params = baseParams(ModelId::S1000, PluginMode::Fx, /*character=*/false);
    params.timeFactor = 300.0;

    const AudioBuffer denorm =
        makeAdversaryBuffer(1, kFrames, kHostRate, Adversary::DenormalTail);
    // A "loud" reference: the denormal tail's first transient extended, i.e. a
    // normal-magnitude square — the same scheduler work, no denormal magnitudes.
    const AudioBuffer loud =
        makeAdversaryBuffer(1, kFrames, kHostRate, Adversary::Square);

    auto timeRun = [&](const AudioBuffer& in) {
        EngineHost host;
        host.prepareFx(kHostRate, kBlock, /*channels=*/1, params);
        // One warm-up to page in the history ring before timing.
        (void) runFxBlocks(host, in, kBlock, params);
        const auto t0 = std::chrono::steady_clock::now();
        const AudioBuffer out = runFxBlocks(host, in, kBlock, params);
        const auto t1 = std::chrono::steady_clock::now();
        REQUIRE(bufferFinite(out));
        host.collectFxGarbage();
        return std::chrono::duration<double>(t1 - t0).count();
    };

    const double tLoud = timeRun(loud);
    const double tDenorm = timeRun(denorm);

    INFO("loud=" << tLoud << "s  denormal-tail=" << tDenorm << "s  ratio="
                 << (tLoud > 0.0 ? tDenorm / tLoud : 0.0));
    // No denormal cliff: the denormal tail is not multiples slower than the
    // loud run (with a floor so a sub-millisecond loud run can't divide-explode).
    REQUIRE(tDenorm <= std::max(tLoud * 4.0, tLoud + 0.05));
}

// ===========================================================================
// 2. Parameter extremes. Each must not crash, must stay finite, and (for the
//    SAMPLE render path) the achieved length must be schedule-derived — i.e.
//    OfflineRenderer's own predictedOutputFrames, never round(N * T).
// ===========================================================================
TEST_CASE("qa-adversarial: extreme parameters render finite, schedule-derived lengths",
          "[qa-adversarial]")
{
    constexpr double kRate = 44100.0;

    struct Case {
        const char* name;
        ModelId model;
        double timeFactor;
        int cycleLen;
        double transpose;
        std::size_t frames;
    };

    // cycle 20 @ 999% is the S950 clamp ceiling; cycle 2000 @ 25% is the long-
    // cycle compression extreme; transpose at both range ends; the 40 ms input
    // is the akaizer-analysis.md §2.1 documented minimum-input floor (44100 *
    // 0.040 = 1764 frames).
    const Case c = GENERATE(
        Case{ "S950 cycle 20 @ 999%", ModelId::S950, 999.0, 20, 0.0, 44100 },
        Case{ "S1100 cycle 2000 @ 25%", ModelId::S1100, 25.0, 2000, 0.0, 44100 },
        Case{ "S1000 transpose +24 st", ModelId::S1000, 150.0, 1000, 24.0, 22050 },
        Case{ "S1000 transpose -24 st", ModelId::S1000, 150.0, 1000, -24.0, 22050 },
        Case{ "S1000 40 ms input floor", ModelId::S1000, 300.0, 1000, 0.0, 1764 },
        Case{ "S950 40 ms input @ 999%", ModelId::S950, 999.0, 1000, 0.0, 1764 });

    INFO("case=" << c.name);

    auto params = baseParams(c.model, PluginMode::Sample, /*character=*/true);
    params.timeFactor = c.timeFactor;
    params.cycleLen = c.cycleLen;
    params.transpose = c.transpose;

    AudioBuffer source(1, c.frames);
    source.sampleRate = kRate;
    {
        auto v = source.channel(0);
        for (std::size_t i = 0; i < c.frames; ++i)
            v[i] = static_cast<float>(
                0.7 * std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / kRate));
    }

    OfflineRenderer renderer;

    // The length the SCHEDULER predicts (model-rate frames), the figure render()
    // checks before allocating. The achieved render length (in source-rate
    // frames) must track this prediction — NOT round(N * timeFactor / 100).
    const std::int64_t predicted =
        renderer.predictedOutputFrames(static_cast<std::int64_t>(c.frames), kRate, params);
    REQUIRE(predicted > 0);

    const RenderResult rr = renderer.render(source, params);
    REQUIRE(rr.ok());
    REQUIRE(rr.out.numFrames() > 0);
    REQUIRE(bufferFinite(rr.out));

    // The naive (wrong) length the §3.4 invariant forbids — the achieved length
    // must NOT be round(N * T) (testing-strategy.md §3 item 2 "bad timing").
    const double clampedT =
        ModelSpec::get(c.model).clamp(params).timeFactor; // S950 -> 999 etc.
    const auto naive = static_cast<std::int64_t>(
        std::llround(static_cast<double>(c.frames) * clampedT / 100.0));

    // The achieved length tracks the schedule prediction (converted to source
    // rate by the renderer for fixed-rate models the model rate IS the source
    // rate; for varclock models the renderer reports source-rate frames in
    // RenderInfo). Assert it equals the renderer's own reported figure.
    REQUIRE(rr.info.outputFrames == static_cast<std::int64_t>(rr.out.numFrames()));

    // Where transpose is neutral and time-stretch dominates, the schedule length
    // diverges from the naive round(N*T) for CLASSIC (the whole point of the
    // "bad timing" property). We only assert divergence for the long-input
    // stretch cases (the 40 ms floor and tiny outputs can coincide with naive).
    if (c.frames >= 22050 && c.transpose == 0.0)
    {
        INFO("achieved=" << rr.out.numFrames() << " naive(round N*T)=" << naive
                         << " predicted(model-rate)=" << predicted);
        // Schedule-derived, not N*T: the achieved length is the engine schedule's,
        // which for CLASSIC quantizes away from the requested factor.
        REQUIRE(rr.out.numFrames() != static_cast<std::size_t>(naive));
    }
}

// A minimum-length zone slice must not crash the worker render path and must
// publish (or cleanly refuse) — exercised through EngineHost's zone render.
TEST_CASE("qa-adversarial: minimum-length zone slice renders without crashing",
          "[qa-adversarial]")
{
    constexpr double kRate = 44100.0;
    constexpr std::size_t kFrames = 44100; // 1 s source

    auto params = baseParams(ModelId::S1000, PluginMode::Sample, /*character=*/false);
    params.timeFactor = 200.0;

    auto source = std::make_shared<AudioBuffer>(1, kFrames);
    source->sampleRate = kRate;
    {
        auto v = source->channel(0);
        for (std::size_t i = 0; i < kFrames; ++i)
            v[i] = static_cast<float>(
                0.7 * std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / kRate));
    }

    EngineHost host;
    host.startWorker();
    host.setSource(source);

    // A vanishingly thin zone: one normalized step wide. The worker crops to it
    // (possibly an empty/near-empty slice) and must not crash; an empty slice is
    // a clean Completed-with-empty or a refusal, never a hang or NaN.
    const auto id = host.requestRender(params, EngineHost::Zone{ 0.5, 0.5000001 });
    REQUIRE(id != 0);
    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.sawFinished);
    REQUIRE((r.outcome == RenderOutcome::Completed
             || r.outcome == RenderOutcome::NotEnoughMemory));

    if (auto rendered = host.currentRender(); rendered && rendered->requestId == id)
        REQUIRE(bufferFinite(rendered->audio));

    host.collectGarbage();
    host.stopWorker();
}

// ===========================================================================
// 3. Host sample-rate matrix — model-rate invariance at the PROCESSOR level.
//    The FX path at 44.1/48/88.2/96/192 kHz host rates must render the SAME
//    underlying SOUND (dsp-engine.md §3.5: "the sound matches the offline
//    render at any host rate"). A cyclic timestretch preserves pitch, so the
//    audible, rate-independent invariant is the OUTPUT FUNDAMENTAL PERIOD: a
//    220 Hz tone in => a 220 Hz tone out at EVERY host rate (the dominant
//    autocorrelation period in seconds is rate-independent), together with a
//    rate-independent output LEVEL (no per-rate gain drift). This is the
//    robust processor-level expression of model-rate invariance — a raw
//    cross-rate waveform compare is meaningless because the FREE scheduler's
//    absolute sample grid, startup latency, and resync points are host-grid
//    dependent (024 pins the exact sample-for-sample equivalence at engine
//    level over a tightly-aligned comparable region).
//
//    CHARACTER OFF is the path this holds on today: there the model rate IS the
//    host rate (CharacterChain §8.4), the cyclic engine runs on a proportional
//    grid at every rate, and pitch is preserved exactly. (With CHARACTER ON the
//    FX path currently runs the stretcher at host rate against model-rate
//    geometry with NO resampler — the documented FxEngine.h deviation tracked by
//    task 053 — so the bit-faithful character path is the SAMPLE-mode offline
//    render meanwhile; 024 pins engine-level invariance exactly at host==model
//    rate. Flip `character` to ON here once 053 lands.)
// ===========================================================================
namespace
{
double rms(const float* p, std::size_t n) noexcept
{
    if (n == 0)
        return 0.0;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        acc += static_cast<double>(p[i]) * static_cast<double>(p[i]);
    return std::sqrt(acc / static_cast<double>(n));
}

// The fundamental period of `x` near a known frequency `centerHz`, in seconds,
// estimated by normalized autocorrelation over a lag band tightly bracketing
// `centerHz` (+/- `band` fraction). The band is intentionally narrow so the
// estimator locks onto the musical fundamental rather than the much stronger
// splice-comb periodicity of a cyclic timestretch (the comb sits at the grain
// hop, far below the pitch). Returns 0 on no clear peak.
double pitchPeriodSeconds(const std::vector<float>& x, double rate, double centerHz,
                          double band)
{
    if (x.size() < 64)
        return 0.0;
    const double loHz = centerHz * (1.0 + band); // higher freq => smaller lag
    const double hiHz = centerHz * (1.0 - band);
    const int lagMin = std::max(1, static_cast<int>(std::floor(rate / loHz)));
    const int lagMax = std::min(static_cast<int>(x.size()) - 1,
                                static_cast<int>(std::ceil(rate / hiHz)));
    if (lagMax <= lagMin)
        return 0.0;
    const mws::core::ConstAudioView view{ x.data(), x.size() };
    const auto lag = mws::core::AutoCorr::bestLag(view, lagMin, lagMax, /*threshold=*/0.1f);
    if (!lag)
        return 0.0;
    return static_cast<double>(*lag) / rate;
}
} // namespace

TEST_CASE("qa-adversarial: host sample-rate matrix — FX output is model-rate invariant",
          "[qa-adversarial]")
{
    constexpr int kBlock = 256;
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    constexpr double kSeconds = 2.0;       // long enough for a stable estimate
    constexpr double kSourceHz = 220.0;    // the input fundamental (pitch-preserved)

    auto sourceAt = [](double rate) {
        const auto n = static_cast<std::size_t>(std::llround(rate * kSeconds));
        AudioBuffer b(1, n);
        b.sampleRate = rate;
        auto v = b.channel(0);
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(i) / rate;
            v[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * kSourceHz * t)
                                      + 0.25 * std::sin(2.0 * kPi * 2.0 * kSourceHz * t)
                                      + 0.12 * std::sin(2.0 * kPi * 3.0 * kSourceHz * t));
        }
        return b;
    };

    // CHARACTER OFF: model rate == host rate at every host rate (see header).
    // cycleLen is in samples; to hold the TIME-DOMAIN configuration fixed across
    // host rates (so only the rate changes — the model-rate-invariance contract,
    // not the documented "cycle length is in samples so rate changes the sound"
    // artifact of testing-strategy §4) we scale cycleLen proportionally to the
    // host rate. The 44.1 kHz reference uses cycleLen 1000 (~22.7 ms).
    constexpr double kRefRate = 44100.0;
    constexpr int kRefCycleLen = 1000;
    auto paramsFor = [&](double rate) {
        auto p = baseParams(ModelId::S1000, PluginMode::Fx, /*character=*/false);
        p.timeFactor = 200.0; // pure time-stretch — pitch is unchanged
        p.cycleLen = std::clamp(
            static_cast<int>(std::llround(kRefCycleLen * rate / kRefRate)),
            mws::model::superset::kCycleLenMin, mws::model::superset::kCycleLenMax);
        return p;
    };

    // The reference output level (44.1 kHz) for the per-rate level invariant.
    double refRms = 0.0;
    {
        const auto params = paramsFor(kRefRate);
        const AudioBuffer in = sourceAt(kRefRate);
        EngineHost host;
        host.prepareFx(kRefRate, kBlock, /*channels=*/1, params);
        const AudioBuffer out = runFxBlocks(host, in, kBlock, params);
        REQUIRE(bufferFinite(out));
        refRms = rms(out.channel(0).data(), out.numFrames());
        host.collectFxGarbage();
    }
    REQUIRE(refRms > 0.0);

    const double expectedPeriod = 1.0 / kSourceHz; // ~4.545 ms

    std::vector<double> periods; // collected across rates for the cross-rate check
    for (double rate : rates)
    {
        INFO("host rate=" << rate);
        const auto params = paramsFor(rate);
        const AudioBuffer in = sourceAt(rate);

        EngineHost host;
        const int latency = host.prepareFx(rate, kBlock, /*channels=*/1, params);
        REQUIRE(latency >= 0);
        const AudioBuffer out = runFxBlocks(host, in, kBlock, params);
        REQUIRE(bufferFinite(out));

        // Skip the FX startup latency before estimating pitch (the read head
        // primes during the first `latency` samples).
        const std::size_t skip = std::min(static_cast<std::size_t>(latency),
                                          out.numFrames() / 4);
        std::vector<float> tail(out.channel(0).data() + skip,
                                out.channel(0).data() + out.numFrames());

        // PITCH preserved: the output fundamental period (seconds) tracks the
        // input's — a cyclic timestretch preserves pitch (model-rate invariance,
        // audibly). The +/-20% band brackets 220 Hz, below the splice-comb; the
        // 6% tolerance accounts for integer-lag resolution + the comb's pull on
        // the autocorrelation peak.
        const double period = pitchPeriodSeconds(tail, rate, kSourceHz, /*band=*/0.2);
        const double periodErrPct =
            std::abs(period - expectedPeriod) / expectedPeriod * 100.0;
        INFO("period=" << period << "s expected=" << expectedPeriod
                       << "s err=" << periodErrPct << "%");
        REQUIRE(period > 0.0);
        REQUIRE(periodErrPct < 6.0);
        periods.push_back(period);

        // LEVEL invariance: no per-rate gain drift.
        const double thisRms = rms(tail.data(), tail.size());
        const double rmsRatio = thisRms / refRms;
        INFO("rmsRatio=" << rmsRatio);
        REQUIRE(rmsRatio > 0.7);
        REQUIRE(rmsRatio < 1.43);
    }

    // CROSS-RATE INVARIANCE (the core claim): every rate's output pitch period
    // agrees with every other rate's within tight tolerance — the sound is the
    // same regardless of host rate.
    const double minP = *std::min_element(periods.begin(), periods.end());
    const double maxP = *std::max_element(periods.begin(), periods.end());
    const double spreadPct = (maxP - minP) / minP * 100.0;
    INFO("cross-rate pitch-period spread=" << spreadPct << "%");
    REQUIRE(spreadPct < 6.0);
}

// ===========================================================================
// 4. Render cap — 2000% on a synthetic long file, at the PROCESSOR level.
//    The worker must refuse with the typed NotEnoughMemory event and publish
//    nothing (no over-cap allocation). The 10-min model-rate cap means a
//    sufficiently long source x 2000% overruns it.
// ===========================================================================
TEST_CASE("qa-adversarial: render cap refuses 2000% on a long file with no over-cap allocation",
          "[qa-adversarial]")
{
    constexpr double kRate = 44100.0;
    // 40 s @ 2000% = 800 s > the 600 s (10-min) model-rate cap.
    constexpr std::size_t kFrames = static_cast<std::size_t>(44100.0 * 40.0);

    auto params = baseParams(ModelId::S1000, PluginMode::Sample, /*character=*/false);
    params.timeFactor = 2000.0;

    auto source = std::make_shared<AudioBuffer>(1, kFrames);
    source->sampleRate = kRate;
    {
        auto v = source->channel(0);
        for (std::size_t i = 0; i < kFrames; ++i)
            v[i] = static_cast<float>(
                0.5 * std::sin(2.0 * kPi * 110.0 * static_cast<double>(i) / kRate));
    }

    // The pure-math predictor exceeds the cap BEFORE any allocation (the figure
    // render() guards on).
    OfflineRenderer renderer;
    const std::int64_t predicted =
        renderer.predictedOutputFrames(static_cast<std::int64_t>(kFrames), kRate, params);
    const std::int64_t cap = OfflineRenderer::maxOutputFrames(
        ModelSpec::get(ModelId::S1000).modelRateHz(params));
    REQUIRE(predicted > cap);

    // A direct render refuses with the typed error and an empty buffer.
    const RenderResult rr = renderer.render(*source, params);
    REQUIRE_FALSE(rr.ok());
    REQUIRE(rr.error == mws::engine::RenderError::NotEnoughMemory);
    REQUIRE(rr.out.numFrames() == 0); // nothing allocated past the cap

    // The processor worker path surfaces the same as a typed event; nothing new
    // is published.
    EngineHost host;
    host.startWorker();
    host.setSource(source);
    const auto id = host.requestRender(params);
    REQUIRE(id != 0);
    const auto r = drainUntilFinished(host, id);
    REQUIRE(r.sawFinished);
    REQUIRE(r.outcome == RenderOutcome::NotEnoughMemory);

    auto rendered = host.currentRender();
    REQUIRE((rendered == nullptr || rendered->requestId != id));

    host.stopWorker();
}

// ===========================================================================
// FX soak — 30-minute streamed run, flat memory + bounded CPU.
//
// HOW TO RUN (it is HIDDEN from the NO-FILTER default ctest run via the Catch2
// `[.]` prefix on the `[.soak]` tag; the `qa-adversarial`-labelled CTest then
// excludes it explicitly via its `[qa-adversarial]~[.soak]` selector, so
// `ctest -L qa-adversarial` never triggers it — run it directly by tag):
//
//   cmake --preset default
//   cmake --build --preset default -j 6 --target mwstime_plugin_tests
//   ./build/default/tests/plugin/mwstime_plugin_tests_artefacts/RelWithDebInfo/mwstime_plugin_tests "[.soak]"
//
// It streams 30 minutes of audio (default 48 kHz) through the FX path in 512-
// frame blocks and asserts:
//   - MEMORY: no heap growth after a warm-up window. The FX path is allocation-
//     free after prepareFx() (the 30 s history ring is preallocated there), so
//     the resident set must be flat. We sample the process RSS at the warm-up
//     mark and at the end; the end must not exceed the warm-up mark by more than
//     a small slack (page-cache / allocator noise).
//   - CPU: bounded and roughly stationary. We time successive 1-minute windows;
//     the last window must not be materially slower than the first (a leak or an
//     unbounded buffer would show as monotonically rising per-window time).
//
// The run also asserts every block stays finite for the full 30 minutes.
// Record the measured figures in the PR (acceptance criterion).
// ===========================================================================
namespace
{
// Best-effort current resident-set size in bytes (macOS / Linux). Returns 0 if
// unavailable — the soak test then skips the absolute-RSS assertion and relies
// on the CPU-stationarity check.
std::size_t currentRssBytes() noexcept
{
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count)
        == KERN_SUCCESS)
        return static_cast<std::size_t>(info.resident_size);
    return 0;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr)
        return 0;
    long total = 0, resident = 0;
    const int got = std::fscanf(f, "%ld %ld", &total, &resident);
    std::fclose(f);
    if (got < 2 || resident <= 0)
        return 0;
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}
} // namespace

TEST_CASE("qa-adversarial: FX 30-minute soak — flat memory, bounded CPU, finite output",
          "[qa-adversarial][.soak]")
{
    constexpr double kHostRate = 48000.0;
    constexpr int kBlock = 512;
    constexpr double kTotalSeconds = 30.0 * 60.0; // 30 minutes
    constexpr double kWindowSeconds = 60.0;       // one CPU-timing window
    const auto totalFrames = static_cast<std::int64_t>(kHostRate * kTotalSeconds);
    const auto windowFrames = static_cast<std::int64_t>(kHostRate * kWindowSeconds);

    // FREE T > 100% so the scheduler runs continuously for the whole soak;
    // CHARACTER OFF so the path is the fully allocation-free one (the 30 s
    // history ring is the only allocation, done in prepareFx).
    auto params = baseParams(ModelId::S1000, PluginMode::Fx, /*character=*/false);
    params.timeFactor = 175.0;

    EngineHost host;
    host.prepareFx(kHostRate, kBlock, /*channels=*/2, params);

    std::vector<std::vector<float>> bufs(2, std::vector<float>(kBlock));
    std::vector<float*> chans(2);

    // A cheap deterministic per-block input generator (a slow sweep) so the
    // soak feeds real, varying content without holding 30 min of audio in RAM.
    std::uint32_t rng = 0x12345u;
    auto fillBlock = [&](std::int64_t startFrame, std::int64_t len) {
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            for (std::int64_t i = 0; i < len; ++i)
            {
                const double t = static_cast<double>(startFrame + i) / kHostRate;
                rng = rng * 1664525u + 1013904223u;
                const double noise =
                    static_cast<double>(static_cast<std::int32_t>(rng)) * (0.02 / 2147483648.0);
                bufs[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                    0.6 * std::sin(2.0 * kPi * 220.0 * t) + noise);
            }
            chans[ch] = bufs[ch].data();
        }
    };

    const RealtimeStretcher::TransportInfo transport{}; // FREE ignores it

    std::size_t warmupRss = 0;
    std::size_t finalRss = 0;
    double firstWindowSec = 0.0;
    double lastWindowSec = 0.0;
    std::int64_t windowIndex = 0;
    std::int64_t windowStartFrame = 0;
    auto windowClock = std::chrono::steady_clock::now();
    bool allFiniteSoFar = true;

    std::int64_t pos = 0;
    while (pos < totalFrames)
    {
        const auto len = std::min<std::int64_t>(kBlock, totalFrames - pos);
        fillBlock(pos, len);
        host.processFxBlock(chans.data(), 2, static_cast<int>(len), params, transport);
        for (std::size_t ch = 0; ch < 2; ++ch)
            if (!allFinite(bufs[ch].data(), static_cast<std::size_t>(len)))
                allFiniteSoFar = false;
        pos += len;

        // Close a 1-minute window: record its wall time, and the RSS at the end
        // of the first window (warm-up mark) and at the very end.
        if (pos - windowStartFrame >= windowFrames || pos >= totalFrames)
        {
            const auto now = std::chrono::steady_clock::now();
            const double sec = std::chrono::duration<double>(now - windowClock).count();
            if (windowIndex == 0)
            {
                firstWindowSec = sec;
                warmupRss = currentRssBytes();
            }
            lastWindowSec = sec;
            windowClock = now;
            windowStartFrame = pos;
            ++windowIndex;
        }
    }
    finalRss = currentRssBytes();
    host.collectFxGarbage();

    REQUIRE(allFiniteSoFar);

    std::printf("[soak] windows=%lld  first-window=%.3fs  last-window=%.3fs  "
                "warmup-RSS=%.1f MB  final-RSS=%.1f MB\n",
                static_cast<long long>(windowIndex), firstWindowSec, lastWindowSec,
                static_cast<double>(warmupRss) / (1024.0 * 1024.0),
                static_cast<double>(finalRss) / (1024.0 * 1024.0));

    // MEMORY: flat after warm-up. Allow a small slack for allocator/page noise;
    // a leak over 30 minutes would dwarf this. Skipped if RSS is unavailable.
    if (warmupRss > 0 && finalRss > 0)
    {
        const std::size_t slack = 32u * 1024u * 1024u; // 32 MB
        REQUIRE(finalRss <= warmupRss + slack);
    }

    // CPU: roughly stationary. The last 1-minute window must not be materially
    // slower than the first (a growing buffer would show as a rising trend).
    if (firstWindowSec > 0.0 && lastWindowSec > 0.0)
        REQUIRE(lastWindowSec <= firstWindowSec * 2.0 + 0.5);
}
