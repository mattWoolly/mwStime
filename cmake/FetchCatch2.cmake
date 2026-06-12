# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# Catch2 v3 via FetchContent, pinned tag (tests only — never linked by
# mwstime_core itself).

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(Catch2)

# Makes include(Catch) / catch_discover_tests available to tests/.
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
