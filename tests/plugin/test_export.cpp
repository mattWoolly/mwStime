// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Drag-out / save-as export tests (plan/backlog/036) — the modern half of the
// render-to-new-sample workflow (architecture.md §5.1: RenderedSample ->
// drag-out / save-as WAV; ui-design.md §6.3 step 4):
//   - a published RenderedSample written to a WAV ROUND-TRIPS via mws::core::WavIo
//     and is BIT-FAITHFUL to the render buffer at the chosen depth (16-bit
//     default PI, plus 24-bit),
//   - the deterministic file-name pattern <sample>_<model>_<timeFactor>.wav,
//   - the no-render guard: with nothing published, export is a no-op that posts
//     the typed NoRender event for the LCD (architecture.md §9 — typed enum at
//     this layer, the LCD string is the UI layer's job),
//   - temp files created for drag-out are cleaned up on ExportService
//     destruction (acceptance criterion).
//
// Test-case names begin with "export" so `ctest -R export` selects them (README
// test-selection rules); the bit-faithful gate uses WavIo::read for the readback
// (acceptance: "round-trips via WavIo").

#include <catch2/catch_test_macros.hpp>

#include "EngineHost.h"
#include "ExportService.h"

#include "mws/core/WavIo.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
using mws::core::AudioBuffer;
using mws::core::WavIo;
using mws::engine::ParamSnapshot;
using mws::model::ModelId;
using mws::plugin::EngineHost;
using mws::plugin::ExportDepth;
using mws::plugin::ExportOutcome;
using mws::plugin::ExportService;
using mws::plugin::RenderedSample;

// A deterministic, structured render buffer whose 16-bit codes are all exactly
// representable (every value is an integer multiple of 1/2^15) so the
// quantize-and-readback comparison can be EXACT, not tolerance-based.
std::shared_ptr<const RenderedSample> makeRender(std::size_t frames,
                                                 double rate = 44100.0,
                                                 std::size_t channels = 2)
{
    auto rs = std::make_shared<RenderedSample>();
    rs->audio = AudioBuffer(channels, frames);
    rs->audio.sampleRate = rate;
    rs->info.outputSampleRate = rate;
    rs->info.outputFrames = static_cast<std::int64_t>(frames);
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
        auto v = rs->audio.channel(ch);
        for (std::size_t i = 0; i < frames; ++i)
        {
            // A small integer code in [-100, 100] divided by 2^15: exactly
            // representable in float32 AND exactly recoverable at 16-bit depth.
            const int code = static_cast<int>((i + ch * 7) % 201) - 100;
            v[i] = static_cast<float>(code) / 32768.0f;
        }
    }
    return rs;
}

// Per-depth quantization step (codes that survive a round trip exactly).
double quantStep(ExportDepth depth)
{
    return depth == ExportDepth::Bits16 ? (1.0 / 32768.0) : (1.0 / 8388608.0);
}
} // namespace

TEST_CASE("export round-trips a render via WavIo bit-faithfully at 16-bit",
          "[export]")
{
    const auto rs = makeRender(512, 44100.0, 2);

    juce::TemporaryFile tmp(".wav");
    const auto file = tmp.getFile();

    ExportService svc;
    const auto outcome = svc.writeRender(*rs, file, ExportDepth::Bits16);
    REQUIRE(outcome == ExportOutcome::Ok);
    REQUIRE(file.existsAsFile());

    const auto rr = WavIo::read(file.getFullPathName().toStdString());
    REQUIRE(rr.ok());
    REQUIRE(rr.buffer.numChannels() == rs->audio.numChannels());
    REQUIRE(rr.buffer.numFrames() == rs->audio.numFrames());
    // Sample rate preserved (44100.0 is exactly representable; compare bytes to
    // avoid the -Wfloat-equal/-Werror trip, as test_sample_mode does).
    REQUIRE(std::memcmp(&rr.buffer.sampleRate, &rs->audio.sampleRate,
                        sizeof(double))
            == 0);

    // Bit-faithful: every sample equals the source value (which was chosen to be
    // exactly representable at 16-bit) bit-for-bit through the WAV layer.
    for (std::size_t ch = 0; ch < rs->audio.numChannels(); ++ch)
    {
        const auto a = rs->audio.channel(ch);
        const auto b = rr.buffer.channel(ch);
        REQUIRE(std::memcmp(b.data(), a.data(), a.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("export writes 24-bit when the depth policy selects it", "[export]")
{
    const auto rs = makeRender(256, 22050.0, 1);

    juce::TemporaryFile tmp(".wav");
    const auto file = tmp.getFile();

    ExportService svc;
    REQUIRE(svc.writeRender(*rs, file, ExportDepth::Bits24) == ExportOutcome::Ok);

    const auto rr = WavIo::read(file.getFullPathName().toStdString());
    REQUIRE(rr.ok());
    REQUIRE(rr.buffer.numFrames() == rs->audio.numFrames());

    const auto step = quantStep(ExportDepth::Bits24);
    const auto a = rs->audio.channel(0);
    const auto b = rr.buffer.channel(0);
    for (std::size_t i = 0; i < rs->audio.numFrames(); ++i)
        REQUIRE(std::abs(static_cast<double>(b[i] - a[i])) <= step);
}

TEST_CASE("export filename follows the <sample>_<model>_<timeFactor> pattern",
          "[export]")
{
    ParamSnapshot p;
    p.model = ModelId::S950;
    p.timeFactor = 300.0;

    const auto name = ExportService::deterministicFileName("AMEN_165", p);
    // Deterministic and stable across calls.
    REQUIRE(name == ExportService::deterministicFileName("AMEN_165", p));
    REQUIRE(name == juce::String("AMEN_165_S950_300.wav"));

    // Fractional time factor is rendered without a trailing ".00".
    ParamSnapshot q;
    q.model = ModelId::S1000;
    q.timeFactor = 174.5;
    REQUIRE(ExportService::deterministicFileName("break", q)
            == juce::String("break_S1000_174.5.wav"));

    // An empty / blank sample name falls back to a safe stem (never an empty
    // or path-illegal leading underscore-only name).
    ParamSnapshot r;
    r.model = ModelId::S1100;
    r.timeFactor = 100.0;
    const auto fallback = ExportService::deterministicFileName("", r);
    REQUIRE(fallback == juce::String("render_S1100_100.wav"));
}

TEST_CASE("export filename sanitizes path-illegal characters in the sample name",
          "[export]")
{
    ParamSnapshot p;
    p.model = ModelId::S1000;
    p.timeFactor = 200.0;
    // Slashes / colons / spaces collapse to safe characters so the name is a
    // legal single path segment on every platform.
    const auto name = ExportService::deterministicFileName("a/b:c d", p);
    REQUIRE_FALSE(name.containsAnyOf("/\\:"));
    REQUIRE(name.endsWith("_S1000_200.wav"));
}

TEST_CASE("export with no render published is a no-op that posts NoRender",
          "[export]")
{
    EngineHost host; // nothing published

    int events = 0;
    ExportOutcome last = ExportOutcome::Ok;
    ExportService svc;
    svc.attach(&host);
    svc.onExportEvent = [&](ExportOutcome o) { ++events; last = o; };

    const auto result = svc.exportToTempFile();
    REQUIRE(result.outcome == ExportOutcome::NoRender);
    REQUIRE_FALSE(result.file.existsAsFile());
    REQUIRE(events == 1);
    REQUIRE(last == ExportOutcome::NoRender);
}

TEST_CASE("export to temp file writes the published render and cleans up on "
          "destruction",
          "[export]")
{
    EngineHost host;
    const auto rs = makeRender(128, 44100.0, 2);
    // EngineHost has no public publish API (publication is the worker thread's
    // job); the drag-out path reads currentRender(). Here we verify the temp-file
    // write + cleanup via exportRenderToTempFile, which takes the render the
    // drag-out has already acquired — the same tracked-temp-file code path.

    juce::File written;
    {
        ExportService svc;
        svc.attach(&host);
        svc.setSampleName("loop");

        ParamSnapshot p;
        p.model = ModelId::S1000;
        p.timeFactor = 250.0;
        svc.setRenderParams(p);

        const auto result = svc.exportRenderToTempFile(*rs);
        REQUIRE(result.outcome == ExportOutcome::Ok);
        written = result.file;
        REQUIRE(written.existsAsFile());
        REQUIRE(written.getFileName() == juce::String("loop_S1000_250.wav"));

        // Round-trips via WavIo.
        const auto rr = WavIo::read(written.getFullPathName().toStdString());
        REQUIRE(rr.ok());
        REQUIRE(rr.buffer.numFrames() == rs->audio.numFrames());
    }

    // The service deleted the temp file it created when it went out of scope.
    REQUIRE_FALSE(written.existsAsFile());
}
