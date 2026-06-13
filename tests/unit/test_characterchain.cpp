// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/engine/Params.h"
#include "mws/model/CharacterChain.h"
#include "mws/model/ModelId.h"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

// Test-case names begin with the tag word so `ctest -R characterchain`
// matches (plan/backlog/README.md test-selection rules).
//
// Spec: docs/design/dsp-engine.md §8 (per-model chain dispatch), §8.4
// (CHARACTER bypass: engine runs at host rate on unquantized audio), §5
// (S900/S950 stereo->mono sum rule), §7.4 (modelRateFor feeds the FX latency
// formula); docs/design/architecture.md §5.1 (stages [1] and [4]), §9
// (global CHARACTER bypass; mono engines). Task:
// plan/backlog/019-character-chain-api.md.

namespace
{
using mws::core::AudioBuffer;
using mws::core::ConstAudioView;
using mws::engine::ParamSnapshot;
using mws::engine::SampleRateSel;
using mws::model::CharacterChain;
using mws::model::ModelId;

constexpr double kPi = std::numbers::pi_v<double>;

/// Deterministic broadband-ish signal (sum of incommensurate sines), scaled
/// to stay inside +-1.0 full scale, phase-offset per channel so stereo
/// channels differ (the mono-sum tests need L != R).
AudioBuffer makeTestBuffer(std::size_t numChannels, std::size_t numFrames,
                           double sampleRate)
{
    AudioBuffer buffer(numChannels, numFrames);
    buffer.sampleRate = sampleRate;
    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        const double phase = 0.5 * static_cast<double>(ch);
        auto view = buffer.channel(ch);
        for (std::size_t n = 0; n < numFrames; ++n)
        {
            const auto t = static_cast<double>(n);
            view[n] = static_cast<float>(
                0.5 * std::sin(2.0 * kPi * 0.011 * t + phase)
                + 0.3 * std::sin(2.0 * kPi * 0.041 * t + 0.7 + phase)
                + 0.2 * std::sin(2.0 * kPi * 0.127 * t + 1.3 + phase));
        }
    }
    return buffer;
}

/// True when every sample of every channel sits exactly on the mid-tread
/// lattice with `codesPerUnit` codes per unit amplitude (2048 at 12-bit,
/// 32768 at 16-bit). Exact in float32 (steps are powers of two).
bool onLattice(const AudioBuffer& buffer, double codesPerUnit)
{
    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
    {
        const ConstAudioView view = buffer.channel(ch);
        for (std::size_t n = 0; n < view.size(); ++n)
        {
            const double scaled = static_cast<double>(view[n]) * codesPerUnit;
            if (scaled != std::nearbyint(scaled))
                return false;
        }
    }
    return true;
}

/// Bit-identical comparison, channel layout included.
bool bitIdentical(const AudioBuffer& a, const AudioBuffer& b)
{
    if (a.numChannels() != b.numChannels() || a.numFrames() != b.numFrames())
        return false;
    for (std::size_t ch = 0; ch < a.numChannels(); ++ch)
    {
        const ConstAudioView va = a.channel(ch);
        const ConstAudioView vb = b.channel(ch);
        for (std::size_t n = 0; n < va.size(); ++n)
            if (va[n] != vb[n])
                return false;
    }
    return true;
}

ParamSnapshot makeParams(ModelId model)
{
    ParamSnapshot params;
    params.model = model;
    return params;
}
} // namespace

TEST_CASE("characterchain: dispatch — S950 ingest lands on the 12-bit "
          "lattice, S1000 on the 16-bit lattice",
          "[characterchain]")
{
    // Per-model dispatch (dsp-engine.md §8.1 vs §8.2): the facade must route
    // S950 through the VarClock 12-bit chain and S1000 through the
    // FixedRate 16-bit chain. The quantizer lattice is the fingerprint.
    constexpr std::size_t numFrames = 8192;
    const AudioBuffer input = makeTestBuffer(1, numFrames, 44100.0);

    SECTION("S950 -> 12-bit lattice at 2.5 x bandwidth")
    {
        ParamSnapshot params = makeParams(ModelId::S950);
        params.bandwidth = 19.2;

        const auto result = CharacterChain::ingest(input, ModelId::S950, params);
        REQUIRE(result.audio.numChannels() == 1);
        REQUIRE(result.audio.numFrames() > 0);
        REQUIRE(result.audio.sampleRate == 48000.0);
        REQUIRE(onLattice(result.audio, 2048.0));
    }

    SECTION("S1000 -> 16-bit lattice at sampleRateSel")
    {
        ParamSnapshot params = makeParams(ModelId::S1000);
        params.sampleRateSel = SampleRateSel::Fs44100;

        const auto result = CharacterChain::ingest(input, ModelId::S1000, params);
        REQUIRE(result.audio.numChannels() == 1);
        REQUIRE(result.audio.numFrames() > 0);
        REQUIRE(result.audio.sampleRate == 44100.0);
        REQUIRE(onLattice(result.audio, 32768.0));

        // The 16-bit lattice signal must NOT be entirely on the (coarser)
        // 12-bit lattice — i.e. S1000 really took the 16-bit path.
        REQUIRE(!onLattice(result.audio, 2048.0));
    }
}

TEST_CASE("characterchain: bypass — character OFF is bit-identical in/out "
          "for all four shipping models",
          "[characterchain]")
{
    // dsp-engine.md §8.4: character = OFF skips every §8 stage — no
    // resample, no quantize, no mono sum. Identity for both calls.
    constexpr std::size_t numFrames = 4096;
    constexpr double hostRate = 48000.0;
    const AudioBuffer input = makeTestBuffer(2, numFrames, hostRate);

    for (const ModelId model :
         { ModelId::S900, ModelId::S950, ModelId::S1000, ModelId::S1100 })
    {
        INFO("model = " << mws::model::toString(model));
        ParamSnapshot params = makeParams(model);
        params.character = false;

        const auto ingested = CharacterChain::ingest(input, model, params);
        REQUIRE(bitIdentical(ingested.audio, input));
        REQUIRE(ingested.audio.sampleRate == input.sampleRate);
        REQUIRE(!ingested.monoSummed); // no mono sum when bypassed

        const AudioBuffer played = CharacterChain::playback(
            input, model, params, /*clockRatio=*/1.0, hostRate);
        REQUIRE(bitIdentical(played, input));
        REQUIRE(played.sampleRate == input.sampleRate);
    }
}

TEST_CASE("characterchain: mono sum — stereo into S950 yields 1 channel "
          "flagged monoSummed; S1000 keeps 2 channels",
          "[characterchain]")
{
    // dsp-engine.md §5: the S950 is a mono machine; stereo input is summed
    // to mono FIRST (authentic; the LCD states it via the monoSummed flag).
    constexpr std::size_t numFrames = 4096;
    const AudioBuffer stereo = makeTestBuffer(2, numFrames, 48000.0);

    SECTION("S950 sums to mono and reports it")
    {
        ParamSnapshot params = makeParams(ModelId::S950);
        const auto result = CharacterChain::ingest(stereo, ModelId::S950, params);
        REQUIRE(result.audio.numChannels() == 1);
        REQUIRE(result.monoSummed);
    }

    SECTION("S900 sums to mono and reports it")
    {
        ParamSnapshot params = makeParams(ModelId::S900);
        const auto result = CharacterChain::ingest(stereo, ModelId::S900, params);
        REQUIRE(result.audio.numChannels() == 1);
        REQUIRE(result.monoSummed);
    }

    SECTION("mono into S950 is not flagged (nothing was summed)")
    {
        const AudioBuffer mono = makeTestBuffer(1, numFrames, 48000.0);
        ParamSnapshot params = makeParams(ModelId::S950);
        const auto result = CharacterChain::ingest(mono, ModelId::S950, params);
        REQUIRE(result.audio.numChannels() == 1);
        REQUIRE(!result.monoSummed);
    }

    SECTION("S1000 keeps both channels, never flags")
    {
        ParamSnapshot params = makeParams(ModelId::S1000);
        const auto result =
            CharacterChain::ingest(stereo, ModelId::S1000, params);
        REQUIRE(result.audio.numChannels() == 2);
        REQUIRE(!result.monoSummed);

        // Channels stay independent (no accidental sum): the test signal is
        // phase-offset per channel, so L != R must survive ingest.
        const ConstAudioView left = result.audio.channel(0);
        const ConstAudioView right = result.audio.channel(1);
        bool anyDifferent = false;
        for (std::size_t n = 0; n < left.size() && !anyDifferent; ++n)
            anyDifferent = (left[n] != right[n]);
        REQUIRE(anyDifferent);
    }
}

TEST_CASE("characterchain: modelRateFor is the single rate authority",
          "[characterchain]")
{
    constexpr double hostRate = 96000.0;

    SECTION("S950 BW 19.2 => 48000")
    {
        ParamSnapshot params = makeParams(ModelId::S950);
        params.bandwidth = 19.2;
        REQUIRE(CharacterChain::modelRateFor(ModelId::S950, params, hostRate)
                == 48000.0);
    }

    SECTION("S900 BW 16.0 => 40000")
    {
        ParamSnapshot params = makeParams(ModelId::S900);
        params.bandwidth = 16.0;
        REQUIRE(CharacterChain::modelRateFor(ModelId::S900, params, hostRate)
                == 40000.0);
    }

    SECTION("S900 clamps bandwidth above its 16 kHz ceiling (rate <= 40 kHz)")
    {
        ParamSnapshot params = makeParams(ModelId::S900);
        params.bandwidth = 19.2; // legal superset value, above the S900 max
        REQUIRE(CharacterChain::modelRateFor(ModelId::S900, params, hostRate)
                == 40000.0);
    }

    SECTION("S1000 FS selector honored")
    {
        ParamSnapshot params = makeParams(ModelId::S1000);
        params.sampleRateSel = SampleRateSel::Fs44100;
        REQUIRE(CharacterChain::modelRateFor(ModelId::S1000, params, hostRate)
                == 44100.0);
        params.sampleRateSel = SampleRateSel::Fs22050;
        REQUIRE(CharacterChain::modelRateFor(ModelId::S1000, params, hostRate)
                == 22050.0);
    }

    SECTION("character OFF => engine model rate is the host rate (§8.4)")
    {
        for (const ModelId model :
             { ModelId::S900, ModelId::S950, ModelId::S1000, ModelId::S1100 })
        {
            INFO("model = " << mws::model::toString(model));
            ParamSnapshot params = makeParams(model);
            params.character = false;
            REQUIRE(CharacterChain::modelRateFor(model, params, hostRate)
                    == hostRate);
        }
    }
}

TEST_CASE("characterchain: S1100 playback differs from S1000 by the dither "
          "delta only",
          "[characterchain]")
{
    // §8.2 S1100 delta: same engine + chain; output stage adds the seeded
    // TPDF 16-bit quantize. The facade must dispatch the delta per model.
    constexpr double hostRate = 48000.0;
    constexpr std::size_t numFrames = 4096;
    AudioBuffer stretched = makeTestBuffer(1, numFrames, 44100.0);

    const ParamSnapshot p1000 = makeParams(ModelId::S1000);
    const ParamSnapshot p1100 = makeParams(ModelId::S1100);

    const AudioBuffer s1000 =
        CharacterChain::playback(stretched, ModelId::S1000, p1000, 1.0, hostRate);
    const AudioBuffer s1100 =
        CharacterChain::playback(stretched, ModelId::S1100, p1100, 1.0, hostRate);
    REQUIRE(s1000.numFrames() == s1100.numFrames());
    REQUIRE(s1000.sampleRate == hostRate);
    REQUIRE(s1100.sampleRate == hostRate);

    // Dithered S1100 output sits on the 16-bit lattice; the S1000 path does
    // not get an output quantize, so the two differ — but only at the
    // +-2 LSB dither level (no level offset, §2 outTrim row).
    REQUIRE(onLattice(s1100, 32768.0));
    const ConstAudioView a = s1000.channel(0);
    const ConstAudioView b = s1100.channel(0);
    float maxDiff = 0.0f;
    for (std::size_t n = 0; n < a.size(); ++n)
        maxDiff = std::max(maxDiff, std::abs(b[n] - a[n]));
    REQUIRE(maxDiff > 0.0f);
    REQUIRE(maxDiff <= 2.0f / 32768.0f);

    // Deterministic: the facade's internal seed is fixed (same input =>
    // bit-identical output, architecture.md §7).
    const AudioBuffer again =
        CharacterChain::playback(stretched, ModelId::S1100, p1100, 1.0, hostRate);
    REQUIRE(bitIdentical(s1100, again));
}

TEST_CASE("characterchain: S3000 is reserved — fails loudly, never aliases "
          "another model",
          "[characterchain]")
{
    // ADR-004 / task 019 out-of-scope rule: the S3000 case must fail loudly
    // (debug assert; empty/zero results in release), NOT silently behave
    // like the S1000.
    constexpr double hostRate = 48000.0;
    const AudioBuffer input = makeTestBuffer(1, 1024, hostRate);
    const ParamSnapshot params = makeParams(ModelId::S3000);

    REQUIRE(CharacterChain::modelRateFor(ModelId::S3000, params, hostRate)
            == 0.0);

    const auto ingested = CharacterChain::ingest(input, ModelId::S3000, params);
    REQUIRE(ingested.audio.numFrames() == 0);

    const AudioBuffer played =
        CharacterChain::playback(input, ModelId::S3000, params, 1.0, hostRate);
    REQUIRE(played.numFrames() == 0);
}
