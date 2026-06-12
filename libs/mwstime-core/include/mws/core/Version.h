// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include <cstdint>
#include <string_view>

namespace mws::core {

/// Engine version string. Bump when any change can alter rendered output;
/// stored in render metadata so a reloaded session re-renders
/// deterministically (docs/design/architecture.md §6).
inline constexpr std::string_view kEngineVersion = "0.1.0";

/// FNV-1a 64-bit hash (compile-time) — used to derive the engine version hash.
constexpr std::uint64_t fnv1a64(std::string_view text) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : text)
    {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x00000100000001b3ull;
    }
    return hash;
}

/// Stable 64-bit hash of the engine version, for compact render metadata.
inline constexpr std::uint64_t kEngineVersionHash = fnv1a64(kEngineVersion);

/// Runtime accessors (defined in src/core/Version.cpp) so the constants are
/// also reachable through the compiled static library, not only the header.
[[nodiscard]] std::string_view engineVersion() noexcept;
[[nodiscard]] std::uint64_t engineVersionHash() noexcept;

} // namespace mws::core
