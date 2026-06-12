// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#include <catch2/catch_test_macros.hpp>

#include "mws/core/Version.h"

// Test-case names begin with the tag word so `ctest -R smoke` matches
// (plan/backlog/README.md test-selection rules).

TEST_CASE("smoke: engine version constant is exposed and consistent", "[smoke]")
{
    REQUIRE(mws::core::kEngineVersion == "0.1.0");
    REQUIRE_FALSE(mws::core::engineVersion().empty());
    REQUIRE(mws::core::engineVersion() == mws::core::kEngineVersion);
}

TEST_CASE("smoke: engine version hash matches the version string", "[smoke]")
{
    STATIC_REQUIRE(mws::core::kEngineVersionHash != 0);
    REQUIRE(mws::core::engineVersionHash() == mws::core::kEngineVersionHash);
    REQUIRE(mws::core::engineVersionHash()
            == mws::core::fnv1a64(mws::core::engineVersion()));
}
