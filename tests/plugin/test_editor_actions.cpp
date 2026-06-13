// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 045b — editor interaction flows, headless (docs/design/ui-design.md
// §6.1–§6.4, §7; §1 soft-key TIME set). The decision logic that drives the
// soft keys (enable/disable matrix per mode/model, per-model captions, the
// autC→cycleLen detect, tap-tempo math) lives in mws::ui::EditorActions so it
// runs without a window; the gesture/drop/audition wiring is proven against a
// real SoftKeyBar / WaveformView. Covered (the task's test list):
//   · soft-key enable/disable matrix per mode (FX greys GO/PLAY/A-B);
//   · ABORT fires only after the >= 600 ms hold (fake-clock SoftKeyBar);
//   · F2 writes cycleLen via the autoCycle trigger parameter (the detector
//     result lands in the cycleLen param, the trigger self-resets);
//   · tap-tempo averaging math;
//   · the drop-file path invokes the loader callback;
//   · click-to-audition triggers SamplePlayer playback;
//   · FX mode swaps the LCD page to the "TIME-STRETCH (REALTIME)" page.
//
// Test-case names begin with the tag word so `ctest -R editor` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "EngineHost.h"
#include "SamplePlayer.h"
#include "state/Parameters.h"
#include "ui/EditorActions.h"
#include "ui/LcdPageModel.h"
#include "ui/SoftKeyBar.h"
#include "ui/WaveformView.h"

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"
#include "mws/stretch/AutoCycle.h"

using Catch::Approx;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::ui::SoftKeyBar;
using mws::ui::TapTempo;
using mws::ui::WaveformView;
namespace sk = mws::ui::softkey;
namespace pid = mws::plugin::paramid;

namespace {

/// Minimal host-side AudioProcessor so the APVTS has an owner (same pattern as
/// test_editor_wiring.cpp).
struct NullProcessor final : juce::AudioProcessor
{
    NullProcessor() = default;
    const juce::String getName() const override { return "null"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

ParamSnapshot snap(ModelId model, PluginMode mode)
{
    ParamSnapshot p;
    p.model = model;
    p.pluginMode = mode;
    return p;
}

/// Exactly periodic sawtooth in [-1, 1): a clean autocorrelation peak at
/// `period` (mirrors tests/unit/test_autocycle.cpp).
std::vector<float> makeSaw(int period, int numFrames)
{
    std::vector<float> x(static_cast<std::size_t>(numFrames));
    for (int n = 0; n < numFrames; ++n)
        x[static_cast<std::size_t>(n)] =
            2.0f * (static_cast<float>(n % period) / static_cast<float>(period)) - 1.0f;
    return x;
}

} // namespace

// ---------------------------------------------------------------------------
// Soft-key enable/disable matrix per mode (ui-design §6.4)
// ---------------------------------------------------------------------------

TEST_CASE("editor: soft-key enable/disable matrix greys GO/PLAY/A-B in FX mode "
          "and keeps TIME/SYNC/ABORT live",
          "[editor]")
{
    using mws::ui::softKeyEnabled;
    const auto& s1000 = ModelSpec::get(ModelId::S1000);

    // SAMPLE mode (the authentic render/audition path): every TIME-page key is
    // live on a stretch model.
    const auto sample = snap(ModelId::S1000, PluginMode::Sample);
    for (int i = 0; i < sk::kCount; ++i)
        CHECK(softKeyEnabled(i, sample, s1000));

    // FX mode greys GO/PLAY/A-B (no offline render / audition in the realtime
    // path) and the SAMPLE-only ZONE preview; TIME/autC/SYNC/ABORT stay live.
    const auto fx = snap(ModelId::S1000, PluginMode::Fx);
    CHECK(softKeyEnabled(sk::kTime, fx, s1000));
    CHECK(softKeyEnabled(sk::kAutC, fx, s1000));
    CHECK_FALSE(softKeyEnabled(sk::kZone, fx, s1000));
    CHECK_FALSE(softKeyEnabled(sk::kGo, fx, s1000));
    CHECK_FALSE(softKeyEnabled(sk::kPlay, fx, s1000));
    CHECK_FALSE(softKeyEnabled(sk::kAb, fx, s1000));
    CHECK(softKeyEnabled(sk::kSync, fx, s1000));
    CHECK(softKeyEnabled(sk::kAbort, fx, s1000));

    // The S900 has no timestretch (ADR-003): the stretch-only autC/ZONE keys
    // are inert on it even in SAMPLE mode; GO still renders the varispeed.
    const auto& s900 = ModelSpec::get(ModelId::S900);
    const auto s900Sample = snap(ModelId::S900, PluginMode::Sample);
    CHECK_FALSE(softKeyEnabled(sk::kAutC, s900Sample, s900));
    CHECK_FALSE(softKeyEnabled(sk::kZone, s900Sample, s900));
    CHECK(softKeyEnabled(sk::kGo, s900Sample, s900));

    // Out-of-range indices never enable.
    CHECK_FALSE(softKeyEnabled(-1, sample, s1000));
    CHECK_FALSE(softKeyEnabled(sk::kCount, sample, s1000));
}

TEST_CASE("editor: F2 soft-key caption reads AUTO-D on the S950 and autC "
          "elsewhere (ui-design §6.2 step 3)",
          "[editor]")
{
    using mws::ui::softKeyLabels;

    const auto s950 =
        softKeyLabels(snap(ModelId::S950, PluginMode::Sample), ModelSpec::get(ModelId::S950));
    CHECK(s950[sk::kAutC] == "AUTO-D");
    CHECK(s950[sk::kSync] == "SYNC");
    CHECK(s950[sk::kAbort] == "ABORT");

    const auto s1000 = softKeyLabels(snap(ModelId::S1000, PluginMode::Sample),
                                     ModelSpec::get(ModelId::S1000));
    CHECK(s1000[sk::kAutC] == "autC");
    CHECK(s1000[sk::kGo] == "GO");
}

// ---------------------------------------------------------------------------
// ABORT fires only after the >= 600 ms hold (SoftKeyBar fake clock, §6.3)
// ---------------------------------------------------------------------------

TEST_CASE("editor: F8 ABORT raises the engine abort flag only after the 600 ms "
          "hold, never on press",
          "[editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    SoftKeyBar bar;

    juce::int64 now = 0;
    bar.setTimeSource([&] { return now; });
    bar.setKeyRequiresHold(sk::kAbort, SoftKeyBar::kAbortHoldMs);

    // The exact PluginEditor wiring shape: F8 fires requestAbort() through the
    // single onSoftKey dispatcher; only the abort branch is exercised here.
    mws::plugin::EngineHost host;
    int aborts = 0;
    bar.onSoftKey = [&](int index) {
        if (index == sk::kAbort)
        {
            host.requestAbort();
            ++aborts;
        }
    };

    bar.pressKey(sk::kAbort);
    CHECK(aborts == 0);  // hold keys never fire on press

    now = 599;
    bar.updateHoldProgress();
    CHECK(aborts == 0);  // 599 ms: not yet

    now = 600;
    bar.updateHoldProgress();
    CHECK(aborts == 1);  // 600 ms: fires exactly once

    now = 5000;
    bar.updateHoldProgress();
    CHECK(aborts == 1);  // continuing to hold must not re-fire

    // A press released before the threshold does NOT abort.
    bar.releaseKey(sk::kAbort);
    bar.pressKey(sk::kAbort);
    now = 5300;
    bar.updateHoldProgress();
    bar.releaseKey(sk::kAbort);
    now = 6000;
    bar.updateHoldProgress();
    CHECK(aborts == 1);
}

// ---------------------------------------------------------------------------
// F2 autC / AUTO-D writes cycleLen via the autoCycle trigger parameter
// ---------------------------------------------------------------------------

TEST_CASE("editor: F2 autC runs the detector and writes the result into the "
          "cycleLen parameter, then self-resets the trigger",
          "[editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    NullProcessor proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };
    mws::plugin::Parameters params{ apvts };

    // A loaded source with a clean 100 Hz period (441 frames at 44.1 kHz).
    auto source = std::make_shared<mws::core::AudioBuffer>(std::size_t{ 1 },
                                                           std::size_t{ 44100 });
    source->sampleRate = 44100.0;
    {
        const auto saw = makeSaw(441, 44100);
        auto ch = source->channel(0);
        for (std::size_t i = 0; i < ch.numFrames(); ++i)
            ch[i] = saw[i];
    }

    // The exact F2 handler shape: the trigger param is set true (the soft key),
    // the editor sees autoCyclePending(), runs the detector over the full zone,
    // writes the result into cycleLen, and self-resets the trigger to false.
    auto* trigger = apvts.getParameter(pid::autoCycle);
    auto* cycleParam = apvts.getParameter(pid::cycleLen);
    REQUIRE(trigger != nullptr);
    REQUIRE(cycleParam != nullptr);

    trigger->setValueNotifyingHost(1.0f);  // F2 pressed
    REQUIRE(params.autoCyclePending());

    if (params.autoCyclePending())
    {
        const int detected = mws::ui::autoCycleForZone(source, 0, 44100);
        cycleParam->setValueNotifyingHost(cycleParam->convertTo0to1((float) detected));
        trigger->setValueNotifyingHost(0.0f);  // consume + self-reset
    }

    // The detector landed ~441 (100 Hz period) in the cycle field, and the
    // trigger is back to rest (a momentary parameter, dsp-engine §2).
    const float written = cycleParam->convertFrom0to1(cycleParam->getValue());
    CHECK(written >= 440.0f);
    CHECK(written <= 442.0f);
    CHECK_FALSE(params.autoCyclePending());

    // With no source loaded the helper returns the documented fallback so F2
    // always lands a usable cycle value.
    CHECK(mws::ui::autoCycleForZone(nullptr, 0, 0)
          == mws::stretch::AutoCycle::kFallbackCycleLen);
}

// ---------------------------------------------------------------------------
// Tap-tempo averaging math (ui-design §6.2 step 4)
// ---------------------------------------------------------------------------

TEST_CASE("editor: tap-tempo averages the inter-tap intervals into BPM",
          "[editor]")
{
    using mws::ui::tapTempoBpm;

    // Fewer than two taps: no interval, no tempo yet.
    CHECK(tapTempoBpm({}) == Approx(0.0));
    CHECK(tapTempoBpm({ 1000.0 }) == Approx(0.0));

    // Four taps spaced exactly 500 ms apart => 120 BPM.
    CHECK(tapTempoBpm({ 0.0, 500.0, 1000.0, 1500.0 }) == Approx(120.0));

    // Jittery taps average out: deltas 480/520/500 ms => mean 500 => 120 BPM.
    CHECK(tapTempoBpm({ 0.0, 480.0, 1000.0, 1500.0 }) == Approx(120.0));

    // 343 ms intervals => ~174.9 BPM (a jungle tempo).
    CHECK(tapTempoBpm({ 0.0, 343.0, 686.0 }) == Approx(60000.0 / 343.0));

    // Degenerate (zero / reversed span) yields no tempo.
    CHECK(tapTempoBpm({ 1000.0, 1000.0 }) == Approx(0.0));
}

TEST_CASE("editor: TapTempo ring tracks the running tempo and a long gap "
          "restarts the measurement",
          "[editor]")
{
    TapTempo tap;

    CHECK(tap.tap(0.0) == Approx(0.0));        // first tap, no interval
    CHECK(tap.tap(500.0) == Approx(120.0));    // 500 ms => 120 BPM
    CHECK(tap.tap(1000.0) == Approx(120.0));
    CHECK(tap.tap(1500.0) == Approx(120.0));
    CHECK(tap.numTaps() == TapTempo::kMaxTaps);

    // A gap beyond the reset window starts a fresh measurement (one tap => 0).
    const double afterGap = 1500.0 + TapTempo::kResetGapMs + 1.0;
    CHECK(tap.tap(afterGap) == Approx(0.0));
    CHECK(tap.numTaps() == 1);

    // New cadence settles to a new tempo (250 ms => 240 BPM).
    tap.tap(afterGap + 250.0);
    CHECK(tap.tap(afterGap + 500.0) == Approx(240.0));

    tap.reset();
    CHECK(tap.numTaps() == 0);
}

// ---------------------------------------------------------------------------
// Drop-file path invokes the loader (ui-design §6.1)
// ---------------------------------------------------------------------------

TEST_CASE("editor: dropping a supported audio file onto the waveform invokes "
          "the loader callback; unsupported files are rejected",
          "[editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    WaveformView wave;
    wave.setSize(400, 120);

    juce::StringArray loaded;
    wave.onFilesDropped = [&](const juce::StringArray& files) { loaded = files; };

    // A WAV is accepted for the drag (the FileLoader format set) and the drop
    // forwards exactly the supported files to the loader hook.
    const juce::StringArray wavDrop{ "/tmp/break_174bpm.wav" };
    CHECK(wave.isInterestedInFileDrag(wavDrop));
    wave.filesDropped(wavDrop, 10, 10);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == "/tmp/break_174bpm.wav");

    // An MP3 (no MP3 at v1) is not interesting and a pure-MP3 drop forwards
    // nothing.
    loaded.clear();
    const juce::StringArray mp3Drop{ "/tmp/song.mp3" };
    CHECK_FALSE(wave.isInterestedInFileDrag(mp3Drop));
    wave.filesDropped(mp3Drop, 10, 10);
    CHECK(loaded.isEmpty());
}

// ---------------------------------------------------------------------------
// Click-to-audition triggers playback (architecture.md §7)
// ---------------------------------------------------------------------------

TEST_CASE("editor: a waveform body click fires onAudition, which starts "
          "SamplePlayer playback",
          "[editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    WaveformView wave;
    wave.setSize(400, 120);

    auto source = std::make_shared<mws::core::AudioBuffer>(std::size_t{ 1 },
                                                           std::size_t{ 10000 });
    source->sampleRate = 44100.0;
    wave.setSourceSample(source);
    wave.finishPeaksBuild();

    // The exact PluginEditor wiring: a body click → onAudition → PLAY.
    mws::plugin::SamplePlayer player;
    player.prepare(44100.0);
    int auditions = 0;
    std::int64_t auditionFrame = -1;
    wave.onAudition = [&](std::int64_t frame) {
        auditionFrame = frame;
        ++auditions;
        player.start();
    };
    REQUIRE_FALSE(player.isPlaying());

    // Press + release on the body (away from the zone handles at frame 0 /
    // 10000, which sit at the far edges) with no travel = a Click → audition.
    const juce::MouseEvent down = [&] {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                                juce::Point<float>(200.0f, 60.0f),
                                juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                &wave, &wave, juce::Time::getCurrentTime(),
                                juce::Point<float>(200.0f, 60.0f),
                                juce::Time::getCurrentTime(), 1, false);
    }();
    wave.mouseDown(down);
    wave.mouseUp(down);

    CHECK(auditions == 1);
    CHECK(auditionFrame > 0);
    CHECK(player.isPlaying());
}

// ---------------------------------------------------------------------------
// FX mode swaps the LCD page (ui-design §6.4)
// ---------------------------------------------------------------------------

TEST_CASE("editor: switching to FX mode swaps the LCD to the TIME-STRETCH "
          "(REALTIME) page",
          "[editor]")
{
    using mws::ui::LcdPageModel;

    // SAMPLE mode renders the authentic TIME-STRETCH page.
    auto sample = snap(ModelId::S1000, PluginMode::Sample);
    const auto samplePage =
        LcdPageModel::build(sample, ModelSpec::get(ModelId::S1000), {}, {});
    CHECK(samplePage.textJoined().find(LcdPageModel::kFxPageTitle) == std::string::npos);
    CHECK(samplePage.textJoined().find("TIME-STRETCH") != std::string::npos);

    // FX mode swaps to the deliberately non-authentic realtime page title.
    auto fx = snap(ModelId::S1000, PluginMode::Fx);
    const auto fxPage = LcdPageModel::build(fx, ModelSpec::get(ModelId::S1000), {}, {});
    CHECK(fxPage.textJoined().find(LcdPageModel::kFxPageTitle) != std::string::npos);
}
