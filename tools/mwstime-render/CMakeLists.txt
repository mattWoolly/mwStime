# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# mwstime-render: the JUCE-free CLI golden-render driver
# (docs/design/architecture.md §2 tools layer — links ONLY mwstime_core;
# task plan/backlog/025-mwstime-render-cli.md).

add_executable(mwstime-render
    main.cpp
    MiniJson.cpp
)

target_link_libraries(mwstime-render PRIVATE mwstime_core)
