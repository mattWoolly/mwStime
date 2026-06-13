// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// WaveformView — the waveform / drop-zone region (task 043; docs/design/
// ui-design.md §1 region 5, §2 table, §6.1/§6.3/§6.4):
//   · cached-peak rendering (min/max per pixel column) of the published
//     SourceSample / RenderedSample buffers, rebuilt time-sliced on the
//     MESSAGE thread only — the audio thread is never touched
//     (architecture.md §4: components read published buffers via a
//     shared_ptr copy; acceptance: "peaks rebuild never blocks audio"),
//   · juce::FileDragAndDropTarget with hover highlight and the idle
//     "DROP SAMPLE HERE" caption; accepts wav/aiff/flac (ui-design §6.1 —
//     no MP3 at v1); the actual file-load path is wired in 045b,
//   · two draggable stretch-zone handles mapping to zoneStart/zoneEnd
//     ("stretch zone / to", MAN §3) with pixel↔sample conversion,
//   · original-vs-render A/B overlay (F6 A/B toggles which layer is
//     prominent, ui-design §6.3 step 3),
//   · play head cursor during audition; FX mode turns the region into a
//     rolling input scope fed by a lock-free FIFO from the processor
//     (decimated at the producer), timer-polled (architecture.md §4:
//     "meter/scope data via lock-free FIFO → timer poll"),
//   · drag-out gesture initiation and click-to-audition hooks (audition is
//     always available from the UI — architecture.md §7); both are wired to
//     ExportService / SamplePlayer in 045b.
//
// All decision logic (zone math, peaks cache, drop filter, gesture
// classification) lives in mws::ui::waveform as plain functions/classes so
// tests/plugin/test_waveformview.cpp exercises it headlessly.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FaceplateSpec.h"
#include "mws/core/Buffer.h"

namespace mws::ui::waveform {

// ---------------------------------------------------------------------------
// Pixel ↔ sample zone math (headless; fit-to-width is the only zoom at v1)
// ---------------------------------------------------------------------------

/// Maps a pixel x in [0, widthPx] onto a frame index in [0, totalFrames]
/// (nearest-frame rounding; out-of-range input is clamped).
[[nodiscard]] std::int64_t pixelToSample(int px, int widthPx,
                                         std::int64_t totalFrames) noexcept;

/// Maps a frame index in [0, totalFrames] onto a pixel x in [0, widthPx]
/// (nearest-pixel rounding; out-of-range input is clamped).
[[nodiscard]] int sampleToPixel(std::int64_t frame, int widthPx,
                                std::int64_t totalFrames) noexcept;

// ---------------------------------------------------------------------------
// Peaks cache (headless)
// ---------------------------------------------------------------------------

/// One pixel column of the peaks cache: the min/max sample values across the
/// column's frame range and all channels.
struct PeakColumn {
    float minValue = 0.0f;
    float maxValue = 0.0f;
};

/// Min/max-per-pixel-column cache over a published (immutable, shared_ptr)
/// audio buffer. The cache keeps its OWN shared_ptr copy of the buffer
/// (RCU-style read of the published sample — architecture.md §4) and is
/// rebuilt with buildSlice() calls so a large file never stalls the message
/// thread ("time-sliced on the message thread"). NEVER touched by — and never
/// touching — the audio thread.
class PeaksCache
{
public:
    /// Frames consumed per buildSlice() call by default: ~1M frames is well
    /// under a frame budget on any 2020s machine but bounds the worst case.
    static constexpr std::size_t kDefaultSliceFrames = 1u << 20;

    /// Points the cache at a buffer (may be nullptr) and a column count
    /// (usually the view width in px); resets build progress. Cheap — no
    /// scanning happens here.
    void reset(std::shared_ptr<const core::AudioBuffer> buffer, int numColumns);

    /// Builds at most ~maxFrames worth of columns; returns true when the
    /// whole cache is complete (also when there is nothing to build).
    bool buildSlice(std::size_t maxFrames = kDefaultSliceFrames);

    /// Convenience: run buildSlice until complete (small buffers, tests).
    void buildAll();

    [[nodiscard]] bool isComplete() const noexcept;
    [[nodiscard]] int numColumns() const noexcept;
    [[nodiscard]] int numColumnsBuilt() const noexcept;
    [[nodiscard]] std::int64_t totalFrames() const noexcept;

    /// Valid for index < numColumnsBuilt().
    [[nodiscard]] PeakColumn column(int index) const noexcept;

private:
    std::shared_ptr<const core::AudioBuffer> source;
    std::vector<PeakColumn> columns;
    int built = 0;
};

// ---------------------------------------------------------------------------
// Drop-file filter (headless)
// ---------------------------------------------------------------------------

/// True for the v1 load formats wav/aiff/flac (ui-design §6.1; FileLoader
/// task 031 decodes the same set — no MP3 at v1). Case-insensitive.
[[nodiscard]] bool isSupportedAudioFile(const juce::String& path);

/// True when at least one file in the list is loadable (drag hover accept).
[[nodiscard]] bool anySupportedAudioFile(const juce::StringArray& paths);

// ---------------------------------------------------------------------------
// Gesture classification (headless)
// ---------------------------------------------------------------------------

/// What a mouse-down at pixel x lands on, given the two handle pixels.
enum class ZonePart : std::uint8_t { StartHandle, EndHandle, Body };

/// Nearest-handle-wins hit test within grabRadiusPx; everything else is Body.
[[nodiscard]] ZonePart hitTestZone(int px, int startHandlePx, int endHandlePx,
                                   int grabRadiusPx) noexcept;

/// The three gestures the region distinguishes (task 043 scope).
enum class Gesture : std::uint8_t {
    Click,       ///< plain click on the body → audition event
    HandleDrag,  ///< press began on a zone handle → zone edit (never audition)
    DragOut,     ///< body drag past the slop → export initiation (hook, 045b)
};

/// Classifies a completed (or in-flight) gesture from where the press began
/// and how far the pointer travelled. A press that begins on a handle is a
/// HandleDrag even with zero travel — "a plain click on the waveform (not on
/// a zone handle, not a drag) emits an audition event" (task 043).
[[nodiscard]] Gesture classifyGesture(ZonePart pressedPart, int maxTravelPx,
                                      int clickSlopPx) noexcept;

// ---------------------------------------------------------------------------
// Scope FIFO (audio-thread producer → message-thread consumer)
// ---------------------------------------------------------------------------

/// Single-producer/single-consumer lock-free float FIFO for the FX-mode
/// input scope (architecture.md §4 "lock-free FIFO → timer poll"). The
/// processor pushes decimated input samples from the audio thread (wired in
/// 045b); the view drains it on its UI timer. push/pop never allocate, never
/// lock, and drop samples instead of blocking when full.
class ScopeFifo
{
public:
    explicit ScopeFifo(int capacity = 8192);

    /// Audio-thread side. Writes as many samples as fit; excess is dropped.
    void push(const float* samples, int numSamples) noexcept;

    /// Audio-thread side. Pushes every strideth sample of the block (the
    /// "decimated" feed — decimation happens at the producer).
    void pushDecimated(const float* samples, int numSamples, int stride) noexcept;

    /// Message-thread side. Returns the number of samples written to dest.
    int pop(float* dest, int maxSamples) noexcept;

    [[nodiscard]] int capacity() const noexcept { return fifo.getTotalSize(); }
    [[nodiscard]] int numReady() const noexcept { return fifo.getNumReady(); }

private:
    juce::AbstractFifo fifo;
    std::vector<float> storage;
};

} // namespace mws::ui::waveform

namespace mws::ui {

/// The waveform region component (geometry::kWaveform). See file header.
class WaveformView final : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
{
public:
    /// Handle grab tolerance / click-vs-drag slop, in px at any scale.
    static constexpr int kHandleGrabRadiusPx = 6;
    static constexpr int kClickSlopPx = 4;
    /// Rolling scope window length (decimated samples).
    static constexpr int kScopeLength = 1024;

    WaveformView();
    ~WaveformView() override;

    /// Palette restyle from a model's FaceplateSpec (palette only —
    /// geometry never forks per model, ui-design §3). Defaults to S1000.
    void setSpec(const FaceplateSpec& spec);

    // --- published buffers (message thread; shared_ptr copies) -------------
    /// The loaded SourceSample. Resets the stretch zone to full length
    /// (ui-design §6.1 step 2; no zone-change callback for the reset) and
    /// schedules a time-sliced peaks rebuild.
    void setSourceSample(std::shared_ptr<const core::AudioBuffer> sample);
    /// The latest RenderedSample for the A/B overlay (nullptr clears it).
    void setRenderedSample(std::shared_ptr<const core::AudioBuffer> sample);
    [[nodiscard]] std::int64_t sourceFrames() const noexcept;
    [[nodiscard]] bool hasSource() const noexcept { return sourceFrames() > 0; }
    [[nodiscard]] bool hasRender() const noexcept { return renderedSample != nullptr; }

    /// Completes any pending time-sliced peaks build synchronously (small
    /// fixtures, tests, screenshot harness).
    void finishPeaksBuild();
    [[nodiscard]] bool isPeaksBuildComplete() const noexcept;

    // --- stretch zone -------------------------------------------------------
    /// Sets the zone in frames (clamped to [0, sourceFrames], start < end).
    void setZone(std::int64_t startFrame, std::int64_t endFrame,
                 juce::NotificationType notification = juce::sendNotification);
    [[nodiscard]] std::int64_t zoneStart() const noexcept { return zoneStartFrame; }
    [[nodiscard]] std::int64_t zoneEnd() const noexcept { return zoneEndFrame; }

    // --- A/B overlay / play head / FX scope ---------------------------------
    /// true → the render layer is prominent, original dimmed (F6 A/B).
    void setRenderProminent(bool renderOnTop);
    [[nodiscard]] bool isRenderProminent() const noexcept { return renderProminent; }

    /// Play head frame during audition; pass a negative frame to hide.
    void setPlayheadFrame(std::int64_t frame);
    [[nodiscard]] std::int64_t playheadFrame() const noexcept { return playhead; }

    /// FX mode: the region becomes a live input scope (ui-design §6.4).
    void setFxScopeMode(bool enabled);
    [[nodiscard]] bool isFxScopeMode() const noexcept { return fxScopeMode; }

    /// Non-owning: the processor-owned FIFO the scope polls (wired in 045b).
    void attachScopeFifo(waveform::ScopeFifo* fifoToPoll) noexcept;

    // --- event hooks (wired in 045b) ----------------------------------------
    std::function<void(std::int64_t startFrame, std::int64_t endFrame)> onZoneChange;
    std::function<void(std::int64_t frame)> onAudition;         ///< click-to-audition
    std::function<void(const juce::StringArray& files)> onFilesDropped;
    std::function<void()> onDragExport;  ///< drag-out initiation → ExportService

    // --- juce::FileDragAndDropTarget ----------------------------------------
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // --- juce::Component -----------------------------------------------------
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void restartPeaksBuild();
    void emitZoneChange();
    [[nodiscard]] int handlePixel(std::int64_t frame) const noexcept;

    void paintIdleCaption(juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintScope(juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintPeakLayer(juce::Graphics& g, juce::Rectangle<float> area,
                        const waveform::PeaksCache& peaks, juce::Colour colour,
                        float alpha) const;
    void paintZone(juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintPlayhead(juce::Graphics& g, juce::Rectangle<float> area) const;

    const FaceplateSpec* spec_ = nullptr;  // never null (defaults to S1000)

    std::shared_ptr<const core::AudioBuffer> sourceSample;
    std::shared_ptr<const core::AudioBuffer> renderedSample;
    waveform::PeaksCache sourcePeaks, renderPeaks;

    std::int64_t zoneStartFrame = 0, zoneEndFrame = 0;
    std::int64_t playhead = -1;
    bool renderProminent = false;

    bool fxScopeMode = false;
    waveform::ScopeFifo* scopeFifo = nullptr;  // non-owning (processor's)
    std::vector<float> scopeRing;              // rolling window, newest last
    std::vector<float> scopePopChunk;          // timer-poll scratch

    bool dragHover = false;

    // in-flight mouse gesture
    waveform::ZonePart pressedPart = waveform::ZonePart::Body;
    int maxTravelPx = 0;
    bool dragOutFired = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformView)
};

} // namespace mws::ui
