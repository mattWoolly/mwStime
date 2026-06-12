# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# clap-juce-extensions via FetchContent, pinned commit.
#
# IMPORTANT: clap-juce-extensions is an unofficial shim that can lag JUCE
# releases (docs/design/architecture.md §3, plan/decisions/002). Bumping the
# pin below is an EXPLICIT backlog task, never an incidental upgrade — do not
# change it as a side effect of other work.
#
# Pinned to a commit hash, not a release tag: the newest upstream release tag
# (0.26.0) predates JUCE 7/8 support and fails to compile against JUCE 8
# (includes JUCE-6-era wrapper paths). The commit below is upstream main as of
# 2026-06-01, which builds cleanly against the JUCE tag in FetchJUCE.cmake.
#
# Must be included AFTER cmake/FetchJUCE.cmake (the shim wraps JUCE targets).

include(FetchContent)

FetchContent_Declare(
    clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        51a9359315298de632cf44e9d7524940868441e6
)

FetchContent_MakeAvailable(clap-juce-extensions)
