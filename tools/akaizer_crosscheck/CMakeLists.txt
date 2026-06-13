# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# akaizer_crosscheck — the LOCAL-ONLY Akaizer secondary cross-check
# (task plan/backlog/026c-akaizer-crosscheck.md; docs/design/testing-strategy.md
# §7 Wave 2, §8; docs/qa/akaizer-crosscheck.md).
#
#   *** NEVER RUN IN CI. NOT A GOLDEN/REGRESSION GATE. ***
#
# Akaizer is closed payware whose "near-exact" fidelity claim was refuted 0-3
# (deep-research-report.md Finding 6); hardware captures are the oracle (task
# 026b). This builds a JUCE-free driver (akaizer-crosscheck) that produces a
# per-case DEVIATION REPORT, plus a self-test (`akzcheck`) of the comparison
# MATH on synthetic data — the self-test needs NO Akaizer binary, so it is the
# only Akaizer-related ctest and it is registered HERE (not in tests/CMakeLists.txt
# and never from CI, per the 026c acceptance criteria and the 051 CI rule).
#
# Links mwstime_render_lib (cases.json loader + ModelSpec) — same JUCE-free
# tool layer as mwstime-render (architecture.md §2).

# Same FP discipline as the core (architecture.md §7).
set(MWSTIME_AKZ_FP_OPTS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>)

add_executable(akaizer_crosscheck main.cpp)
target_include_directories(akaizer_crosscheck PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(akaizer_crosscheck PRIVATE mwstime_render_lib)
target_compile_features(akaizer_crosscheck PRIVATE cxx_std_20)
target_compile_options(akaizer_crosscheck PRIVATE ${MWSTIME_AKZ_FP_OPTS})

# The shipped binary is the hyphenated `akaizer-crosscheck` (the name used in
# docs/qa/akaizer-crosscheck.md); the CMake target keeps the underscore form.
set_target_properties(akaizer_crosscheck PROPERTIES OUTPUT_NAME "akaizer-crosscheck")

# `akzcheck` self-test of the comparison math on SYNTHETIC data. Registered as a
# ctest so the comparison math is verifiable WITHOUT any Akaizer binary present
# (026c acceptance criterion). The Catch test-case names begin with "akzcheck",
# so `ctest -R akzcheck` selects exactly these (silent-pass rule). Catch2 is
# fetched at the top level for tests only.
if(TARGET Catch2::Catch2WithMain)
    include(Catch)
    add_executable(akaizer_crosscheck_tests test_akzcheck.cpp)
    target_include_directories(akaizer_crosscheck_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(akaizer_crosscheck_tests
        PRIVATE
            mwstime_core
            Catch2::Catch2WithMain
    )
    target_compile_features(akaizer_crosscheck_tests PRIVATE cxx_std_20)
    target_compile_options(akaizer_crosscheck_tests PRIVATE ${MWSTIME_AKZ_FP_OPTS})
    catch_discover_tests(akaizer_crosscheck_tests)
endif()
