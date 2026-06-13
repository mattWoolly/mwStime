// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// WaveformView implementation (task 043). See WaveformView.h for the spec
// references. Threading: everything in this file runs on the MESSAGE thread;
// the only audio-thread code is ScopeFifo::push/pushDecimated (lock-free,
// allocation-free). Peaks are computed from the cache's own shared_ptr copy
// of the published buffer, so the audio thread is never read from and the
// buffer can never disappear mid-build (architecture.md §4).

#include "WaveformView.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "lookandfeel/SeriesLookAndFeel.h"

namespace mws::ui::waveform {

// ---------------------------------------------------------------------------
// Pixel ↔ sample zone math
// ---------------------------------------------------------------------------

std::int64_t pixelToSample(int px, int widthPx, std::int64_t totalFrames) noexcept
{
    if (widthPx <= 0 || totalFrames <= 0)
        return 0;

    px = std::clamp(px, 0, widthPx);
    const auto frame = std::llround(static_cast<double>(px)
                                    * static_cast<double>(totalFrames)
                                    / static_cast<double>(widthPx));
    return std::clamp<std::int64_t>(frame, 0, totalFrames);
}

int sampleToPixel(std::int64_t frame, int widthPx, std::int64_t totalFrames) noexcept
{
    if (widthPx <= 0 || totalFrames <= 0)
        return 0;

    frame = std::clamp<std::int64_t>(frame, 0, totalFrames);
    const auto px = std::llround(static_cast<double>(frame)
                                 * static_cast<double>(widthPx)
                                 / static_cast<double>(totalFrames));
    return static_cast<int>(std::clamp<std::int64_t>(px, 0, widthPx));
}

// ---------------------------------------------------------------------------
// PeaksCache
// ---------------------------------------------------------------------------

void PeaksCache::reset(std::shared_ptr<const core::AudioBuffer> buffer, int numColumns)
{
    source = std::move(buffer);
    if (source != nullptr && (source->numFrames() == 0 || source->numChannels() == 0))
        source = nullptr;

    columns.assign(static_cast<std::size_t>(std::max(numColumns, 0)), PeakColumn{});
    built = 0;

    if (source == nullptr)
        columns.clear();
}

bool PeaksCache::buildSlice(std::size_t maxFrames)
{
    if (isComplete())
        return true;

    const auto total = static_cast<std::int64_t>(source->numFrames());
    const auto width = static_cast<int>(columns.size());
    std::size_t consumed = 0;

    while (built < width && consumed < maxFrames)
    {
        // Column frame range [begin, end): proportional split of the buffer,
        // never empty (when width > total, columns repeat single frames).
        auto begin = static_cast<std::int64_t>(built) * total / width;
        auto end = static_cast<std::int64_t>(built + 1) * total / width;
        begin = std::clamp<std::int64_t>(begin, 0, total - 1);
        end = std::clamp<std::int64_t>(end, begin + 1, total);

        auto minV = std::numeric_limits<float>::max();
        auto maxV = std::numeric_limits<float>::lowest();
        for (std::size_t ch = 0; ch < source->numChannels(); ++ch)
        {
            const auto view = source->channel(ch);
            for (auto f = begin; f < end; ++f)
            {
                const auto v = view[static_cast<std::size_t>(f)];
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
            }
        }

        columns[static_cast<std::size_t>(built)] = { minV, maxV };
        ++built;
        consumed += static_cast<std::size_t>(end - begin) * source->numChannels();
    }

    return isComplete();
}

void PeaksCache::buildAll()
{
    while (! buildSlice())
    {
    }
}

bool PeaksCache::isComplete() const noexcept
{
    return source == nullptr || built == static_cast<int>(columns.size());
}

int PeaksCache::numColumns() const noexcept
{
    return static_cast<int>(columns.size());
}

int PeaksCache::numColumnsBuilt() const noexcept
{
    return built;
}

std::int64_t PeaksCache::totalFrames() const noexcept
{
    return source == nullptr ? 0 : static_cast<std::int64_t>(source->numFrames());
}

PeakColumn PeaksCache::column(int index) const noexcept
{
    jassert(index >= 0 && index < built);
    return columns[static_cast<std::size_t>(index)];
}

// ---------------------------------------------------------------------------
// Drop-file filter
// ---------------------------------------------------------------------------

bool isSupportedAudioFile(const juce::String& path)
{
    const auto lower = path.toLowerCase();
    return lower.endsWith(".wav") || lower.endsWith(".aiff") || lower.endsWith(".aif")
           || lower.endsWith(".flac");
}

bool anySupportedAudioFile(const juce::StringArray& paths)
{
    return std::any_of(paths.begin(), paths.end(),
                       [](const juce::String& p) { return isSupportedAudioFile(p); });
}

// ---------------------------------------------------------------------------
// Gesture classification
// ---------------------------------------------------------------------------

ZonePart hitTestZone(int px, int startHandlePx, int endHandlePx,
                     int grabRadiusPx) noexcept
{
    const auto dStart = std::abs(px - startHandlePx);
    const auto dEnd = std::abs(px - endHandlePx);

    if (dStart <= grabRadiusPx && dStart <= dEnd)
        return ZonePart::StartHandle;
    if (dEnd <= grabRadiusPx)
        return ZonePart::EndHandle;
    return ZonePart::Body;
}

Gesture classifyGesture(ZonePart pressedPart, int maxTravelPx, int clickSlopPx) noexcept
{
    // A press that begins on a handle is always a zone edit, never an
    // audition — even with zero travel (task 043 disambiguation rule).
    if (pressedPart == ZonePart::StartHandle || pressedPart == ZonePart::EndHandle)
        return Gesture::HandleDrag;

    return maxTravelPx <= clickSlopPx ? Gesture::Click : Gesture::DragOut;
}

// ---------------------------------------------------------------------------
// ScopeFifo
// ---------------------------------------------------------------------------

ScopeFifo::ScopeFifo(int capacity)
    : fifo(std::max(capacity, 2)), storage(static_cast<std::size_t>(std::max(capacity, 2)), 0.0f)
{
}

void ScopeFifo::push(const float* samples, int numSamples) noexcept
{
    // Write what fits; drop the rest (never block the audio thread).
    const auto scope = fifo.write(std::min(numSamples, fifo.getFreeSpace()));
    for (int i = 0; i < scope.blockSize1; ++i)
        storage[static_cast<std::size_t>(scope.startIndex1 + i)] = samples[i];
    for (int i = 0; i < scope.blockSize2; ++i)
        storage[static_cast<std::size_t>(scope.startIndex2 + i)] =
            samples[scope.blockSize1 + i];
}

void ScopeFifo::pushDecimated(const float* samples, int numSamples, int stride) noexcept
{
    stride = std::max(stride, 1);
    const auto wanted = (numSamples + stride - 1) / stride;
    const auto scope = fifo.write(std::min(wanted, fifo.getFreeSpace()));

    int srcIndex = 0;
    for (int i = 0; i < scope.blockSize1; ++i, srcIndex += stride)
        storage[static_cast<std::size_t>(scope.startIndex1 + i)] = samples[srcIndex];
    for (int i = 0; i < scope.blockSize2; ++i, srcIndex += stride)
        storage[static_cast<std::size_t>(scope.startIndex2 + i)] = samples[srcIndex];
}

int ScopeFifo::pop(float* dest, int maxSamples) noexcept
{
    const auto scope = fifo.read(std::min(maxSamples, fifo.getNumReady()));
    for (int i = 0; i < scope.blockSize1; ++i)
        dest[i] = storage[static_cast<std::size_t>(scope.startIndex1 + i)];
    for (int i = 0; i < scope.blockSize2; ++i)
        dest[scope.blockSize1 + i] =
            storage[static_cast<std::size_t>(scope.startIndex2 + i)];
    return scope.blockSize1 + scope.blockSize2;
}

} // namespace mws::ui::waveform

// ===========================================================================
// WaveformView component
// ===========================================================================

namespace mws::ui {

namespace wf = waveform;

namespace {

/// Inner drawing area: the 2 px recessed bezel border is not part of the
/// pixel↔sample mapping.
constexpr int kBorderPx = 2;

juce::Rectangle<int> innerArea(const juce::Component& c)
{
    return c.getLocalBounds().reduced(kBorderPx);
}

} // namespace

WaveformView::WaveformView()
    : spec_(&faceplateSpecFor(model::ModelId::S1000))
{
    scopeRing.reserve(static_cast<std::size_t>(kScopeLength));
    setOpaque(false);
    startTimerHz(30);
}

WaveformView::~WaveformView() = default;

void WaveformView::setSpec(const FaceplateSpec& spec)
{
    spec_ = &spec;
    repaint();
}

// --- published buffers ------------------------------------------------------

void WaveformView::setSourceSample(std::shared_ptr<const core::AudioBuffer> sample)
{
    sourceSample = std::move(sample);

    // Stretch zone defaults to full length on load (ui-design §6.1 step 2);
    // silent — the load path, not the user, set it.
    zoneStartFrame = 0;
    zoneEndFrame = sourceFrames();
    playhead = -1;

    restartPeaksBuild();
    repaint();
}

void WaveformView::setRenderedSample(std::shared_ptr<const core::AudioBuffer> sample)
{
    renderedSample = std::move(sample);
    restartPeaksBuild();
    repaint();
}

std::int64_t WaveformView::sourceFrames() const noexcept
{
    return sourceSample == nullptr ? 0
                                   : static_cast<std::int64_t>(sourceSample->numFrames());
}

void WaveformView::finishPeaksBuild()
{
    sourcePeaks.buildAll();
    renderPeaks.buildAll();
    repaint();
}

bool WaveformView::isPeaksBuildComplete() const noexcept
{
    return sourcePeaks.isComplete() && renderPeaks.isComplete();
}

void WaveformView::restartPeaksBuild()
{
    const auto columns = std::max(innerArea(*this).getWidth(), 0);
    sourcePeaks.reset(sourceSample, columns);
    renderPeaks.reset(renderedSample, columns);
}

// --- stretch zone -------------------------------------------------------------

void WaveformView::setZone(std::int64_t startFrame, std::int64_t endFrame,
                           juce::NotificationType notification)
{
    const auto total = sourceFrames();
    startFrame = std::clamp<std::int64_t>(startFrame, 0, std::max<std::int64_t>(total - 1, 0));
    endFrame = std::clamp<std::int64_t>(endFrame, startFrame + 1, std::max<std::int64_t>(total, startFrame + 1));

    if (startFrame == zoneStartFrame && endFrame == zoneEndFrame)
        return;

    zoneStartFrame = startFrame;
    zoneEndFrame = endFrame;
    repaint();

    if (notification != juce::dontSendNotification)
        emitZoneChange();
}

void WaveformView::emitZoneChange()
{
    if (onZoneChange != nullptr)
        onZoneChange(zoneStartFrame, zoneEndFrame);
}

int WaveformView::handlePixel(std::int64_t frame) const noexcept
{
    const auto inner = innerArea(*this);
    return inner.getX() + wf::sampleToPixel(frame, inner.getWidth(), sourceFrames());
}

// --- A/B / play head / FX scope -----------------------------------------------

void WaveformView::setRenderProminent(bool renderOnTop)
{
    if (renderProminent == renderOnTop)
        return;
    renderProminent = renderOnTop;
    repaint();
}

void WaveformView::setPlayheadFrame(std::int64_t frame)
{
    const auto clamped = frame < 0 ? std::int64_t{ -1 }
                                   : std::min(frame, sourceFrames());
    if (playhead == clamped)
        return;
    playhead = clamped;
    repaint();
}

void WaveformView::setFxScopeMode(bool enabled)
{
    if (fxScopeMode == enabled)
        return;
    fxScopeMode = enabled;
    scopeRing.clear();
    repaint();
}

void WaveformView::attachScopeFifo(wf::ScopeFifo* fifoToPoll) noexcept
{
    scopeFifo = fifoToPoll;
}

// --- drag-and-drop -------------------------------------------------------------

bool WaveformView::isInterestedInFileDrag(const juce::StringArray& files)
{
    return wf::anySupportedAudioFile(files);
}

void WaveformView::fileDragEnter(const juce::StringArray&, int, int)
{
    dragHover = true;
    repaint();
}

void WaveformView::fileDragExit(const juce::StringArray&)
{
    dragHover = false;
    repaint();
}

void WaveformView::filesDropped(const juce::StringArray& files, int, int)
{
    dragHover = false;
    repaint();

    juce::StringArray supported;
    for (const auto& f : files)
        if (wf::isSupportedAudioFile(f))
            supported.add(f);

    if (! supported.isEmpty() && onFilesDropped != nullptr)
        onFilesDropped(supported);  // → FileLoader path, wired in 045b
}

// --- mouse gestures --------------------------------------------------------------

void WaveformView::mouseDown(const juce::MouseEvent& e)
{
    maxTravelPx = 0;
    dragOutFired = false;
    pressedPart = wf::ZonePart::Body;

    if (fxScopeMode || ! hasSource())
        return;  // no zone/audition surface without a sample (§6.4 scope mode)

    pressedPart = wf::hitTestZone(e.x, handlePixel(zoneStartFrame),
                                  handlePixel(zoneEndFrame), kHandleGrabRadiusPx);
}

void WaveformView::mouseDrag(const juce::MouseEvent& e)
{
    maxTravelPx = std::max(maxTravelPx, e.getDistanceFromDragStart());

    if (fxScopeMode || ! hasSource())
        return;

    const auto inner = innerArea(*this);
    const auto frame =
        wf::pixelToSample(e.x - inner.getX(), inner.getWidth(), sourceFrames());

    switch (wf::classifyGesture(pressedPart, maxTravelPx, kClickSlopPx))
    {
        case wf::Gesture::HandleDrag:
            if (pressedPart == wf::ZonePart::StartHandle)
                setZone(std::min(frame, zoneEndFrame - 1), zoneEndFrame);
            else
                setZone(zoneStartFrame, std::max(frame, zoneStartFrame + 1));
            break;

        case wf::Gesture::DragOut:
            // Drag-out export initiation: hook only at 043; ExportService
            // performs the real external drag in 045b. Only a render can be
            // dragged out (ui-design §6.3 step 4). Fires once per gesture.
            if (! dragOutFired && hasRender())
            {
                dragOutFired = true;
                if (onDragExport != nullptr)
                    onDragExport();
            }
            break;

        case wf::Gesture::Click:
            break;  // still within slop — nothing yet
    }
}

void WaveformView::mouseUp(const juce::MouseEvent& e)
{
    if (fxScopeMode || ! hasSource())
        return;

    if (wf::classifyGesture(pressedPart, maxTravelPx, kClickSlopPx)
        == wf::Gesture::Click)
    {
        // Plain click on the body → audition (architecture.md §7: audition is
        // always available from the UI). Wired to SamplePlayer in 045b.
        if (onAudition != nullptr)
        {
            const auto inner = innerArea(*this);
            onAudition(wf::pixelToSample(e.x - inner.getX(), inner.getWidth(),
                                         sourceFrames()));
        }
    }
}

// --- timer: time-sliced peaks build + scope poll ----------------------------------

void WaveformView::timerCallback()
{
    bool dirty = false;

    if (! isPeaksBuildComplete())
    {
        sourcePeaks.buildSlice();
        renderPeaks.buildSlice();
        dirty = true;
    }

    if (fxScopeMode && scopeFifo != nullptr && scopeFifo->numReady() > 0)
    {
        scopePopChunk.resize(static_cast<std::size_t>(scopeFifo->capacity()));
        const auto n = scopeFifo->pop(scopePopChunk.data(),
                                      static_cast<int>(scopePopChunk.size()));
        if (n > 0)
        {
            scopeRing.insert(scopeRing.end(), scopePopChunk.begin(),
                             scopePopChunk.begin() + n);
            if (scopeRing.size() > static_cast<std::size_t>(kScopeLength))
                scopeRing.erase(scopeRing.begin(),
                                scopeRing.end() - kScopeLength);
            dirty = true;
        }
    }

    if (dirty)
        repaint();
}

// --- painting -----------------------------------------------------------------------

void WaveformView::resized()
{
    restartPeaksBuild();
}

void WaveformView::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto inner = innerArea(*this).toFloat();

    // Recessed LCD-like window: bezel shadow frame + flat back.
    g.setColour(spec_->chassisEdge);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(spec_->lcdBack);
    g.fillRect(inner);

    if (fxScopeMode)
    {
        paintScope(g, inner);
    }
    else if (! hasSource())
    {
        paintIdleCaption(g, inner);
    }
    else
    {
        // A/B overlay: both layers always drawn; the prominent one solid and
        // on top, the other dimmed (ui-design §2 "render-overlay A/B").
        const auto inkAlpha = renderProminent && hasRender() ? 0.35f : 1.0f;
        const auto renderAlpha = renderProminent ? 1.0f : 0.35f;

        if (renderProminent && hasRender())
        {
            paintPeakLayer(g, inner, sourcePeaks, spec_->lcdInk, inkAlpha);
            paintPeakLayer(g, inner, renderPeaks, spec_->accent, renderAlpha);
        }
        else
        {
            if (hasRender())
                paintPeakLayer(g, inner, renderPeaks, spec_->accent, renderAlpha);
            paintPeakLayer(g, inner, sourcePeaks, spec_->lcdInk, inkAlpha);
        }

        paintZone(g, inner);
        paintPlayhead(g, inner);
    }

    // Drop hover highlight on top of everything (ui-design §6.1).
    if (dragHover)
    {
        g.setColour(spec_->accent);
        g.drawRect(getLocalBounds(), kBorderPx);
    }
}

void WaveformView::paintIdleCaption(juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setColour(dragHover ? spec_->accent : spec_->lcdInk.withAlpha(0.55f));
    g.setFont(SeriesLookAndFeel::legendFont(std::min(14.0f, area.getHeight() * 0.22f)));
    g.drawText("DROP SAMPLE HERE", area, juce::Justification::centred);
}

void WaveformView::paintScope(juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setColour(spec_->lcdInk.withAlpha(0.25f));
    g.drawHorizontalLine(static_cast<int>(area.getCentreY()), area.getX(),
                         area.getRight());

    if (scopeRing.size() < 2)
        return;

    const auto n = static_cast<int>(scopeRing.size());
    const auto half = area.getHeight() * 0.5f - 1.0f;

    juce::Path path;
    for (int i = 0; i < n; ++i)
    {
        const auto x = area.getX() + area.getWidth() * static_cast<float>(i)
                                         / static_cast<float>(n - 1);
        const auto v = std::clamp(scopeRing[static_cast<std::size_t>(i)], -1.0f, 1.0f);
        const auto y = area.getCentreY() - v * half;
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    g.setColour(spec_->lcdInk);
    g.strokePath(path, juce::PathStrokeType(1.2f));
}

void WaveformView::paintPeakLayer(juce::Graphics& g, juce::Rectangle<float> area,
                                  const wf::PeaksCache& peaks, juce::Colour colour,
                                  float alpha) const
{
    if (peaks.numColumnsBuilt() == 0)
        return;

    const auto half = area.getHeight() * 0.5f - 1.0f;
    const auto centreY = area.getCentreY();
    const auto x0 = static_cast<int>(area.getX());

    g.setColour(colour.withAlpha(alpha));
    const auto columns = std::min(peaks.numColumnsBuilt(),
                                  static_cast<int>(area.getWidth()));
    for (int i = 0; i < columns; ++i)
    {
        const auto c = peaks.column(i);
        // Snapped to whole pixels: crisp LCD-style columns, no AA smear.
        const auto yTop = static_cast<int>(
            std::floor(centreY - std::clamp(c.maxValue, -1.0f, 1.0f) * half));
        const auto yBot = static_cast<int>(
            std::ceil(centreY - std::clamp(c.minValue, -1.0f, 1.0f) * half));
        g.fillRect(x0 + i, yTop, 1, std::max(yBot - yTop, 1));
    }
}

void WaveformView::paintZone(juce::Graphics& g, juce::Rectangle<float> area) const
{
    const auto startX = static_cast<float>(handlePixel(zoneStartFrame));
    const auto endX = static_cast<float>(handlePixel(zoneEndFrame));

    // Dim outside the stretch zone.
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    if (startX > area.getX())
        g.fillRect(area.getX(), area.getY(), startX - area.getX(), area.getHeight());
    if (endX < area.getRight())
        g.fillRect(endX, area.getY(), area.getRight() - endX, area.getHeight());

    // The two handles: solid accent line + grab tab at the top.
    g.setColour(spec_->accent);
    for (const auto x : { startX, endX })
    {
        g.fillRect(x - 1.0f, area.getY(), 2.0f, area.getHeight());
        g.fillRect(x - 4.0f, area.getY(), 8.0f, 5.0f);
    }
}

void WaveformView::paintPlayhead(juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (playhead < 0)
        return;

    const auto x = static_cast<float>(handlePixel(playhead));
    g.setColour(spec_->legend.withAlpha(0.9f));
    g.fillRect(x, area.getY(), 1.0f, area.getHeight());
}

} // namespace mws::ui
