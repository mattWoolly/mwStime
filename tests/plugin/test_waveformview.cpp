// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 043 — WaveformView headless logic tests: pixel↔sample zone math at
// several widths/totals, peaks-cache correctness on a known ramp buffer,
// drop-file extension filter, click-vs-handle-drag gesture disambiguation,
// the lock-free scope FIFO, and a component-level fixture render asserting
// the handles/waveform actually land on pixels.
//
// Test-case names begin with the tag word so `ctest -R waveformview`
// matches (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "ui/WaveformView.h"

#include "mws/core/Buffer.h"

using mws::core::AudioBuffer;
using mws::ui::WaveformView;
namespace wf = mws::ui::waveform;

namespace {

/// A deterministic full-scale ramp fixture: channel 0 rises -1 → +1 over
/// numFrames, channel 1 (when present) is its negation.
std::shared_ptr<const AudioBuffer> makeRamp(std::size_t numChannels,
                                            std::size_t numFrames)
{
    auto buffer = std::make_shared<AudioBuffer>(numChannels, numFrames);
    for (std::size_t f = 0; f < numFrames; ++f)
    {
        const auto v = numFrames < 2
                           ? 0.0f
                           : -1.0f
                                 + 2.0f * static_cast<float>(f)
                                       / static_cast<float>(numFrames - 1);
        buffer->channel(0)[f] = v;
        if (numChannels > 1)
            buffer->channel(1)[f] = -v;
    }
    buffer->sampleRate = 44100.0;
    return buffer;
}

/// Brute-force reference min/max for one peaks column (independent
/// reimplementation of the cache's column split).
wf::PeakColumn referenceColumn(const AudioBuffer& buffer, int column, int width)
{
    const auto total = static_cast<std::int64_t>(buffer.numFrames());
    auto begin = static_cast<std::int64_t>(column) * total / width;
    auto end = static_cast<std::int64_t>(column + 1) * total / width;
    if (begin > total - 1)
        begin = total - 1;
    if (end < begin + 1)
        end = begin + 1;

    wf::PeakColumn out{ std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::lowest() };
    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
        for (auto f = begin; f < end; ++f)
        {
            const auto v = buffer.channel(ch)[static_cast<std::size_t>(f)];
            out.minValue = std::min(out.minValue, v);
            out.maxValue = std::max(out.maxValue, v);
        }
    return out;
}

/// True when any pixel in image column x is within tolerance of colour.
bool columnContainsColour(const juce::Image& image, int x, juce::Colour colour,
                          int tolerance = 8)
{
    for (int y = 0; y < image.getHeight(); ++y)
    {
        const auto p = image.getPixelAt(x, y);
        if (std::abs(p.getRed() - colour.getRed()) <= tolerance
            && std::abs(p.getGreen() - colour.getGreen()) <= tolerance
            && std::abs(p.getBlue() - colour.getBlue()) <= tolerance)
            return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Pixel ↔ sample zone math (several widths / fit-to-width "zooms")
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: zone math maps endpoints exactly at several widths",
          "[waveformview]")
{
    const std::int64_t totals[] = { 131072, 44100, 100 };
    const int widths[] = { 336, 680, 1000, 127 };

    for (const auto total : totals)
        for (const auto width : widths)
        {
            CAPTURE(total, width);
            CHECK(wf::pixelToSample(0, width, total) == 0);
            CHECK(wf::pixelToSample(width, width, total) == total);
            CHECK(wf::sampleToPixel(0, width, total) == 0);
            CHECK(wf::sampleToPixel(total, width, total) == width);
        }
}

TEST_CASE("waveformview: zone math is monotonic and round-trips px → frame → px "
          "when frames outnumber pixels",
          "[waveformview]")
{
    for (const auto width : { 336, 680, 127 })
    {
        const std::int64_t total = 131072;  // §1 mockup zone-end value
        std::int64_t previous = -1;
        for (int px = 0; px <= width; ++px)
        {
            const auto frame = wf::pixelToSample(px, width, total);
            CAPTURE(width, px, frame);
            CHECK(frame >= previous);  // monotonic
            CHECK(wf::sampleToPixel(frame, width, total) == px);  // round trip
            previous = frame;
        }
    }
}

TEST_CASE("waveformview: zone math clamps out-of-range input and degenerate sizes",
          "[waveformview]")
{
    CHECK(wf::pixelToSample(-25, 340, 1000) == 0);
    CHECK(wf::pixelToSample(999, 340, 1000) == 1000);
    CHECK(wf::sampleToPixel(-1, 340, 1000) == 0);
    CHECK(wf::sampleToPixel(2000, 340, 1000) == 340);

    // No crash / zero result on empty buffer or zero width.
    CHECK(wf::pixelToSample(10, 0, 1000) == 0);
    CHECK(wf::pixelToSample(10, 340, 0) == 0);
    CHECK(wf::sampleToPixel(10, 0, 1000) == 0);
    CHECK(wf::sampleToPixel(10, 340, 0) == 0);
}

// ---------------------------------------------------------------------------
// Peaks cache on a known ramp buffer
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: peaks cache matches brute-force min/max on a stereo ramp",
          "[waveformview]")
{
    const auto ramp = makeRamp(2, 1000);

    for (const auto width : { 10, 100, 336 })
    {
        wf::PeaksCache cache;
        cache.reset(ramp, width);
        REQUIRE_FALSE(cache.isComplete());

        cache.buildAll();
        REQUIRE(cache.isComplete());
        REQUIRE(cache.numColumns() == width);
        REQUIRE(cache.numColumnsBuilt() == width);
        REQUIRE(cache.totalFrames() == 1000);

        for (int c = 0; c < width; ++c)
        {
            const auto expected = referenceColumn(*ramp, c, width);
            const auto got = cache.column(c);
            CAPTURE(width, c);
            CHECK(got.minValue == expected.minValue);
            CHECK(got.maxValue == expected.maxValue);
        }
    }
}

TEST_CASE("waveformview: peaks cache time-sliced build progresses incrementally "
          "and converges to the one-shot result",
          "[waveformview]")
{
    const auto ramp = makeRamp(1, 4096);
    const int width = 64;

    wf::PeaksCache sliced;
    sliced.reset(ramp, width);

    int slices = 0;
    int previousBuilt = 0;
    while (! sliced.buildSlice(256))  // tiny budget → many slices
    {
        ++slices;
        CHECK(sliced.numColumnsBuilt() >= previousBuilt);
        previousBuilt = sliced.numColumnsBuilt();
        REQUIRE(slices < 10000);  // never spins forever
    }
    CHECK(slices > 1);  // the budget actually time-sliced the work

    wf::PeaksCache oneShot;
    oneShot.reset(ramp, width);
    oneShot.buildAll();

    for (int c = 0; c < width; ++c)
    {
        CHECK(sliced.column(c).minValue == oneShot.column(c).minValue);
        CHECK(sliced.column(c).maxValue == oneShot.column(c).maxValue);
    }
}

TEST_CASE("waveformview: peaks cache handles more columns than frames and "
          "empty/null buffers",
          "[waveformview]")
{
    // width > frames: every column still covers at least one frame.
    const auto tiny = makeRamp(1, 4);
    wf::PeaksCache cache;
    cache.reset(tiny, 9);
    cache.buildAll();
    REQUIRE(cache.isComplete());
    for (int c = 0; c < 9; ++c)
    {
        const auto expected = referenceColumn(*tiny, c, 9);
        CHECK(cache.column(c).minValue == expected.minValue);
        CHECK(cache.column(c).maxValue == expected.maxValue);
    }

    // Null buffer → trivially complete, no columns.
    cache.reset(nullptr, 100);
    CHECK(cache.isComplete());
    CHECK(cache.numColumns() == 0);
    CHECK(cache.buildSlice());

    // Zero-frame buffer behaves like null.
    cache.reset(std::make_shared<AudioBuffer>(2, 0), 100);
    CHECK(cache.isComplete());
    CHECK(cache.numColumns() == 0);
}

TEST_CASE("waveformview: peaks cache keeps its own shared_ptr to the published "
          "buffer (RCU-style — no audio-thread access, architecture.md §4)",
          "[waveformview]")
{
    wf::PeaksCache cache;
    {
        auto ramp = makeRamp(1, 256);
        cache.reset(ramp, 16);
        ramp.reset();  // publisher drops its reference before the build runs
    }

    cache.buildAll();  // must still read valid audio via the cache's copy
    REQUIRE(cache.isComplete());
    CHECK(cache.column(0).minValue == -1.0f);
    CHECK(cache.column(15).maxValue == 1.0f);
}

// ---------------------------------------------------------------------------
// Drop-file filter (wav/aiff/flac at v1 — ui-design §6.1, no MP3)
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: drop filter accepts wav/aiff/flac case-insensitively "
          "and rejects everything else",
          "[waveformview]")
{
    CHECK(wf::isSupportedAudioFile("/breaks/amen_165.wav"));
    CHECK(wf::isSupportedAudioFile("C:\\Breaks\\AMEN.WAV"));
    CHECK(wf::isSupportedAudioFile("/x/think.aiff"));
    CHECK(wf::isSupportedAudioFile("/x/think.AIF"));
    CHECK(wf::isSupportedAudioFile("/x/apache.flac"));
    CHECK(wf::isSupportedAudioFile("/x/apache.FlAc"));

    CHECK_FALSE(wf::isSupportedAudioFile("/x/amen.mp3"));  // no MP3 at v1
    CHECK_FALSE(wf::isSupportedAudioFile("/x/amen.ogg"));
    CHECK_FALSE(wf::isSupportedAudioFile("/x/notes.txt"));
    CHECK_FALSE(wf::isSupportedAudioFile("/x/wav"));         // extension, not name
    CHECK_FALSE(wf::isSupportedAudioFile("/x/amen.wav.mp3"));
    CHECK_FALSE(wf::isSupportedAudioFile(""));

    CHECK(wf::anySupportedAudioFile({ "/x/a.mp3", "/x/b.wav" }));
    CHECK_FALSE(wf::anySupportedAudioFile({ "/x/a.mp3", "/x/b.ogg" }));
    CHECK_FALSE(wf::anySupportedAudioFile({}));
}

// ---------------------------------------------------------------------------
// Click vs handle-drag disambiguation
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: zone hit test picks the nearest handle inside the grab "
          "radius, body elsewhere",
          "[waveformview]")
{
    constexpr int grab = WaveformView::kHandleGrabRadiusPx;  // 6 px
    const int startPx = 80, endPx = 240;

    CHECK(wf::hitTestZone(startPx, startPx, endPx, grab) == wf::ZonePart::StartHandle);
    CHECK(wf::hitTestZone(startPx + grab, startPx, endPx, grab)
          == wf::ZonePart::StartHandle);
    CHECK(wf::hitTestZone(startPx + grab + 1, startPx, endPx, grab)
          == wf::ZonePart::Body);
    CHECK(wf::hitTestZone(endPx - 2, startPx, endPx, grab) == wf::ZonePart::EndHandle);
    CHECK(wf::hitTestZone((startPx + endPx) / 2, startPx, endPx, grab)
          == wf::ZonePart::Body);
    CHECK(wf::hitTestZone(0, startPx, endPx, grab) == wf::ZonePart::Body);

    // Coincident handles: the start handle wins the tie (then dragging it
    // apart re-separates them).
    CHECK(wf::hitTestZone(100, 100, 100, grab) == wf::ZonePart::StartHandle);

    // Near-coincident: nearest handle wins even when both are in range.
    CHECK(wf::hitTestZone(103, 100, 104, grab) == wf::ZonePart::EndHandle);
    CHECK(wf::hitTestZone(101, 100, 104, grab) == wf::ZonePart::StartHandle);
}

TEST_CASE("waveformview: gesture classification — plain body click auditions, "
          "handle press always edits the zone, body drag exports",
          "[waveformview]")
{
    constexpr int slop = WaveformView::kClickSlopPx;  // 4 px

    // Plain click (within slop) on the body → audition event.
    CHECK(wf::classifyGesture(wf::ZonePart::Body, 0, slop) == wf::Gesture::Click);
    CHECK(wf::classifyGesture(wf::ZonePart::Body, slop, slop) == wf::Gesture::Click);

    // Body drag past the slop → drag-out (export hook), never an audition.
    CHECK(wf::classifyGesture(wf::ZonePart::Body, slop + 1, slop)
          == wf::Gesture::DragOut);
    CHECK(wf::classifyGesture(wf::ZonePart::Body, 200, slop) == wf::Gesture::DragOut);

    // A press that begins on a handle is a zone edit regardless of travel —
    // "not on a zone handle, not a drag" is the audition condition (task 043).
    for (const auto part : { wf::ZonePart::StartHandle, wf::ZonePart::EndHandle })
        for (const auto travel : { 0, slop, slop + 1, 300 })
            CHECK(wf::classifyGesture(part, travel, slop) == wf::Gesture::HandleDrag);
}

// ---------------------------------------------------------------------------
// Scope FIFO (audio-thread producer → timer-polled consumer)
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: scope FIFO delivers samples in order and drops on "
          "overflow instead of blocking",
          "[waveformview]")
{
    wf::ScopeFifo fifo(64);

    std::vector<float> in(40);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i);

    fifo.push(in.data(), static_cast<int>(in.size()));
    REQUIRE(fifo.numReady() == 40);

    std::vector<float> out(64, -1.0f);
    REQUIRE(fifo.pop(out.data(), 64) == 40);
    for (int i = 0; i < 40; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == static_cast<float>(i));

    // Overflow: capacity bounds what is kept; push never blocks or grows.
    wf::ScopeFifo small(16);
    std::vector<float> big(100);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<float>(i);
    small.push(big.data(), static_cast<int>(big.size()));
    const auto kept = small.numReady();
    CHECK(kept > 0);
    CHECK(kept < 16 + 1);
    REQUIRE(small.pop(out.data(), 64) == kept);
    for (int i = 0; i < kept; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == static_cast<float>(i));  // oldest kept
}

TEST_CASE("waveformview: scope FIFO decimated push keeps every strideth sample "
          "(producer-side decimation, architecture.md §4)",
          "[waveformview]")
{
    wf::ScopeFifo fifo(64);

    std::vector<float> in(16);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i);

    fifo.pushDecimated(in.data(), static_cast<int>(in.size()), 4);
    REQUIRE(fifo.numReady() == 4);

    float out[8]{};
    REQUIRE(fifo.pop(out, 8) == 4);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == 4.0f);
    CHECK(out[2] == 8.0f);
    CHECK(out[3] == 12.0f);
}

// ---------------------------------------------------------------------------
// Component-level: fixture render, zone API + events, drop callbacks
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: component renders a loaded ramp fixture with zone "
          "handles on the expected pixel columns",
          "[waveformview]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    WaveformView view;
    view.setSize(340, 86);  // geometry::kWaveform frame size

    view.setSourceSample(makeRamp(2, 4096));
    REQUIRE(view.hasSource());
    CHECK(view.sourceFrames() == 4096);

    // Zone defaults to full length on load (ui-design §6.1 step 2).
    CHECK(view.zoneStart() == 0);
    CHECK(view.zoneEnd() == 4096);

    view.setZone(1024, 3072, juce::dontSendNotification);
    view.finishPeaksBuild();
    REQUIRE(view.isPeaksBuildComplete());

    const auto snapshot =
        view.createComponentSnapshot(view.getLocalBounds(), false, 1.0f);
    REQUIRE_FALSE(snapshot.isNull());
    REQUIRE(snapshot.getWidth() == 340);
    REQUIRE(snapshot.getHeight() == 86);

    const auto& spec = mws::ui::faceplateSpecFor(mws::model::ModelId::S1000);

    // Handle columns: inner area starts at x=2, inner width 336.
    const auto startX = 2 + wf::sampleToPixel(1024, 336, 4096);
    const auto endX = 2 + wf::sampleToPixel(3072, 336, 4096);
    CHECK(columnContainsColour(snapshot, startX, spec.accent));
    CHECK(columnContainsColour(snapshot, endX, spec.accent));

    // The waveform ink lands mid-window (ramp passes through the centre).
    CHECK(columnContainsColour(snapshot, 170, spec.lcdInk));

    // No accent handle out in the dimmed body region.
    CHECK_FALSE(columnContainsColour(snapshot, (startX + endX) / 2, spec.accent, 4));
}

TEST_CASE("waveformview: idle (no sample) view paints the DROP SAMPLE HERE "
          "caption, not a waveform",
          "[waveformview]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    WaveformView view;
    view.setSize(340, 86);
    REQUIRE_FALSE(view.hasSource());

    const auto snapshot =
        view.createComponentSnapshot(view.getLocalBounds(), false, 1.0f);
    REQUIRE_FALSE(snapshot.isNull());

    // Some caption ink must land near the centre row; the dimmed caption is
    // lighter than the LCD back but not full-strength ink.
    const auto& spec = mws::ui::faceplateSpecFor(mws::model::ModelId::S1000);
    bool captionInkFound = false;
    for (int x = 60; x < 280 && ! captionInkFound; ++x)
        for (int y = 30; y < 56 && ! captionInkFound; ++y)
            captionInkFound = snapshot.getPixelAt(x, y) != spec.lcdBack;
    CHECK(captionInkFound);
}

TEST_CASE("waveformview: setZone clamps to the source length, keeps start < end, "
          "and notifies exactly per NotificationType",
          "[waveformview]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    WaveformView view;
    view.setSize(340, 86);
    view.setSourceSample(makeRamp(1, 1000));

    int notifications = 0;
    std::int64_t lastStart = -1, lastEnd = -1;
    view.onZoneChange = [&](std::int64_t s, std::int64_t e) {
        ++notifications;
        lastStart = s;
        lastEnd = e;
    };

    view.setZone(100, 900);
    CHECK(notifications == 1);
    CHECK(lastStart == 100);
    CHECK(lastEnd == 900);

    view.setZone(100, 900);  // no change → no re-notification
    CHECK(notifications == 1);

    view.setZone(-50, 2000);  // clamped to the buffer
    CHECK(notifications == 2);
    CHECK(lastStart == 0);
    CHECK(lastEnd == 1000);

    view.setZone(500, 400);  // inverted → start kept, end forced past it
    CHECK(view.zoneStart() == 500);
    CHECK(view.zoneEnd() == 501);

    view.setZone(10, 20, juce::dontSendNotification);
    CHECK(notifications == 3);  // only the inverted edit above notified
    CHECK(view.zoneStart() == 10);
    CHECK(view.zoneEnd() == 20);
}

TEST_CASE("waveformview: file drag interest and drop callback pass only the "
          "supported files through",
          "[waveformview]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    WaveformView view;
    view.setSize(340, 86);

    CHECK(view.isInterestedInFileDrag({ "/x/amen.wav" }));
    CHECK(view.isInterestedInFileDrag({ "/x/a.mp3", "/x/b.flac" }));
    CHECK_FALSE(view.isInterestedInFileDrag({ "/x/a.mp3", "/x/b.ogg" }));

    juce::StringArray received;
    int drops = 0;
    view.onFilesDropped = [&](const juce::StringArray& files) {
        ++drops;
        received = files;
    };

    view.filesDropped({ "/x/a.mp3", "/x/b.wav", "/x/c.aiff" }, 10, 10);
    REQUIRE(drops == 1);
    REQUIRE(received.size() == 2);
    CHECK(received[0] == "/x/b.wav");
    CHECK(received[1] == "/x/c.aiff");

    view.filesDropped({ "/x/a.mp3" }, 10, 10);  // nothing loadable → no event
    CHECK(drops == 1);
}

TEST_CASE("waveformview: A/B prominence and play head state are tracked and "
          "render without a window",
          "[waveformview]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    WaveformView view;
    view.setSize(340, 86);
    view.setSourceSample(makeRamp(2, 4096));
    view.setRenderedSample(makeRamp(2, 2048));
    view.finishPeaksBuild();

    CHECK(view.hasRender());
    CHECK_FALSE(view.isRenderProminent());

    view.setRenderProminent(true);  // F6 A/B (ui-design §6.3 step 3)
    CHECK(view.isRenderProminent());

    // With the render prominent its accent layer is drawn solid mid-window.
    const auto snapshot =
        view.createComponentSnapshot(view.getLocalBounds(), false, 1.0f);
    REQUIRE_FALSE(snapshot.isNull());
    const auto& spec = mws::ui::faceplateSpecFor(mws::model::ModelId::S1000);
    CHECK(columnContainsColour(snapshot, 170, spec.accent));

    view.setPlayheadFrame(2048);
    CHECK(view.playheadFrame() == 2048);
    view.setPlayheadFrame(99999);  // clamped to the source length
    CHECK(view.playheadFrame() == 4096);
    view.setPlayheadFrame(-1);
    CHECK(view.playheadFrame() == -1);
}

TEST_CASE("waveformview: FX scope mode flips the region into scope state and "
          "snapshots headlessly",
          "[waveformview]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    WaveformView view;
    view.setSize(340, 86);

    wf::ScopeFifo fifo(4096);
    view.attachScopeFifo(&fifo);

    CHECK_FALSE(view.isFxScopeMode());
    view.setFxScopeMode(true);  // ui-design §6.4 step 1
    CHECK(view.isFxScopeMode());

    // Producer side (audio thread in real life) feeding the fifo is legal and
    // lock-free regardless of the view's paint/poll state.
    std::vector<float> block(512, 0.5f);
    fifo.pushDecimated(block.data(), static_cast<int>(block.size()), 8);
    CHECK(fifo.numReady() == 64);

    const auto snapshot =
        view.createComponentSnapshot(view.getLocalBounds(), false, 1.0f);
    REQUIRE_FALSE(snapshot.isNull());

    view.setFxScopeMode(false);
    CHECK_FALSE(view.isFxScopeMode());
}

// ---------------------------------------------------------------------------
// Visual-review snapshot (env-gated, house pattern — never a normal output).
// Set MWS_WAVEFORM_SNAPSHOT_DIR to dump a PNG of the loaded-fixture view.
// ---------------------------------------------------------------------------

TEST_CASE("waveformview: snapshot dump for visual review when "
          "MWS_WAVEFORM_SNAPSHOT_DIR is set",
          "[waveformview]")
{
    const auto dir =
        juce::SystemStats::getEnvironmentVariable("MWS_WAVEFORM_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
    {
        SUCCEED("MWS_WAVEFORM_SNAPSHOT_DIR not set — snapshot dump skipped");
        return;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto outDir = juce::File(dir);
    REQUIRE(outDir.createDirectory().wasOk());

    WaveformView view;
    view.setSize(340, 86);
    view.setSourceSample(makeRamp(2, 4096));
    view.setRenderedSample(makeRamp(2, 2048));
    view.setZone(1024, 3072, juce::dontSendNotification);
    view.setPlayheadFrame(2048);
    view.finishPeaksBuild();

    const auto snapshot =
        view.createComponentSnapshot(view.getLocalBounds(), false, 1.0f);
    REQUIRE_FALSE(snapshot.isNull());

    const auto file = outDir.getChildFile("waveformview_fixture.png");
    file.deleteFile();
    juce::FileOutputStream stream(file);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(snapshot, stream));
    std::cout << "waveformview snapshot: " << file.getFullPathName() << "\n";
}
