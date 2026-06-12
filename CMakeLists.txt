# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

cmake_minimum_required(VERSION 3.25)

project(mwStime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# FP discipline for the deterministic core (docs/design/architecture.md §7):
#   - no -ffast-math anywhere in libs/ (never added; do not add it),
#   - -ffp-contract=off on mwstime_core (pinned double rounding for golden
#     renders) — applied on the target in libs/mwstime-core/CMakeLists.txt.
# ---------------------------------------------------------------------------

option(MWS_BUILD_PLUGIN "Build the JUCE plugin (plugin/ — task 027)" ON)

include(cmake/FetchCatch2.cmake)

add_subdirectory(libs/mwstime-core)   # mwstime_core (static, JUCE-free)

# CLI golden-render driver — task 025:
# add_subdirectory(tools/mwstime-render)

# JUCE plugin — task 027. Guarded: the directory may not exist yet.
if(MWS_BUILD_PLUGIN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugin/CMakeLists.txt")
    add_subdirectory(plugin)
endif()

enable_testing()
add_subdirectory(tests)
