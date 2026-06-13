# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# mwstime_render: the JUCE-free command-line renderer (wav in -> wav out, all
# params) over mws::engine::OfflineRenderer + mws::core::WavIo
# (docs/design/architecture.md §2, §8; task plan/backlog/025-mwstime-render-cli.md).
#
# Links ONLY mwstime_core (hard rule — no JUCE in the tools layer,
# architecture.md §2). It is the driver for golden tests (task 026) and the
# agent DSP-debugging tool: DSP work never requires a host.

# Same FP discipline as the core (architecture.md §7): the CLI is the golden
# render driver, so it must not enable contraction/fast-math either.
set(MWSTIME_RENDER_FP_OPTS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>)

# The CLI logic (arg parsing, the hand-rolled JSON reader, the case-file loader)
# lives in a small static lib so the smoke test can link and exercise it
# alongside driving the real binary.
add_library(mwstime_render_lib STATIC
    Args.cpp
    Cases.cpp
    Json.cpp
)
target_include_directories(mwstime_render_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mwstime_render_lib PUBLIC mwstime_core)
target_compile_features(mwstime_render_lib PUBLIC cxx_std_20)
target_compile_options(mwstime_render_lib PRIVATE ${MWSTIME_RENDER_FP_OPTS})

add_executable(mwstime_render main.cpp)
target_link_libraries(mwstime_render PRIVATE mwstime_render_lib)
target_compile_features(mwstime_render PRIVATE cxx_std_20)
target_compile_options(mwstime_render PRIVATE ${MWSTIME_RENDER_FP_OPTS})

# The shipped binary is the hyphenated `mwstime-render` (the name used in
# docs/design/testing-strategy.md §4 and the task verification commands); the
# CMake target keeps the underscore form for the usual identifier rules.
set_target_properties(mwstime_render PROPERTIES OUTPUT_NAME "mwstime-render")

# rendercli smoke test (acceptance criterion: CTest [rendercli] passes; binary
# links only mwstime_core). Catch2 is fetched at the top level for tests only.
if(TARGET Catch2::Catch2WithMain)
    include(Catch)
    add_executable(mwstime_render_tests test_rendercli.cpp)
    target_link_libraries(mwstime_render_tests
        PRIVATE
            mwstime_render_lib
            Catch2::Catch2WithMain
    )
    target_compile_features(mwstime_render_tests PRIVATE cxx_std_20)
    target_compile_options(mwstime_render_tests PRIVATE ${MWSTIME_RENDER_FP_OPTS})

    # Inject the built binary path so the test drives the real CLI exec path
    # (the same one the golden runner uses, testing-strategy.md §4).
    target_compile_definitions(mwstime_render_tests PRIVATE
        "MWSTIME_RENDER_BINARY=\"$<TARGET_FILE:mwstime_render>\"")

    # The smoke test writes/reads small WAVs + a one-case cases.json in its
    # working directory; give it a private scratch dir so parallel ctest runs
    # never collide on those filenames.
    set(rendercli_workdir "${CMAKE_CURRENT_BINARY_DIR}/rendercli_scratch")
    file(MAKE_DIRECTORY "${rendercli_workdir}")

    catch_discover_tests(mwstime_render_tests
        WORKING_DIRECTORY "${rendercli_workdir}")
endif()
