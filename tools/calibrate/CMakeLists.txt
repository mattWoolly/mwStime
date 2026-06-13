# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# calibrate: the SpliceCal / D-TIME hardware-capture calibration tool
# (docs/qa/hardware-capture-plan.md; task plan/backlog/026b). JUCE-free; links
# only mwstime_core (the engines + WavIo) and the shared mwstime_render_lib
# case schema (so the manifest parses identically to tests/golden/cases.json).
# It is the analysis half of the hardware-capture calibration loop
# (docs/design/testing-strategy.md §7 Wave 2; architecture.md §10 risk 1) and
# the v1-freeze gate for the S950 D-TIME mapping (dsp-engine.md §5).

# Same FP discipline as the core (architecture.md §7): the fitter re-renders the
# deterministic core and compares, so it must not contract/fast-math either.
set(MWSTIME_CALIBRATE_FP_OPTS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>)

# The fitter + manifest loader live in a small static lib so the self-test can
# link and exercise them directly alongside driving the real binary.
add_library(calibrate_lib STATIC
    Calibrate.cpp
    Manifest.cpp
)
target_include_directories(calibrate_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# mwstime_render_lib brings the shared cases.json schema/loader (Cases.h, Json.h)
# and transitively mwstime_core.
target_link_libraries(calibrate_lib PUBLIC mwstime_render_lib)
target_compile_features(calibrate_lib PUBLIC cxx_std_20)
target_compile_options(calibrate_lib PRIVATE ${MWSTIME_CALIBRATE_FP_OPTS})

add_executable(calibrate main.cpp)
target_link_libraries(calibrate PRIVATE calibrate_lib)
target_compile_features(calibrate PRIVATE cxx_std_20)
target_compile_options(calibrate PRIVATE ${MWSTIME_CALIBRATE_FP_OPTS})

# calibrate self-test (acceptance criterion: ctest `calibrate` recovers planted
# SpliceCal values from synthetic captures). Catch2 is fetched at the top level
# for tests only.
if(TARGET Catch2::Catch2WithMain)
    include(Catch)
    add_executable(calibrate_tests test_calibrate.cpp)
    target_link_libraries(calibrate_tests
        PRIVATE
            calibrate_lib
            Catch2::Catch2WithMain
    )
    target_compile_features(calibrate_tests PRIVATE cxx_std_20)
    target_compile_options(calibrate_tests PRIVATE ${MWSTIME_CALIBRATE_FP_OPTS})

    # Inject the built binary path so the test also drives the real CLI exit
    # paths (the same one a QA agent runs against a local capture dir).
    target_compile_definitions(calibrate_tests PRIVATE
        "MWSTIME_CALIBRATE_BINARY=\"$<TARGET_FILE:calibrate>\"")

    # The self-test writes/reads small WAVs + a manifest in its working
    # directory; give it a private scratch dir so parallel ctest runs never
    # collide on those filenames (mirrors the rendercli pattern).
    set(calibrate_workdir "${CMAKE_CURRENT_BINARY_DIR}/calibrate_scratch")
    file(MAKE_DIRECTORY "${calibrate_workdir}")

    # Test-case names begin with "calibrate" so `ctest -R calibrate` selects the
    # suite (plan/backlog/README.md test-selection rules); the verification
    # command pairs this with --no-tests=error (silent-pass guard).
    catch_discover_tests(calibrate_tests
        WORKING_DIRECTORY "${calibrate_workdir}")
endif()
