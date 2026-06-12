// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include "mws/core/Version.h"

namespace mws::core {

std::string_view engineVersion() noexcept
{
    return kEngineVersion;
}

std::uint64_t engineVersionHash() noexcept
{
    return kEngineVersionHash;
}

} // namespace mws::core
