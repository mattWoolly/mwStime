// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

#include <filesystem>
#include <string>

namespace mws::core {

/// Minimal, dependency-free WAV reader/writer — the IO layer for the
/// `mwstime-render` CLI and the golden tests (docs/design/architecture.md §2).
/// Supported envelope (same as Akaizer, docs/research/akaizer-analysis.md §2.1):
/// 16/24/32-bit integer PCM and 32-bit IEEE float, mono/stereo. Whole-file IO
/// only; no exceptions cross the API — errors are reported via Result-style
/// return values whose `error` string is empty on success.
///
/// Deterministic conversion rules (tested in tests/unit/test_wavio.cpp):
///   - int -> float32:  sample / 2^(bits-1), computed in double then narrowed
///     (exact in float32 for 16- and 24-bit codes).
///   - float32 -> int:  clamp(llround(double(sample) * 2^(bits-1)),
///                            -2^(bits-1), 2^(bits-1) - 1)
///     i.e. round to nearest, ties away from zero, then clamp.
/// These rules make int round trips bit-exact for any code representable in a
/// float32 mantissa, and float32 round trips exact for all values.
class WavIo
{
public:
    enum class BitDepth
    {
        Int16,
        Int24,
        Int32,
        Float32,
    };

    struct ReadResult
    {
        AudioBuffer buffer;     ///< float32, source sample rate preserved.
        std::string error;      ///< empty on success.

        [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    };

    struct WriteResult
    {
        std::string error;      ///< empty on success.

        [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    };

    /// Reads a whole WAV file, converting to float32 and keeping the source
    /// sample rate. RIFF parsing is tolerant of extra/unknown chunks
    /// (including odd-sized ones) and of WAVE_FORMAT_EXTENSIBLE wrappers;
    /// unsupported codecs/bit depths/channel counts and malformed or
    /// truncated files are rejected with a descriptive `error`.
    [[nodiscard]] static ReadResult read(const std::filesystem::path& path);

    /// Writes the whole buffer as a canonical little-endian WAV at the given
    /// bit depth (interleaved frames; `fact` chunk emitted for Float32).
    /// `buffer.sampleRate` must be positive and the channel count 1 or 2.
    [[nodiscard]] static WriteResult write(const std::filesystem::path& path,
                                           const AudioBuffer& buffer,
                                           BitDepth depth);
};

} // namespace mws::core
