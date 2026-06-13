# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# UI golden-screenshot regression harness (task plan/backlog/047b; docs/design/
# ui-design.md §4, docs/design/testing-strategy.md §7 Wave 3, ADR-005). Builds
# the headless `ui_screenshot` driver (capture + comparer, software renderer),
# registers ONE CTest per defined state under LABEL `ui-golden`, and defines the
# gated `bless_ui_goldens` target. Mirrors the task-026 golden-render harness
# structure (per-case CTest, bless script, committed plain-git goldens).
#
# Only built when the JUCE plugin is built (the driver links the UI components +
# JUCE GUI). On non-reference platforms the per-state runner downgrades the exact
# gate to a tolerance compare automatically (the driver detects the reference
# platform at compile time) — never silently skips on the reference platform.

include(Catch)

juce_add_console_app(ui_screenshot
    PRODUCT_NAME "ui_screenshot"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
)

target_sources(ui_screenshot
    PRIVATE
        screenshot/main.cpp
        screenshot/CaptureStates.cpp
        # The real, shipping UI components — captured exactly as the editor draws
        # them (one parametric Faceplate + the hero LcdDisplay driven by the
        # headless LcdPageModel; ADR-005). No PluginEditor: the capture path is
        # pure so it is deterministic (no timers/animations/file IO).
        "${CMAKE_SOURCE_DIR}/plugin/ui/Faceplate.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdDisplay.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdPageBinding.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdPageModel.cpp"
        # Faceplate.cpp draws header text via the look-and-feel font helpers.
        "${CMAKE_SOURCE_DIR}/plugin/ui/lookandfeel/SeriesLookAndFeel.cpp"
)

target_include_directories(ui_screenshot
    PRIVATE
        "${CMAKE_SOURCE_DIR}/plugin/src"   # EngineHost.h (WorkerEvent types) for LcdPageBinding
        "${CMAKE_SOURCE_DIR}/plugin"       # ui/... include paths
)

target_compile_definitions(ui_screenshot
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=0
)

target_link_libraries(ui_screenshot
    PRIVATE
        mwstime_core
        juce::juce_gui_basics
        juce::juce_graphics
        juce::juce_audio_basics   # EngineHost.h pulls juce::AbstractFifo
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# --- paths ------------------------------------------------------------------
set(UI_BLESSED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/blessed")
set(UI_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/ui_scratch")
file(MAKE_DIRECTORY "${UI_SCRATCH}")

set(UI_BIN "$<TARGET_FILE:ui_screenshot>")
set(RUN_UI_CASE_SH "${CMAKE_SOURCE_DIR}/tools/run_ui_golden_case.sh")
set(BLESS_UI_SH "${CMAKE_SOURCE_DIR}/tools/bless_ui_goldens.sh")

# --- per-state CTest registration -------------------------------------------
# The state ids are the single source of truth in CaptureStates.cpp; we mirror
# the list here for the CTest registration. Each state name begins with
# "ui-golden" so `ctest -R ui-golden` selects them (backlog/README silent-pass
# rule) AND carries LABEL `ui-golden` so `ctest -L ui-golden` selects exactly
# this suite. Keep this list in sync with CaptureStates.cpp::states() — a stale
# entry fails because the runner cannot capture an unknown id, and a missing
# blessed PNG fails the case loudly (run_ui_golden_case refuses on absence).
set(UI_GOLDEN_STATES
    faceplate_s900_time
    faceplate_s950_time
    faceplate_s1000_time
    faceplate_s1100_time
    lcd_s950_2line
    lcd_fx_realtime
    faceplate_s1000_scale06
    faceplate_s1000_scale20
)

foreach(state IN LISTS UI_GOLDEN_STATES)
    add_test(
        NAME "ui-golden: ${state}"
        COMMAND "${RUN_UI_CASE_SH}"
                "${UI_BIN}" "${UI_BLESSED_DIR}" "${UI_SCRATCH}" "${state}"
    )
    set_tests_properties("ui-golden: ${state}" PROPERTIES LABELS ui-golden)
endforeach()

list(LENGTH UI_GOLDEN_STATES UI_GOLDEN_COUNT)
message(STATUS "ui golden harness: registered ${UI_GOLDEN_COUNT} states (label `ui-golden`)")

# --- bless target -----------------------------------------------------------
# `cmake --build --preset default --target bless_ui_goldens` re-captures every
# state into tests/ui/blessed/ and writes MANIFEST.json. The script REFUSES
# unless BLESS_REASON is set (the gate; mirrors task-026 bless_goldens).
add_custom_target(bless_ui_goldens
    COMMAND "${CMAKE_COMMAND}" -E echo
            "bless_ui_goldens: invoking tools/bless_ui_goldens.sh (BLESS_REASON gate enforced by the script)"
    COMMAND "${BLESS_UI_SH}" "${UI_BIN}" "${UI_BLESSED_DIR}"
    DEPENDS ui_screenshot
    VERBATIM
    USES_TERMINAL
)
