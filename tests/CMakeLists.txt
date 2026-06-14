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
    # Host smoke matrix (task 048b): the headless Standalone no-transport SYNC
    # fallback row (testing-strategy.md §6 Standalone; ADR-006). JUCE-free — it
    # drives RealtimeStretcher directly, the exact engine the Standalone app
    # runs, with NO window and NO transport (the task's headless-safe rule).
    unit/test_host_smoke.cpp
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
    # FX history-reconfiguration handoff + processor-level null / automation-spam
    # (task 033). Lives here (not in the JUCE plugin test binary) because
    # FxEngine.h is JUCE-free — so the FX reconfiguration TSan path runs under the
    # `tsan` preset, which has MWS_BUILD_PLUGIN=OFF (testing-strategy.md §3.6
    # names this path explicitly).
    unit/test_enginehost_fx_reconfig.cpp
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

# FX history reconfiguration TSan path (task 033 deliverable: "the model/
# bandwidth/FS reconfiguration handoff test carries CTest label `tsan` and runs
# under the task-030 `tsan` preset" — testing-strategy.md §3.6). The name begins
# with "enginehost" so `ctest -R enginehost` selects it too, and `ctest -L tsan`
# selects exactly the [tsan]-tagged FX-reconfig concurrency case.
add_test(
    NAME "enginehost: tsan-labelled FX history reconfiguration handoff"
    COMMAND mwstime_tests "[enginehost][tsan]"
)
set_tests_properties(
    "enginehost: tsan-labelled FX history reconfiguration handoff"
    PROPERTIES LABELS tsan
)

# Host smoke matrix (tests/plugin/host-matrix.md — task 048b; testing-strategy.md
# §6). The ONE row that must actually EXECUTE headlessly is the Standalone
# no-transport SYNC fallback (ADR-006): a JUCE-free RealtimeStretcher harness
# (test_host_smoke.cpp, [host-smoke]-tagged) drives the exact engine the
# Standalone app runs with no window and no play head. catch_discover_tests above
# already registers each "host-smoke: ..." case individually (so
# `ctest -R host-smoke` selects them — backlog/README silent-pass rule), but
# Catch2 3.x does not map tags to CTest LABELS, so register the [host-smoke]-
# tagged set as one labelled CTest. The name begins with "host-smoke" so a
# `ctest -R host-smoke` name selector also matches it, and `ctest -L host-smoke`
# selects exactly the headless Standalone row (+ the REAPER self-skip below).
add_test(
    NAME "host-smoke: Standalone no-transport SYNC fallback (headless)"
    COMMAND mwstime_tests "[host-smoke]"
)
set_tests_properties(
    "host-smoke: Standalone no-transport SYNC fallback (headless)"
    PROPERTIES LABELS host-smoke
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

    # UI golden-screenshot regression harness (tests/ui/ — task 047b). Builds the
    # headless ui_screenshot driver (software renderer), registers one CTest per
    # state under LABEL `ui-golden`, and defines the gated `bless_ui_goldens`
    # target. JUCE-linked (it captures the real UI components), so it lives under
    # the same plugin-build guard.
    add_subdirectory(ui)
endif()

# License-header convention check (docs/CONTRIBUTING.md). Every committed C++
# source under libs/ plugin/ tools/ tests/ must carry the SPDX header; every
# later task inherits this gate automatically.
#
# Guarded to non-Windows (task 050): the checker is a POSIX/bash script
# (git ls-files + head + grep). It validates COMMITTED source headers, which are
# identical on every checkout, so the macOS/Linux ctest runs cover it fully;
# registering a .sh CTest on Windows would hard-fail (CTest cannot exec it)
# rather than add coverage. Not in scope to port a repo-content lint to .ps1.
if(NOT WIN32)
    add_test(
        NAME license_headers
        COMMAND "${CMAKE_SOURCE_DIR}/tools/check_license_headers.sh" "${CMAKE_SOURCE_DIR}"
    )
endif()
