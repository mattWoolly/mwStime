# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

include(Catch)

add_executable(mwstime_tests
    unit/SmokeTests.cpp
    unit/test_autocorr.cpp
    unit/test_autocycle.cpp
    unit/test_buffer.cpp
    unit/test_butterworth.cpp
    unit/test_characterchain.cpp
    unit/test_cyclic_classic.cpp
    unit/test_cyclic_properties.cpp
    unit/test_cyclic_revised.cpp
    unit/test_modelspec.cpp
    unit/test_movinglpf.cpp
    unit/test_offline_renderer.cpp
    unit/test_quantizer.cpp
    unit/test_wavio.cpp
    unit/test_resampler.cpp
    unit/test_repitch.cpp
    unit/test_rtstretch_free.cpp
    unit/test_rtstretch_sync.cpp
    unit/test_s950.cpp
    unit/test_s1000chain.cpp
    unit/test_stream_offline_equivalence.cpp
    unit/test_tempomap.cpp
    unit/test_transpose.cpp
    unit/test_varclock.cpp
    # Threading: RCU publication protocol stress / TSan (task 030). Lives in the
    # JUCE-free core test binary because Published.h depends only on the C++20
    # standard library — so it builds and runs cleanly under the `tsan` preset.
    plugin/test_publication.cpp
)

target_include_directories(mwstime_tests
    PRIVATE
        # Published.h (the RCU publication mechanism, task 030) lives in the
        # plugin tree but is deliberately JUCE-free; the core test binary
        # includes it directly for the publication stress / TSan test.
        "${CMAKE_SOURCE_DIR}/plugin/src"
        # TestAllocationCounter.h (shared global new-counter, task 005) so the
        # publication test can assert the audio-thread path allocates nothing.
        "${CMAKE_CURRENT_SOURCE_DIR}/unit"
)

find_package(Threads REQUIRED)

target_link_libraries(mwstime_tests
    PRIVATE
        mwstime_core
        Catch2::Catch2WithMain
        Threads::Threads
)

catch_discover_tests(mwstime_tests)

# Explicit CTest entry for the publish/swap concurrency subset so it carries the
# `tsan` LABEL (task 030 deliverable: "the publish/swap and abort tests carry
# CTest label `tsan` and run under that preset"). Catch2 3.7.x does not map tags
# to CTest labels, so we register the [tsan]-tagged Catch cases as one labelled
# test. The name begins with "publication" so `ctest -R publication` selects it
# too, and `ctest -L tsan` selects exactly the concurrency cases.
add_test(
    NAME "publication: tsan-labelled concurrency (publish/swap + graveyard)"
    COMMAND mwstime_tests "[tsan]"
)
set_tests_properties(
    "publication: tsan-labelled concurrency (publish/swap + graveyard)"
    PROPERTIES LABELS tsan
)

# Golden-render character regression (tests/golden/ — task 026). Registers one
# CTest per cases.json case (LABEL `golden`), the `bless_goldens` target, and the
# input-corpus reproducibility check. JUCE-free: it drives the mwstime_render +
# golden_compare CLIs, so it builds and runs without the plugin.
add_subdirectory(golden)

# JUCE-linked plugin tests (tests/plugin/ — harness created by task 028 and
# reused by every later tests/plugin/ task). Only built when the plugin is.
if(MWS_BUILD_PLUGIN AND TARGET juce::juce_audio_processors)
    add_subdirectory(plugin)
endif()

# License-header convention check (docs/CONTRIBUTING.md). Every committed C++
# source under libs/ plugin/ tools/ tests/ must carry the SPDX header; every
# later task inherits this gate automatically.
add_test(
    NAME license_headers
    COMMAND "${CMAKE_SOURCE_DIR}/tools/check_license_headers.sh" "${CMAKE_SOURCE_DIR}"
)
