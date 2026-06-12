// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/WavIo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace mws::core {

namespace {

// ---------------------------------------------------------------------------
// Little-endian byte helpers
// ---------------------------------------------------------------------------

std::uint16_t readU16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t readU32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16)
           | (static_cast<std::uint32_t>(p[3]) << 24);
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
    v.insert(v.end(), tag, tag + 4);
}

bool tagEquals(const std::uint8_t* p, const char (&tag)[5]) noexcept
{
    return std::memcmp(p, tag, 4) == 0;
}

// ---------------------------------------------------------------------------
// Deterministic int <-> float conversion (contract documented in WavIo.h)
// ---------------------------------------------------------------------------

/// int code -> float32: code / 2^(bits-1), computed in double then narrowed.
float codeToFloat(std::int64_t code, double scale) noexcept
{
    return static_cast<float>(static_cast<double>(code) / scale);
}

/// float32 -> int code: round to nearest, ties away from zero, clamped.
std::int64_t floatToCode(float sample, double scale, std::int64_t lo,
                         std::int64_t hi) noexcept
{
    const double scaled = static_cast<double>(sample) * scale;
    // Clamp in the double domain first so llround never sees out-of-range
    // values (Int32's hi = 2^31 - 1 is fine in double).
    const double clamped = std::clamp(scaled, static_cast<double>(lo),
                                      static_cast<double>(hi));
    return std::llround(clamped);
}

// ---------------------------------------------------------------------------
// WAV constants
// ---------------------------------------------------------------------------

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

struct FmtInfo
{
    std::uint16_t formatTag = 0;   // effective tag (sub-format for extensible)
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

WavIo::ReadResult WavIo::read(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return { {}, "cannot open file for reading: " + path.string() };

    std::vector<std::uint8_t> file{ std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>() };
    if (in.bad())
        return { {}, "read error: " + path.string() };

    if (file.size() < 12 || !tagEquals(file.data(), "RIFF")
        || !tagEquals(file.data() + 8, "WAVE"))
        return { {}, "not a RIFF/WAVE file (wrong magic): " + path.string() };

    // Chunk scan. Tolerant of unknown/extra chunks (odd sizes are padded to
    // even per the RIFF spec) and of a RIFF size field that disagrees with
    // the actual file size — only the chunks we use must lie within the file.
    FmtInfo fmt;
    bool haveFmt = false;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    bool haveData = false;

    std::size_t off = 12;
    while (off + 8 <= file.size())
    {
        const std::uint8_t* hdr = file.data() + off;
        const std::uint32_t chunkSize = readU32(hdr + 4);
        const std::size_t payload = off + 8;

        if (tagEquals(hdr, "fmt "))
        {
            if (payload + chunkSize > file.size())
                return { {}, "truncated fmt chunk" };
            if (chunkSize < 16)
                return { {}, "fmt chunk too small" };

            const std::uint8_t* p = file.data() + payload;
            fmt.formatTag = readU16(p + 0);
            fmt.channels = readU16(p + 2);
            fmt.sampleRate = readU32(p + 4);
            fmt.bitsPerSample = readU16(p + 14);

            if (fmt.formatTag == kFormatExtensible)
            {
                // WAVE_FORMAT_EXTENSIBLE: the effective codec is the first
                // two bytes of the 16-byte sub-format GUID at offset 24.
                if (chunkSize < 40)
                    return { {}, "extensible fmt chunk too small" };
                fmt.formatTag = readU16(p + 24);
            }
            haveFmt = true;
        }
        else if (tagEquals(hdr, "data"))
        {
            if (payload + chunkSize > file.size())
                return { {}, "truncated file: data chunk extends past end of file" };
            dataOffset = payload;
            dataSize = chunkSize;
            haveData = true;
        }
        // Any other chunk (JUNK, LIST, fact, ...) is skipped.

        off = payload + chunkSize + (chunkSize & 1u); // chunks are word-aligned
    }

    if (!haveFmt)
        return { {}, "missing fmt chunk" };
    if (!haveData)
        return { {}, "missing data chunk" };

    if (fmt.formatTag != kFormatPcm && fmt.formatTag != kFormatIeeeFloat)
        return { {}, "unsupported format tag " + std::to_string(fmt.formatTag)
                         + " (only PCM and IEEE float are supported)" };
    if (fmt.channels < 1 || fmt.channels > 2)
        return { {}, "unsupported channel count " + std::to_string(fmt.channels)
                         + " (only mono/stereo are supported)" };
    if (fmt.sampleRate == 0)
        return { {}, "invalid sample rate 0" };

    const bool isFloat = fmt.formatTag == kFormatIeeeFloat;
    if (isFloat && fmt.bitsPerSample != 32)
        return { {}, "unsupported float bit depth " + std::to_string(fmt.bitsPerSample)
                         + " (only 32-bit float is supported)" };
    if (!isFloat && fmt.bitsPerSample != 16 && fmt.bitsPerSample != 24
        && fmt.bitsPerSample != 32)
        return { {}, "unsupported PCM bit depth " + std::to_string(fmt.bitsPerSample)
                         + " (only 16/24/32-bit are supported)" };

    const std::size_t bytesPerSample = fmt.bitsPerSample / 8u;
    const std::size_t frameBytes = bytesPerSample * fmt.channels;
    const std::size_t numFrames = dataSize / frameBytes; // trailing partial frame dropped

    AudioBuffer buffer(fmt.channels, numFrames);
    buffer.sampleRate = static_cast<double>(fmt.sampleRate);

    const std::uint8_t* data = file.data() + dataOffset;
    for (std::size_t ch = 0; ch < fmt.channels; ++ch)
    {
        AudioView view = buffer.channel(ch);
        const std::uint8_t* p = data + ch * bytesPerSample;

        switch (fmt.bitsPerSample)
        {
        case 16:
            for (std::size_t i = 0; i < numFrames; ++i, p += frameBytes)
            {
                const auto code = static_cast<std::int16_t>(readU16(p));
                view[i] = codeToFloat(code, 32768.0);
            }
            break;
        case 24:
            for (std::size_t i = 0; i < numFrames; ++i, p += frameBytes)
            {
                std::int32_t code = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(p[0])
                    | (static_cast<std::uint32_t>(p[1]) << 8)
                    | (static_cast<std::uint32_t>(p[2]) << 16));
                if (code & 0x00800000)
                    code -= 0x01000000; // sign-extend
                view[i] = codeToFloat(code, 8388608.0);
            }
            break;
        case 32:
            if (isFloat)
            {
                for (std::size_t i = 0; i < numFrames; ++i, p += frameBytes)
                {
                    const std::uint32_t bits = readU32(p);
                    float sample;
                    std::memcpy(&sample, &bits, sizeof sample);
                    view[i] = sample;
                }
            }
            else
            {
                for (std::size_t i = 0; i < numFrames; ++i, p += frameBytes)
                {
                    const auto code = static_cast<std::int32_t>(readU32(p));
                    view[i] = codeToFloat(code, 2147483648.0);
                }
            }
            break;
        default:
            break; // unreachable: depths validated above
        }
    }

    return { std::move(buffer), {} };
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

WavIo::WriteResult WavIo::write(const std::filesystem::path& path,
                                const AudioBuffer& buffer, BitDepth depth)
{
    const std::size_t channels = buffer.numChannels();
    if (channels < 1 || channels > 2)
        return { "unsupported channel count " + std::to_string(channels)
                 + " (only mono/stereo are supported)" };
    if (!(buffer.sampleRate > 0.0))
        return { "buffer.sampleRate must be positive (got "
                 + std::to_string(buffer.sampleRate) + ")" };

    const auto sampleRate =
        static_cast<std::uint32_t>(std::llround(buffer.sampleRate));
    const bool isFloat = depth == BitDepth::Float32;

    std::size_t bytesPerSample = 0;
    double scale = 0.0;
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    switch (depth)
    {
    case BitDepth::Int16:
        bytesPerSample = 2; scale = 32768.0; lo = -32768; hi = 32767;
        break;
    case BitDepth::Int24:
        bytesPerSample = 3; scale = 8388608.0; lo = -8388608; hi = 8388607;
        break;
    case BitDepth::Int32:
        bytesPerSample = 4; scale = 2147483648.0;
        lo = std::numeric_limits<std::int32_t>::min();
        hi = std::numeric_limits<std::int32_t>::max();
        break;
    case BitDepth::Float32:
        bytesPerSample = 4;
        break;
    }

    const std::size_t numFrames = buffer.numFrames();
    const std::size_t frameBytes = bytesPerSample * channels;
    const std::size_t dataBytes = frameBytes * numFrames;

    std::vector<std::uint8_t> out;
    out.reserve(64 + dataBytes + (dataBytes & 1u));

    // RIFF header (size patched at the end).
    appendTag(out, "RIFF");
    appendU32(out, 0);
    appendTag(out, "WAVE");

    // fmt chunk. Canonical 16-byte PCM form; IEEE float gets the 18-byte form
    // (cbSize = 0) plus a fact chunk, per the WAVE_FORMAT_IEEE_FLOAT spec.
    appendTag(out, "fmt ");
    appendU32(out, isFloat ? 18u : 16u);
    appendU16(out, isFloat ? kFormatIeeeFloat : kFormatPcm);
    appendU16(out, static_cast<std::uint16_t>(channels));
    appendU32(out, sampleRate);
    appendU32(out, static_cast<std::uint32_t>(sampleRate * frameBytes));
    appendU16(out, static_cast<std::uint16_t>(frameBytes));
    appendU16(out, static_cast<std::uint16_t>(bytesPerSample * 8));
    if (isFloat)
    {
        appendU16(out, 0); // cbSize
        appendTag(out, "fact");
        appendU32(out, 4);
        appendU32(out, static_cast<std::uint32_t>(numFrames));
    }

    // data chunk, interleaved frames.
    appendTag(out, "data");
    appendU32(out, static_cast<std::uint32_t>(dataBytes));

    for (std::size_t i = 0; i < numFrames; ++i)
    {
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            const float sample = buffer.channel(ch)[i];
            switch (depth)
            {
            case BitDepth::Int16:
            {
                const auto code = floatToCode(sample, scale, lo, hi);
                appendU16(out, static_cast<std::uint16_t>(
                                   static_cast<std::int16_t>(code)));
                break;
            }
            case BitDepth::Int24:
            {
                const auto code =
                    static_cast<std::uint32_t>(floatToCode(sample, scale, lo, hi));
                out.push_back(static_cast<std::uint8_t>(code & 0xff));
                out.push_back(static_cast<std::uint8_t>((code >> 8) & 0xff));
                out.push_back(static_cast<std::uint8_t>((code >> 16) & 0xff));
                break;
            }
            case BitDepth::Int32:
            {
                const auto code = floatToCode(sample, scale, lo, hi);
                appendU32(out, static_cast<std::uint32_t>(
                                   static_cast<std::int32_t>(code)));
                break;
            }
            case BitDepth::Float32:
            {
                std::uint32_t bits;
                std::memcpy(&bits, &sample, sizeof bits);
                appendU32(out, bits);
                break;
            }
            }
        }
    }

    if (dataBytes & 1u)
        out.push_back(0); // RIFF word alignment pad

    // Patch the RIFF chunk size: whole file minus the 8-byte RIFF header.
    const auto riffSize = static_cast<std::uint32_t>(out.size() - 8);
    out[4] = static_cast<std::uint8_t>(riffSize & 0xff);
    out[5] = static_cast<std::uint8_t>((riffSize >> 8) & 0xff);
    out[6] = static_cast<std::uint8_t>((riffSize >> 16) & 0xff);
    out[7] = static_cast<std::uint8_t>((riffSize >> 24) & 0xff);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return { "cannot open file for writing: " + path.string() };
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    file.flush();
    if (!file.good())
        return { "write error: " + path.string() };

    return {};
}

} // namespace mws::core
