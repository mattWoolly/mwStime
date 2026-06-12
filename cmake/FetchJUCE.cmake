# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# JUCE 8.x via FetchContent, pinned tag (docs/design/architecture.md §7).
# AGPLv3 build: JUCE is used under its GPLv3 option (GPL-compatible).

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.13
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(JUCE)
