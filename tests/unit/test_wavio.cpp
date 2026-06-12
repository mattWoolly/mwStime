// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Buffer.h"
#include "mws/core/WavIo.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Test-case names begin with the tag word so `ctest -R wavio` matches
// (plan/backlog/README.md test-selection rules).

namespace
{
using mws::core::AudioBuffer;
using mws::core::WavIo;

namespace fs = std::filesystem;

/// Unique temp file path, removed on destruction.
struct TempFile
{
    explicit TempFile(const std::string& name)
        : path(fs::temp_directory_path()
               / ("mwstime_wavio_test_" + std::to_string(counter++) + "_" + name))
    {
    }

    ~TempFile()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }

    fs::path path;
    static inline int counter = 0;
};

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::vector<std::uint8_t> readBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void appendU16(std::vector<std::uint8_t>& v, std::uint16_t x)
{
    v.push_back(static_cast<std::uint8_t>(x & 0xff));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xff));
}

void appendU32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    appendU16(v, static_cast<std::uint16_t>(x & 0xffff));
    appendU16(v, static_cast<std::uint16_t>((x >> 16) & 0xffff));
}

void appendTag(std::vector<std::uint8_t>& v, const char (&tag)[5])
{
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<std::uint8_t>(tag[i]));
}

/// Hand-built 16-bit PCM mono WAV with the given fmt tag / bit depth and
/// arbitrary chunks before and after the fmt chunk (RIFF-tolerance fixture).
std::vector<std::uint8_t> craftWav(std::uint16_t formatTag, std::uint16_t bits,
                                   std::uint16_t channels,
                                   const std::vector<std::int16_t>& samples,
                                   bool withExtraChunks)
{
    std::vector<std::uint8_t> body;

    if (withExtraChunks)
    {
        // A JUNK chunk with an ODD payload size (pad byte must be skipped).
        appendTag(body, "JUNK");
        appendU32(body, 3);
        body.push_back('x');
        body.push_back('y');
        body.push_back('z');
        body.push_back(0); // pad byte to even boundary
    }

    appendTag(body, "fmt ");
    appendU32(body, 16);
    appendU16(body, formatTag);
    appendU16(body, channels);
    appendU32(body, 44100);
    appendU32(body, 44100u * channels * (bits / 8u));
    appendU16(body, static_cast<std::uint16_t>(channels * (bits / 8u)));
    appendU16(body, bits);

    if (withExtraChunks)
    {
        appendTag(body, "LIST");
        appendU32(body, 4);
        appendTag(body, "INFO");
    }

    appendTag(body, "data");
    appendU32(body, static_cast<std::uint32_t>(samples.size() * 2));
    for (const std::int16_t s : samples)
        appendU16(body, static_cast<std::uint16_t>(s));

    std::vector<std::uint8_t> file;
    appendTag(file, "RIFF");
    appendU32(file, static_cast<std::uint32_t>(body.size() + 4));
    appendTag(file, "WAVE");
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

/// Locates the data chunk payload inside a (canonical, even-sized-chunks) WAV.
std::vector<std::uint8_t> extractDataPayload(const std::vector<std::uint8_t>& file)
{
    REQUIRE(file.size() >= 12);
    std::size_t off = 12;
    while (off + 8 <= file.size())
    {
        const std::uint32_t size = static_cast<std::uint32_t>(file[off + 4])
                                   | (static_cast<std::uint32_t>(file[off + 5]) << 8)
                                   | (static_cast<std::uint32_t>(file[off + 6]) << 16)
                                   | (static_cast<std::uint32_t>(file[off + 7]) << 24);
        if (std::memcmp(file.data() + off, "data", 4) == 0)
        {
            REQUIRE(off + 8 + size <= file.size());
            return { file.begin() + static_cast<std::ptrdiff_t>(off + 8),
                     file.begin() + static_cast<std::ptrdiff_t>(off + 8 + size) };
        }
        off += 8 + size + (size & 1u);
    }
    FAIL("no data chunk found");
    return {};
}

/// Test buffer whose samples are all exactly representable at the given
/// integer scale (2^(bits-1)), so an int round trip must be float-exact.
AudioBuffer makeExactBuffer(std::size_t channels, std::size_t frames, double scale)
{
    AudioBuffer buffer(channels, frames);
    buffer.sampleRate = 44100.0;
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
        auto view = buffer.channel(ch);
        for (std::size_t i = 0; i < frames; ++i)
        {
            // Spread integer codes across the full range, channel-offset so
            // channels differ; clamp into [-scale, scale - 1].
            const double span = 2.0 * scale - 1.0;
            double code = -scale + (span * static_cast<double>(i * 7919u + ch * 104729u
                                                               + 1u))
                                       / static_cast<double>(frames * 7919u + 104730u);
            code = static_cast<double>(static_cast<long long>(code));
            view[i] = static_cast<float>(code / scale);
        }
        // Pin the exact extremes and zero.
        if (frames >= 3)
        {
            view[0] = -1.0f;
            view[1] = static_cast<float>((scale - 1.0) / scale);
            view[2] = 0.0f;
        }
    }
    return buffer;
}

void requireBuffersIdentical(const AudioBuffer& a, const AudioBuffer& b)
{
    REQUIRE(a.numChannels() == b.numChannels());
    REQUIRE(a.numFrames() == b.numFrames());
    REQUIRE(a.sampleRate == b.sampleRate);
    std::size_t mismatches = 0;
    for (std::size_t ch = 0; ch < a.numChannels(); ++ch)
    {
        auto va = a.channel(ch);
        auto vb = b.channel(ch);
        for (std::size_t i = 0; i < a.numFrames(); ++i)
            if (va[i] != vb[i])
                ++mismatches;
    }
    REQUIRE(mismatches == 0);
}

struct DepthCase
{
    WavIo::BitDepth depth;
    double scale;       // 2^(bits-1); for Float32 unused
    const char* name;
};

} // namespace

TEST_CASE("wavio: round-trip is exact for every bit depth, mono and stereo", "[wavio]")
{
    // For int formats the source values are exact integer codes / 2^(bits-1),
    // so the round trip must be float-bit-exact. For Int32 the codes are
    // multiples of 2^7 (k / 2^24) so they survive the float32 mantissa.
    const DepthCase cases[] = {
        { WavIo::BitDepth::Int16, 32768.0, "Int16" },
        { WavIo::BitDepth::Int24, 8388608.0, "Int24" },
        { WavIo::BitDepth::Int32, 16777216.0, "Int32 (24-bit-representable codes)" },
    };

    for (const auto& c : cases)
    {
        for (std::size_t channels : { std::size_t{ 1 }, std::size_t{ 2 } })
        {
            INFO(c.name << ", channels=" << channels);
            TempFile file("roundtrip.wav");
            const AudioBuffer source = makeExactBuffer(channels, 257, c.scale);

            const auto wr = WavIo::write(file.path, source, c.depth);
            INFO("write error: " << wr.error);
            REQUIRE(wr.ok());

            auto rr = WavIo::read(file.path);
            INFO("read error: " << rr.error);
            REQUIRE(rr.ok());
            requireBuffersIdentical(source, rr.buffer);
        }
    }
}

TEST_CASE("wavio: float32 round-trip preserves arbitrary values exactly", "[wavio]")
{
    for (std::size_t channels : { std::size_t{ 1 }, std::size_t{ 2 } })
    {
        INFO("channels=" << channels);
        AudioBuffer source(channels, 64);
        source.sampleRate = 48000.0;
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            auto view = source.channel(ch);
            for (std::size_t i = 0; i < 64; ++i)
                view[i] = (static_cast<float>(i) - 31.5f) * 0.0413f
                          + static_cast<float>(ch) * 1.0e-7f;
            view[0] = -1.5f;        // out-of-[-1,1] values are legal in float WAVs
            view[1] = 1.25f;
            view[2] = 1.1754944e-38f; // smallest normal float
        }

        TempFile file("float.wav");
        const auto wr = WavIo::write(file.path, source, WavIo::BitDepth::Float32);
        INFO("write error: " << wr.error);
        REQUIRE(wr.ok());

        auto rr = WavIo::read(file.path);
        INFO("read error: " << rr.error);
        REQUIRE(rr.ok());
        requireBuffersIdentical(source, rr.buffer);
    }
}

TEST_CASE("wavio: all 65536 16-bit codes survive a write/read cycle bit-exactly",
          "[wavio]")
{
    AudioBuffer source(1, 65536);
    source.sampleRate = 44100.0;
    auto view = source.channel(0);
    for (std::size_t i = 0; i < 65536; ++i)
        view[i] = static_cast<float>(static_cast<int>(i) - 32768) / 32768.0f;

    TempFile file("allcodes.wav");
    REQUIRE(WavIo::write(file.path, source, WavIo::BitDepth::Int16).ok());

    auto rr = WavIo::read(file.path);
    REQUIRE(rr.ok());
    requireBuffersIdentical(source, rr.buffer);

    // And the stored int16 codes are exactly the original codes.
    const auto payload = extractDataPayload(readBytes(file.path));
    REQUIRE(payload.size() == 65536 * 2);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < 65536; ++i)
    {
        const auto stored = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(payload[i * 2])
            | (static_cast<std::uint16_t>(payload[i * 2 + 1]) << 8));
        if (stored != static_cast<int>(i) - 32768)
            ++mismatches;
    }
    REQUIRE(mismatches == 0);
}

TEST_CASE("wavio: float-to-int conversion rounds to nearest, ties away from zero, "
          "and clamps",
          "[wavio]")
{
    // Conversion contract (WavIo.h): code = clamp(llround(sample * 2^(bits-1))).
    AudioBuffer source(1, 8);
    source.sampleRate = 44100.0;
    auto view = source.channel(0);
    view[0] = 0.25f;             // 8192 exactly
    view[1] = 0.4f / 32768.0f;   // rounds to 0
    view[2] = 0.6f / 32768.0f;   // rounds to 1
    view[3] = 1.5f / 32768.0f;   // tie -> away from zero -> 2
    view[4] = -1.5f / 32768.0f;  // tie -> away from zero -> -2
    view[5] = 1.0f;              // +1.0 would be 32768 -> clamps to 32767
    view[6] = -1.0f;             // exactly -32768
    view[7] = -2.0f;             // clamps to -32768
    const std::int16_t expected[8] = { 8192, 0, 1, 2, -2, 32767, -32768, -32768 };

    TempFile file("rounding.wav");
    REQUIRE(WavIo::write(file.path, source, WavIo::BitDepth::Int16).ok());

    const auto payload = extractDataPayload(readBytes(file.path));
    REQUIRE(payload.size() == 16);
    for (std::size_t i = 0; i < 8; ++i)
    {
        INFO("sample index " << i);
        const auto stored = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(payload[i * 2])
            | (static_cast<std::uint16_t>(payload[i * 2 + 1]) << 8));
        REQUIRE(stored == expected[i]);
    }
}

TEST_CASE("wavio: stereo data is interleaved per frame and de-interleaved on read",
          "[wavio]")
{
    AudioBuffer source(2, 3);
    source.sampleRate = 44100.0;
    source.channel(0)[0] = 1.0f / 32768.0f;
    source.channel(0)[1] = 3.0f / 32768.0f;
    source.channel(0)[2] = 5.0f / 32768.0f;
    source.channel(1)[0] = 2.0f / 32768.0f;
    source.channel(1)[1] = 4.0f / 32768.0f;
    source.channel(1)[2] = 6.0f / 32768.0f;

    TempFile file("interleave.wav");
    REQUIRE(WavIo::write(file.path, source, WavIo::BitDepth::Int16).ok());

    const auto payload = extractDataPayload(readBytes(file.path));
    REQUIRE(payload.size() == 12);
    for (std::size_t i = 0; i < 6; ++i) // L0 R0 L1 R1 L2 R2 == 1..6
    {
        const auto stored = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(payload[i * 2])
            | (static_cast<std::uint16_t>(payload[i * 2 + 1]) << 8));
        REQUIRE(stored == static_cast<std::int16_t>(i + 1));
    }

    auto rr = WavIo::read(file.path);
    REQUIRE(rr.ok());
    requireBuffersIdentical(source, rr.buffer);
}

TEST_CASE("wavio: sample rate is preserved through a round trip", "[wavio]")
{
    AudioBuffer source(1, 16);
    source.sampleRate = 22050.0;
    TempFile file("rate.wav");
    REQUIRE(WavIo::write(file.path, source, WavIo::BitDepth::Int16).ok());

    auto rr = WavIo::read(file.path);
    REQUIRE(rr.ok());
    REQUIRE(rr.buffer.sampleRate == 22050.0);
}

TEST_CASE("wavio: parser tolerates extra RIFF chunks (odd-sized, before and after fmt)",
          "[wavio]")
{
    const std::vector<std::int16_t> samples = { 100, -200, 300, -32768, 32767 };
    TempFile file("extrachunks.wav");
    writeBytes(file.path, craftWav(1 /* PCM */, 16, 1, samples, true));

    auto rr = WavIo::read(file.path);
    INFO("read error: " << rr.error);
    REQUIRE(rr.ok());
    REQUIRE(rr.buffer.numChannels() == 1);
    REQUIRE(rr.buffer.numFrames() == samples.size());
    REQUIRE(rr.buffer.sampleRate == 44100.0);
    auto view = rr.buffer.channel(0);
    for (std::size_t i = 0; i < samples.size(); ++i)
        REQUIRE(view[i] == static_cast<float>(samples[i]) / 32768.0f);
}

TEST_CASE("wavio: rejects a file with the wrong magic", "[wavio]")
{
    TempFile file("notawav.bin");
    writeBytes(file.path, { 'N', 'O', 'P', 'E', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 });

    const auto rr = WavIo::read(file.path);
    REQUIRE_FALSE(rr.ok());
    REQUIRE_FALSE(rr.error.empty());
}

TEST_CASE("wavio: rejects a truncated file", "[wavio]")
{
    TempFile file("truncated.wav");
    const AudioBuffer source = makeExactBuffer(1, 256, 32768.0);
    REQUIRE(WavIo::write(file.path, source, WavIo::BitDepth::Int16).ok());

    auto bytes = readBytes(file.path);
    bytes.resize(bytes.size() / 2); // cut the data chunk short
    writeBytes(file.path, bytes);

    const auto rr = WavIo::read(file.path);
    REQUIRE_FALSE(rr.ok());
    REQUIRE_FALSE(rr.error.empty());
}

TEST_CASE("wavio: rejects unsupported codecs and bit depths with a clear error",
          "[wavio]")
{
    SECTION("ADPCM format tag")
    {
        TempFile file("adpcm.wav");
        writeBytes(file.path, craftWav(2 /* MS ADPCM */, 16, 1, { 0, 0 }, false));
        const auto rr = WavIo::read(file.path);
        REQUIRE_FALSE(rr.ok());
        REQUIRE(rr.error.find("format") != std::string::npos);
    }

    SECTION("8-bit PCM")
    {
        TempFile file("8bit.wav");
        writeBytes(file.path, craftWav(1, 8, 1, { 0, 0 }, false));
        const auto rr = WavIo::read(file.path);
        REQUIRE_FALSE(rr.ok());
        REQUIRE_FALSE(rr.error.empty());
    }

    SECTION("more than two channels")
    {
        TempFile file("quad.wav");
        writeBytes(file.path, craftWav(1, 16, 4, { 0, 0, 0, 0 }, false));
        const auto rr = WavIo::read(file.path);
        REQUIRE_FALSE(rr.ok());
        REQUIRE_FALSE(rr.error.empty());
    }
}

TEST_CASE("wavio: read of a nonexistent file fails cleanly", "[wavio]")
{
    const auto rr = WavIo::read(fs::temp_directory_path()
                                / "mwstime_wavio_does_not_exist_0xdead.wav");
    REQUIRE_FALSE(rr.ok());
    REQUIRE_FALSE(rr.error.empty());
}

TEST_CASE("wavio: write rejects invalid buffers and unwritable paths", "[wavio]")
{
    SECTION("unset sample rate")
    {
        AudioBuffer buffer(1, 4); // sampleRate left at 0.0
        TempFile file("badrate.wav");
        const auto wr = WavIo::write(file.path, buffer, WavIo::BitDepth::Int16);
        REQUIRE_FALSE(wr.ok());
    }

    SECTION("zero channels")
    {
        AudioBuffer buffer;
        buffer.sampleRate = 44100.0;
        TempFile file("nochan.wav");
        const auto wr = WavIo::write(file.path, buffer, WavIo::BitDepth::Int16);
        REQUIRE_FALSE(wr.ok());
    }

    SECTION("unwritable path")
    {
        AudioBuffer buffer(1, 4);
        buffer.sampleRate = 44100.0;
        const auto wr = WavIo::write(fs::temp_directory_path()
                                         / "mwstime_wavio_no_such_dir_0xdead"
                                         / "out.wav",
                                     buffer, WavIo::BitDepth::Int16);
        REQUIRE_FALSE(wr.ok());
        REQUIRE_FALSE(wr.error.empty());
    }
}

TEST_CASE("wavio: reads WAVE_FORMAT_EXTENSIBLE PCM and float files", "[wavio]")
{
    // 24-bit files in the wild are commonly WAVE_FORMAT_EXTENSIBLE (0xFFFE)
    // with the PCM sub-format GUID; the reader must accept them.
    std::vector<std::uint8_t> body;
    appendTag(body, "fmt ");
    appendU32(body, 40);
    appendU16(body, 0xFFFE);            // WAVE_FORMAT_EXTENSIBLE
    appendU16(body, 1);                 // mono
    appendU32(body, 44100);
    appendU32(body, 44100u * 3u);
    appendU16(body, 3);                 // block align
    appendU16(body, 24);                // bits per sample
    appendU16(body, 22);                // cbSize
    appendU16(body, 24);                // valid bits
    appendU32(body, 0);                 // channel mask
    // Sub-format GUID: PCM = 00000001-0000-0010-8000-00AA00389B71.
    appendU32(body, 0x00000001);
    appendU16(body, 0x0000);
    appendU16(body, 0x0010);
    const std::uint8_t guidTail[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
    body.insert(body.end(), guidTail, guidTail + 8);

    appendTag(body, "data");
    appendU32(body, 6);
    // Two 24-bit samples, little-endian: +1 and -8388608 (min).
    body.push_back(0x01); body.push_back(0x00); body.push_back(0x00);
    body.push_back(0x00); body.push_back(0x00); body.push_back(0x80);

    std::vector<std::uint8_t> file;
    appendTag(file, "RIFF");
    appendU32(file, static_cast<std::uint32_t>(body.size() + 4));
    appendTag(file, "WAVE");
    file.insert(file.end(), body.begin(), body.end());

    TempFile temp("extensible.wav");
    writeBytes(temp.path, file);

    auto rr = WavIo::read(temp.path);
    INFO("read error: " << rr.error);
    REQUIRE(rr.ok());
    REQUIRE(rr.buffer.numFrames() == 2);
    REQUIRE(rr.buffer.channel(0)[0] == 1.0f / 8388608.0f);
    REQUIRE(rr.buffer.channel(0)[1] == -1.0f);
}
