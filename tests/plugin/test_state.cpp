// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 029 — non-parameter state tree, stateVersion and migrations
// (architecture.md §6): full round-trip of params + state tree, stateVersion
// written/read, explicit migration dispatch (fake v0 upgraded, unknown future
// version falls back), and defaults when fields are missing. Tag: [state].

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include "state/Migrations.h"
#include "state/Parameters.h"
#include "state/StateTree.h"

using Catch::Approx;
namespace st = mws::plugin::state;
namespace pid = mws::plugin::paramid;

namespace {

/// Minimal host-side AudioProcessor so the APVTS has an owner.
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

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    NullProcessor proc;
    juce::AudioProcessorValueTreeState apvts{ proc, nullptr, "PARAMETERS",
                                              mws::plugin::createParameterLayout() };

    void setPlain(const char* id, float plainValue)
    {
        auto* p = apvts.getParameter(id);
        REQUIRE(p != nullptr);
        p->setValueNotifyingHost(p->convertTo0to1(plainValue));
    }

    float plain(const char* id)
    {
        auto* p = apvts.getParameter(id);
        REQUIRE(p != nullptr);
        return p->convertFrom0to1(p->getValue());
    }
};

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("state: default tree carries stateVersion 1 and every schema field", "[state]")
{
    auto tree = st::createDefault();

    CHECK(static_cast<int>(tree.getProperty(st::id::stateVersion)) == st::kStateVersion);
    CHECK(st::kStateVersion == 1);

    CHECK(tree.getProperty(st::id::mode).toString() == "FX");
    CHECK(static_cast<bool>(tree.getProperty(st::id::embedAudio)) == true); // default ON (dsp-engine.md §2)
    CHECK(static_cast<double>(tree.getProperty(st::id::sourceBPM)) == Approx(0.0));
    CHECK(static_cast<double>(tree.getProperty(st::id::zoneStart)) == Approx(0.0));
    CHECK(static_cast<double>(tree.getProperty(st::id::zoneEnd)) == Approx(1.0));

    const auto sourceFile = tree.getChildWithName(st::id::sourceFile);
    REQUIRE(sourceFile.isValid());
    CHECK(sourceFile.getProperty(st::id::path).toString().isEmpty());
    CHECK(sourceFile.getProperty(st::id::contentHash).toString().isEmpty());

    const auto uiState = tree.getChildWithName(st::id::uiState);
    REQUIRE(uiState.isValid());
    CHECK(static_cast<double>(uiState.getProperty(st::id::scaleFactor)) == Approx(1.0));
    CHECK(static_cast<int>(uiState.getProperty(st::id::lcdPage)) == 0);

    CHECK(tree.getChildWithName(st::id::clampMemory).isValid());

    // renderMeta only appears once a render exists.
    CHECK_FALSE(tree.getChildWithName(st::id::renderMeta).isValid());
    CHECK_FALSE(st::hasRenderMetadata(tree));
}

TEST_CASE("state: full round-trip of params + state tree", "[state]")
{
    Fixture src;

    // Non-default parameter values.
    src.setPlain(pid::timeFactor, 300.0f);
    src.setPlain(pid::cycleLen, 256.0f);
    src.setPlain(pid::transpose, -7.5f);
    src.setPlain(pid::character, 0.0f);

    // Non-default state-tree values across every schema area.
    auto tree = st::createDefault();
    tree.setProperty(st::id::mode, "SAMPLE", nullptr);
    tree.setProperty(st::id::embedAudio, false, nullptr);
    tree.setProperty(st::id::sourceBPM, 165.25, nullptr);
    tree.setProperty(st::id::zoneStart, 0.125, nullptr);
    tree.setProperty(st::id::zoneEnd, 0.875, nullptr);

    auto sourceFile = tree.getChildWithName(st::id::sourceFile);
    sourceFile.setProperty(st::id::path, "/samples/amen.wav", nullptr);
    sourceFile.setProperty(st::id::contentHash, "deadbeefcafe1234", nullptr);

    auto uiState = tree.getChildWithName(st::id::uiState);
    uiState.setProperty(st::id::scaleFactor, 1.5, nullptr);
    uiState.setProperty(st::id::lcdPage, 3, nullptr);

    st::setClampMemory(tree, mws::model::ModelId::S950, 1500.0);
    st::setRenderMetadata(tree, 0xABCDEF0123456789ULL, "tf=300;cl=256");

    juce::MemoryBlock blob;
    st::writePluginState(src.apvts.copyState(), tree, blob);
    REQUIRE(blob.getSize() > 0);

    // Restore into a fresh APVTS instance.
    Fixture dst;
    const auto restored = st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);
    REQUIRE(restored.apvtsState.isValid());
    dst.apvts.replaceState(restored.apvtsState);

    CHECK(dst.plain(pid::timeFactor) == Approx(300.0f));
    CHECK(dst.plain(pid::cycleLen) == Approx(256.0f));
    CHECK(dst.plain(pid::transpose) == Approx(-7.5f));
    CHECK(dst.plain(pid::character) == Approx(0.0f));

    const auto& out = restored.stateTree;
    REQUIRE(out.isValid());
    CHECK(static_cast<int>(out.getProperty(st::id::stateVersion)) == 1);
    CHECK(out.getProperty(st::id::mode).toString() == "SAMPLE");
    CHECK(static_cast<bool>(out.getProperty(st::id::embedAudio)) == false);
    CHECK(static_cast<double>(out.getProperty(st::id::sourceBPM)) == Approx(165.25));
    CHECK(static_cast<double>(out.getProperty(st::id::zoneStart)) == Approx(0.125));
    CHECK(static_cast<double>(out.getProperty(st::id::zoneEnd)) == Approx(0.875));

    const auto outFile = out.getChildWithName(st::id::sourceFile);
    CHECK(outFile.getProperty(st::id::path).toString() == "/samples/amen.wav");
    CHECK(outFile.getProperty(st::id::contentHash).toString() == "deadbeefcafe1234");

    const auto outUi = out.getChildWithName(st::id::uiState);
    CHECK(static_cast<double>(outUi.getProperty(st::id::scaleFactor)) == Approx(1.5));
    CHECK(static_cast<int>(outUi.getProperty(st::id::lcdPage)) == 3);

    CHECK(st::getClampMemory(out, mws::model::ModelId::S950, 0.0) == Approx(1500.0));
    CHECK(st::getClampMemory(out, mws::model::ModelId::S1000, 100.0) == Approx(100.0)); // unset → fallback

    // Render metadata survives the round trip → re-render-on-load can fire.
    REQUIRE(st::hasRenderMetadata(out));
    const auto meta = out.getChildWithName(st::id::renderMeta);
    CHECK(meta.getProperty(st::id::engineVersionHash).toString().equalsIgnoreCase("ABCDEF0123456789"));
    CHECK(meta.getProperty(st::id::paramsUsed).toString() == "tf=300;cl=256");
}

TEST_CASE("state: stateVersion is written into the serialized blob and read back", "[state]")
{
    Fixture f;
    auto tree = st::createDefault();

    juce::MemoryBlock blob;
    st::writePluginState(f.apvts.copyState(), tree, blob);

    // Inspect the raw serialized root, not the (migrated) read path.
    auto root = juce::ValueTree::readFromData(blob.getData(), blob.getSize());
    REQUIRE(root.isValid());
    REQUIRE(root.hasType(st::id::pluginState));
    const auto serialized = root.getChildWithName(st::id::stateTree);
    REQUIRE(serialized.isValid());
    REQUIRE(serialized.hasProperty(st::id::stateVersion));
    CHECK(static_cast<int>(serialized.getProperty(st::id::stateVersion)) == st::kStateVersion);

    const auto restored = st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);
    CHECK(static_cast<int>(restored.stateTree.getProperty(st::id::stateVersion)) == st::kStateVersion);
}

TEST_CASE("state: migration dispatch upgrades a fake v0 tree to v1", "[state]")
{
    // A pre-versioned tree: no stateVersion property, one known field set.
    juce::ValueTree v0(st::id::stateTree);
    v0.setProperty(st::id::mode, "SAMPLE", nullptr);

    SECTION("direct migrate() call")
    {
        auto migrated = st::migrate(v0, 0);
        CHECK(static_cast<int>(migrated.getProperty(st::id::stateVersion)) == 1);
        CHECK(migrated.getProperty(st::id::mode).toString() == "SAMPLE"); // known field preserved
        CHECK_FALSE(static_cast<bool>(migrated.getProperty(st::id::unknownVersionFallback, false)));
    }

    SECTION("through the full read path")
    {
        Fixture f;
        juce::ValueTree root(st::id::pluginState);
        root.appendChild(f.apvts.copyState(), nullptr);
        root.appendChild(v0.createCopy(), nullptr);

        juce::MemoryBlock blob;
        juce::MemoryOutputStream stream(blob, false);
        root.writeToStream(stream);

        const auto restored = st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
        REQUIRE(restored.valid);
        CHECK(static_cast<int>(restored.stateTree.getProperty(st::id::stateVersion)) == 1);
        CHECK(restored.stateTree.getProperty(st::id::mode).toString() == "SAMPLE");
        // Missing fields were defaults-filled after migration.
        CHECK(static_cast<bool>(restored.stateTree.getProperty(st::id::embedAudio)) == true);
    }
}

TEST_CASE("state: migration of current version is the identity", "[state]")
{
    auto tree = st::createDefault();
    tree.setProperty(st::id::sourceBPM, 174.0, nullptr);

    auto migrated = st::migrate(tree, st::kStateVersion);
    CHECK(migrated.isEquivalentTo(tree));
}

TEST_CASE("state: unknown future version falls back to safe defaults and is flagged", "[state]")
{
    juce::ValueTree future(st::id::stateTree);
    future.setProperty(st::id::stateVersion, st::kStateVersion + 41, nullptr);
    future.setProperty(st::id::mode, "HOLOGRAM", nullptr); // field this build cannot interpret

    auto migrated = st::migrate(future, st::kStateVersion + 41);
    CHECK(static_cast<int>(migrated.getProperty(st::id::stateVersion)) == st::kStateVersion);
    CHECK(migrated.getProperty(st::id::mode).toString() == "FX"); // safe default, not "HOLOGRAM"
    CHECK(static_cast<bool>(migrated.getProperty(st::id::unknownVersionFallback)) == true);
}

TEST_CASE("state: defaults are filled when fields are missing", "[state]")
{
    Fixture f;

    // A v1 tree with almost everything missing (e.g. written by a minimal
    // future tool, or a hand-edited session).
    juce::ValueTree sparse(st::id::stateTree);
    sparse.setProperty(st::id::stateVersion, 1, nullptr);
    sparse.setProperty(st::id::sourceBPM, 93.0, nullptr); // the one present field

    juce::ValueTree root(st::id::pluginState);
    root.appendChild(f.apvts.copyState(), nullptr);
    root.appendChild(sparse, nullptr);

    juce::MemoryBlock blob;
    juce::MemoryOutputStream stream(blob, false);
    root.writeToStream(stream);

    const auto restored = st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);
    const auto& out = restored.stateTree;

    CHECK(static_cast<double>(out.getProperty(st::id::sourceBPM)) == Approx(93.0)); // preserved
    CHECK(out.getProperty(st::id::mode).toString() == "FX");
    CHECK(static_cast<bool>(out.getProperty(st::id::embedAudio)) == true);
    CHECK(static_cast<double>(out.getProperty(st::id::zoneStart)) == Approx(0.0));
    CHECK(static_cast<double>(out.getProperty(st::id::zoneEnd)) == Approx(1.0));
    REQUIRE(out.getChildWithName(st::id::sourceFile).isValid());
    REQUIRE(out.getChildWithName(st::id::uiState).isValid());
    CHECK(static_cast<double>(out.getChildWithName(st::id::uiState)
                                  .getProperty(st::id::scaleFactor)) == Approx(1.0));
    CHECK(static_cast<int>(out.getChildWithName(st::id::uiState)
                               .getProperty(st::id::lcdPage)) == 0);
    CHECK_FALSE(st::hasRenderMetadata(out)); // absence preserved — no spurious re-render
}

TEST_CASE("state: garbage input is rejected without throwing", "[state]")
{
    const char garbage[] = "this is not a value tree";
    const auto restored = st::readPluginState(garbage, static_cast<int>(sizeof(garbage)));
    CHECK_FALSE(restored.valid);

    CHECK_FALSE(st::readPluginState(nullptr, 0).valid);
    CHECK_FALSE(st::readPluginState(garbage, 0).valid);
}

TEST_CASE("state: embedded-audio extension point round-trips a blob untouched (task 032 hook)", "[state]")
{
    Fixture f;
    auto tree = st::createDefault();

    juce::MemoryBlock audio;
    audio.append("FLAC-bytes-stand-in", 19);

    juce::MemoryBlock blob;
    st::writePluginState(f.apvts.copyState(), tree, blob, &audio);

    const auto restored = st::readPluginState(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(restored.valid);
    CHECK(restored.embeddedAudioBlob == audio);

    // And no blob in → no blob out.
    juce::MemoryBlock noAudio;
    st::writePluginState(f.apvts.copyState(), tree, noAudio);
    CHECK(st::readPluginState(noAudio.getData(), static_cast<int>(noAudio.getSize()))
              .embeddedAudioBlob.getSize() == 0);
}
