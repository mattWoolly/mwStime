// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 046 — model-switching behavior (docs/design/ui-design.md §6.5, both
// steps; architecture.md §6 ranges-never-change / clamp-at-engine, §5.2 model
// switch = PDC-updating action; testing-strategy.md §6 "latency re-report on
// model switch honored"). Covered:
//   · clamp-memory round-trip (S1000 T=1500 → S950 [LCD 999] → S1000 [1500]),
//     headless on the task-045b ClampMemory + the task-041 LcdPageModel;
//   · the clamp map mirrors into / restores from the task-029 state-tree
//     `clampMemory` field (survives save/reload);
//   · latency re-report observed when the model changes in FX mode (the
//     EngineHost reconfigure path), and NOT on an automatable-only change;
//   · the FX model-switch one-block engine cross-fade: a processBlock that
//     adopts a new engine produces no hard discontinuity and allocates nothing
//     on the audio thread;
//   · all four shipping faceplates carry distinct palettes + the right LCD
//     layout class, and the Faceplate animates a 150 ms cross-fade on switch.
//
// Test-case names begin with "modelswitch" so `ctest -R modelswitch` selects
// them (plan/backlog/README.md test-selection rules). The global operator-new
// replacement (TestAllocationCounter.h) is provided by test_alloc_counter.cpp.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "EngineHost.h"
#include "TestAllocationCounter.h"
#include "state/StateTree.h"
#include "ui/EditorActions.h"
#include "ui/Faceplate.h"
#include "ui/FaceplateSpec.h"
#include "ui/LcdPageModel.h"

#include "mws/engine/Params.h"
#include "mws/model/ModelSpec.h"

using Catch::Approx;
using mws::engine::ParamSnapshot;
using mws::engine::PluginMode;
using mws::engine::RealtimeStretcher;
using mws::engine::SampleRateSel;
using mws::model::ModelId;
using mws::model::ModelSpec;
using mws::plugin::EngineHost;
using mws::ui::ClampMemory;
using mws::ui::LcdPage;
using mws::ui::LcdPageModel;
using mws::ui::LcdSampleInfo;
namespace st = mws::plugin::state;

namespace {

bool pageContains(const LcdPage& page, const std::string& needle)
{
    return page.textJoined().find(needle) != std::string::npos;
}

ParamSnapshot fxParams(ModelId model, double timeFactor, bool character = false)
{
    ParamSnapshot p;
    p.model = model;
    p.bandwidth = 19.2;
    p.sampleRateSel = SampleRateSel::Fs44100;
    p.character = character;
    p.timeFactor = timeFactor;
    p.pluginMode = PluginMode::Fx;
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Clamp memory round-trip (ui-design §6.5 step 1, PI)
// ---------------------------------------------------------------------------

TEST_CASE("modelswitch: clamp memory remembers a pre-clamp timeFactor and "
          "restores it (S1000 1500 -> S950 [999] -> S1000 [1500])",
          "[modelswitch]")
{
    ClampMemory memory;

    // S1000 holds 1500% (well within the 25..2000 superset; S1000 caps at 2000).
    double host = 1500.0;
    CHECK(ModelSpec::get(ModelId::S1000).clamp(fxParams(ModelId::S1000, host)).timeFactor
          == Approx(1500.0));

    // Switch S1000 -> S950: the engine clamps to 999%, which is written to the
    // (range-fixed) host parameter, while 1500 is remembered under S1000.
    host = mws::ui::applyModelSwitchTimeFactor(memory, ModelId::S1000, ModelId::S950, host);
    CHECK(host == Approx(999.0));
    CHECK(memory.recall(ModelId::S1000, -1.0) == Approx(1500.0));

    // The LCD shows the clamped 999% on the S950 page (LcdPageModel is the
    // single display authority; it clamps internally too).
    {
        LcdSampleInfo sample;
        const auto page = LcdPageModel::build(fxParams(ModelId::S950, host),
                                              ModelSpec::get(ModelId::S950), sample, {});
        CHECK(pageContains(page, "999%"));
        CHECK_FALSE(pageContains(page, "1500"));
    }

    // Switch back S950 -> S1000: the remembered 1500 is restored to the host
    // parameter (S1000 does not clamp it).
    host = mws::ui::applyModelSwitchTimeFactor(memory, ModelId::S950, ModelId::S1000, host);
    CHECK(host == Approx(1500.0));

    // And the S1000 page shows 1500% again.
    LcdSampleInfo sample;
    const auto page = LcdPageModel::build(fxParams(ModelId::S1000, host),
                                          ModelSpec::get(ModelId::S1000), sample, {});
    CHECK(pageContains(page, "1500%"));
}

TEST_CASE("modelswitch: a value within the new model's range carries across "
          "unchanged (no spurious clamp)",
          "[modelswitch]")
{
    ClampMemory memory;
    // 800% is in range for every shipping model — switching never alters it.
    double host = 800.0;
    host = mws::ui::applyModelSwitchTimeFactor(memory, ModelId::S1000, ModelId::S950, host);
    CHECK(host == Approx(800.0));
    host = mws::ui::applyModelSwitchTimeFactor(memory, ModelId::S950, ModelId::S1100, host);
    CHECK(host == Approx(800.0));
}

// ---------------------------------------------------------------------------
// State-tree mirror: the memory map survives save/reload (ui-design §6.5 PI;
// the schema is the task-029 `clampMemory` field).
// ---------------------------------------------------------------------------

TEST_CASE("modelswitch: the clamp map mirrors into the state tree and restores "
          "from it (survives save/reload)",
          "[modelswitch]")
{
    // The editor mirrors ClampMemory <-> the state-tree clampMemory field on
    // every switch; model that bridge here without a window.
    ClampMemory memory;
    double host = 1500.0;
    host = mws::ui::applyModelSwitchTimeFactor(memory, ModelId::S1000, ModelId::S950, host);
    REQUIRE(memory.has(ModelId::S1000));

    // Mirror -> state tree (what handleModelSwitch writes).
    auto tree = st::createDefault();
    for (const auto m : mws::model::kAllModels)
        if (memory.has(m))
            st::setClampMemory(tree, m, memory.recall(m, 0.0));

    // Save + reload the full plugin state.
    juce::ScopedJuceInitialiser_GUI juceInit;
    struct NullProc final : juce::AudioProcessor
    {
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
    } proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                             juce::AudioProcessorValueTreeState::ParameterLayout{} };

    juce::MemoryBlock blob;
    st::writePluginState(apvts.copyState(), tree, blob);
    REQUIRE(blob.getSize() > 0);

    const auto restored = st::readPluginState(blob.getData(),
                                              static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);

    // The reloaded tree carries the memory map; the editor seeds ClampMemory
    // back from it on construction.
    CHECK(st::getClampMemory(restored.stateTree, ModelId::S1000, -1.0) == Approx(1500.0));

    ClampMemory reloaded;
    for (const auto m : mws::model::kAllModels)
    {
        const double v = st::getClampMemory(restored.stateTree, m, -1.0);
        if (v > 0.0)
            reloaded.remember(m, v);
    }
    CHECK(reloaded.has(ModelId::S1000));
    CHECK(reloaded.recall(ModelId::S1000, -1.0) == Approx(1500.0));

    // A reloaded S950 still restores 1500 when switching back to S1000.
    double host2 = 999.0;  // the S950 clamp value that was in effect
    host2 = mws::ui::applyModelSwitchTimeFactor(reloaded, ModelId::S950,
                                                ModelId::S1000, host2);
    CHECK(host2 == Approx(1500.0));
}

// ---------------------------------------------------------------------------
// Latency re-report on model switch (architecture.md §5.2 / testing-strategy §6)
// ---------------------------------------------------------------------------

TEST_CASE("modelswitch: an FX model switch re-reports latency; an automatable "
          "change does not",
          "[modelswitch]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    EngineHost host;
    const int base = host.prepareFx(kRate, kBlock, /*channels=*/2,
                                    fxParams(ModelId::S1000, 100.0));
    REQUIRE(base > 0);

    // Automatable-only change (timeFactor): NO reconfigure / re-report.
    REQUIRE_FALSE(host.reconfigureFxIfNeeded(fxParams(ModelId::S1000, 400.0)));

    // Model change S1000 -> S950: reconfigure + latency re-report.
    REQUIRE(host.reconfigureFxIfNeeded(fxParams(ModelId::S950, 100.0)));
    REQUIRE(host.fxLatencySamples() > 0);

    // Model change S950 -> S1100: re-report again.
    REQUIRE(host.reconfigureFxIfNeeded(fxParams(ModelId::S1100, 100.0)));
    REQUIRE(host.fxLatencySamples() > 0);

    host.collectFxGarbage();
}

// ---------------------------------------------------------------------------
// FX model-switch one-block engine cross-fade (ui-design §6.5 step 2, PI)
// ---------------------------------------------------------------------------

TEST_CASE("modelswitch: the FX model switch cross-fades engines over one block "
          "(no hard discontinuity)",
          "[modelswitch]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    EngineHost host;
    const auto start = fxParams(ModelId::S1000, 100.0, /*character=*/false);
    host.prepareFx(kRate, kBlock, /*channels=*/1, start);

    // A slowly-varying input (low-frequency sine) so the dry-through signal's
    // own sample-to-sample steps are tiny; any large step in the output would be
    // the engine swap, which the one-block cross-fade is there to smooth.
    auto sine = [](std::int64_t n) {
        return 0.5f * std::sin(2.0 * M_PI * 50.0 * static_cast<double>(n) / kRate);
    };

    std::vector<float> buf(static_cast<std::size_t>(kBlock));
    std::int64_t pos = 0;
    auto runBlock = [&](const ParamSnapshot& p) {
        for (int i = 0; i < kBlock; ++i)
            buf[static_cast<std::size_t>(i)] = sine(pos + i);
        float* chans[1] = { buf.data() };
        host.processFxBlock(chans, 1, kBlock, p, RealtimeStretcher::TransportInfo{});
        pos += kBlock;
    };

    // Warm the S1000 engine past its pre-roll so its output is the live (delayed)
    // signal rather than start-up silence.
    const int warm = host.fxLatencySamples() / kBlock + 4;
    for (int b = 0; b < warm; ++b)
        runBlock(start);

    const float lastBeforeSwitch = buf[static_cast<std::size_t>(kBlock - 1)];

    // Publish the S950 reconfiguration (message thread) — the audio thread adopts
    // it at the next processFxBlock and cross-fades the engines over that block.
    auto s950 = fxParams(ModelId::S950, 100.0, /*character=*/false);
    REQUIRE(host.reconfigureFxIfNeeded(s950));

    // The switch block.
    for (int i = 0; i < kBlock; ++i)
        buf[static_cast<std::size_t>(i)] = sine(pos + i);
    float* chans[1] = { buf.data() };
    host.processFxBlock(chans, 1, kBlock, s950, RealtimeStretcher::TransportInfo{});

    // No hard step: the boundary from the previous block's last sample into this
    // block's first, and every step within the switch block, stays below a
    // -40 dBFS level step (0.01). A bare engine swap (fresh history ring) would
    // step the first sample straight to ~0 — a step of |lastBeforeSwitch| ~ 0.5.
    constexpr float kStepThresh = 0.01f;  // -40 dBFS
    CHECK(std::abs(buf[0] - lastBeforeSwitch) < kStepThresh);
    float maxStep = 0.0f;
    for (int i = 1; i < kBlock; ++i)
        maxStep = std::max(maxStep,
                           std::abs(buf[static_cast<std::size_t>(i)]
                                    - buf[static_cast<std::size_t>(i - 1)]));
    CHECK(maxStep < kStepThresh);

    host.collectFxGarbage();
}

TEST_CASE("modelswitch: the FX model-switch cross-fade allocates nothing on the "
          "audio thread",
          "[modelswitch]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    EngineHost host;
    const auto start = fxParams(ModelId::S1000, 100.0, /*character=*/false);
    host.prepareFx(kRate, kBlock, /*channels=*/2, start);

    std::vector<float> l(static_cast<std::size_t>(kBlock), 0.25f);
    std::vector<float> r(static_cast<std::size_t>(kBlock), 0.25f);
    float* chans[2] = { l.data(), r.data() };

    // Warm up (first-touch lazy allocation out of the way).
    for (int b = 0; b < 8; ++b)
        host.processFxBlock(chans, 2, kBlock, start, RealtimeStretcher::TransportInfo{});

    // Build + publish the new engine on the MESSAGE thread (allocation here is
    // expected and fine — the heavy 30 s ring is built off the audio thread).
    auto s950 = fxParams(ModelId::S950, 100.0, /*character=*/false);
    REQUIRE(host.reconfigureFxIfNeeded(s950));

    // The audio-thread adoption + one-block cross-fade must allocate nothing
    // (the fade scratch + view array were preallocated in prepareFx).
    const auto before = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    host.processFxBlock(chans, 2, kBlock, s950, RealtimeStretcher::TransportInfo{});
    const auto after = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    CHECK(after == before);

    host.collectFxGarbage();
}

// ---------------------------------------------------------------------------
// Faceplate palettes + LCD layouts + the 150 ms cross-fade (ui-design §6.5 §3)
// ---------------------------------------------------------------------------

TEST_CASE("modelswitch: all four shipping faceplates carry distinct palettes and "
          "the correct LCD layout class",
          "[modelswitch]")
{
    using mws::ui::faceplateSpecFor;
    using mws::ui::LcdLayout;

    const auto& s900 = faceplateSpecFor(ModelId::S900);
    const auto& s950 = faceplateSpecFor(ModelId::S950);
    const auto& s1000 = faceplateSpecFor(ModelId::S1000);
    const auto& s1100 = faceplateSpecFor(ModelId::S1100);

    // Distinct accent palettes (ui-design §3 table).
    CHECK(s900.accent != s950.accent);
    CHECK(s950.accent != s1000.accent);
    CHECK(s1000.accent != s1100.accent);

    // The varclock 2-line machines vs the S1000-family page (ui-design §6.5
    // S950_2LINE <-> S1000_PAGE re-layout).
    CHECK(s900.lcdLayout == LcdLayout::S950_2Line);
    CHECK(s950.lcdLayout == LcdLayout::S950_2Line);
    CHECK(s1000.lcdLayout == LcdLayout::S1000_Page);
    CHECK(s1100.lcdLayout == LcdLayout::S1000_Page);
}

TEST_CASE("modelswitch: the Faceplate animates a 150 ms palette cross-fade then "
          "settles on the new model",
          "[modelswitch]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    mws::ui::Faceplate plate;  // defaults to S1000
    plate.setSize(mws::ui::geometry::kBaseWidth, mws::ui::geometry::kBaseHeight);
    REQUIRE(plate.model() == ModelId::S1000);
    CHECK_FALSE(plate.isCrossfading());

    // A cross-fade to S950 swaps the active model immediately and animates.
    plate.crossfadeToModel(ModelId::S950);
    CHECK(plate.model() == ModelId::S950);
    CHECK(plate.isCrossfading());

    // The fade duration is exactly the §6.5 (PI) 150 ms.
    CHECK(mws::ui::Faceplate::kCrossfadeMs == 150);

    // Switching to the same model is a no-op (no animation kicked off).
    mws::ui::Faceplate plate2;
    plate2.setSize(mws::ui::geometry::kBaseWidth, mws::ui::geometry::kBaseHeight);
    plate2.crossfadeToModel(ModelId::S1000);  // already S1000
    CHECK_FALSE(plate2.isCrossfading());

    // setModel is the instant path (no animation).
    plate2.setModel(ModelId::S1100);
    CHECK(plate2.model() == ModelId::S1100);
    CHECK_FALSE(plate2.isCrossfading());
}
