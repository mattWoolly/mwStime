# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# JUCE-linked Catch2 plugin test target (task 028) — THE harness every later
# tests/plugin/ task reuses: add sources here, don't create new targets.
# Plugin code under test is compiled in directly (no JucePlugin_* defines
# needed for state/ code); Catch2WithMain provides main(), the JUCE console-app
# wrapper provides module setup (JUCE_STANDALONE_APPLICATION, frameworks).

include(Catch)

juce_add_console_app(mwstime_plugin_tests
    PRODUCT_NAME "mwstime_plugin_tests"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
)

target_sources(mwstime_plugin_tests
    PRIVATE
        test_parameters.cpp
        test_faceplatespec.cpp
        test_state.cpp
        test_state_embed.cpp
        test_presets.cpp
        test_lcdfont.cpp
        test_lcdpagemodel.cpp
        test_inputcluster.cpp
        test_waveformview.cpp
        test_controlpanel.cpp
        test_enginehost.cpp
        test_enginehost_fx.cpp
        test_temposync.cpp
        test_fileloader.cpp
        test_sample_mode.cpp
        test_dsp_adversarial.cpp
        test_midivoice.cpp
        test_export.cpp
        test_alloc_counter.cpp
        test_editor_wiring.cpp
        test_editor_actions.cpp
        test_softkey_feedback.cpp
        test_modelswitch.cpp
        test_resize.cpp
        "${CMAKE_SOURCE_DIR}/plugin/src/EngineHost.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/MidiVoice.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/ExportService.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/FileLoader.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/Parameters.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/StateTree.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/StateBlobCache.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/Migrations.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/FactoryPresets.cpp"
        # The canonical golden cases.json loader (JUCE-free, mws/engine/Params.h
        # only) — reused so the factory-preset cross-check (task 038) shares ONE
        # parameter vocabulary with the golden renderer; drift fails the build.
        "${CMAKE_SOURCE_DIR}/tools/mwstime-render/Cases.cpp"
        "${CMAKE_SOURCE_DIR}/tools/mwstime-render/Args.cpp"
        "${CMAKE_SOURCE_DIR}/tools/mwstime-render/Json.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/ControlPanel.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/EditorActions.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/Faceplate.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdDisplay.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdFieldEditor.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdPageBinding.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdPageModel.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/JogWheel.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/ModelSelector.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/SoftKeyBar.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/WaveformView.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/lookandfeel/SeriesLookAndFeel.cpp"
)

target_include_directories(mwstime_plugin_tests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/plugin/src"
        "${CMAKE_SOURCE_DIR}/plugin"
        # TestAllocationCounter.h (shared global new-counter, task 005) so the
        # SAMPLE-mode audio-path no-allocation tests (task 034) can assert it.
        "${CMAKE_SOURCE_DIR}/tests/unit"
        # Cases.h / Json.h / Args.h for the factory-preset cross-check (task 038).
        "${CMAKE_SOURCE_DIR}/tools/mwstime-render"
)

target_compile_definitions(mwstime_plugin_tests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=0
        # Absolute path to tests/golden/cases.json — the factory-preset test
        # (task 038) reads it in-test to cross-check presets against the goldens.
        MWS_CASES_JSON_PATH="${CMAKE_SOURCE_DIR}/tests/golden/cases.json"
)

target_link_libraries(mwstime_plugin_tests
    PRIVATE
        mwstime_core
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_basics
        Catch2::Catch2WithMain
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

catch_discover_tests(mwstime_plugin_tests)

# Adversarial DSP QA (test_dsp_adversarial.cpp — task 034b). catch_discover_tests
# above already registers each "qa-adversarial: ..." case individually (so
# `ctest -R qa-adversarial` selects them — README test-selection rules), but
# Catch2 3.x does not map tags to CTest LABELS. Register the [qa-adversarial]-
# tagged set as one labelled CTest so `ctest -L qa-adversarial` selects exactly
# Wave 1 of the adversarial fleet (testing-strategy.md §7). Catch2's `[.]`-hidden
# prefix only excludes a case from the NO-FILTER default run; an explicit tag
# filter like `[qa-adversarial]` DOES match the `[.soak]` case (it carries the
# `[qa-adversarial]` tag too). So we must exclude the 30-min soak ourselves with
# the `~[.soak]` term — `[qa-adversarial]~[.soak]` selects exactly the 7 fast
# Wave 1 cases. The soak is invoked explicitly by tag instead (see the file header).
add_test(
    NAME "qa-adversarial: Wave 1 DSP adversaries (labelled set)"
    COMMAND mwstime_plugin_tests "[qa-adversarial]~[.soak]"
)
set_tests_properties(
    "qa-adversarial: Wave 1 DSP adversaries (labelled set)"
    PROPERTIES LABELS qa-adversarial
)

# ---------------------------------------------------------------------------
# Format validators (task 048 macOS; task 049 Linux; task 050 Windows;
# testing-strategy.md §5, architecture.md §3/§7, plan/decisions/002).
#   * macOS : pluginval (VST3+AU, strictness 10), auval (-v aumf MwS1 MwSt),
#             clap-validator (CLAP).
#   * Linux : pluginval (VST3 — AU is macOS-only), clap-validator (CLAP), and
#             the LV2 checks (lv2lint + lv2_validate/sord_validate + REQUIRED
#             Carla load smoke; xvfb-wrapped on headless boxes).
#   * Windows : pluginval (VST3 — strictness 10), clap-validator (CLAP). No AU
#             (macOS-only) and no LV2 (the Windows set is VST3 + CLAP +
#             Standalone — task 050; LV2 is dropped on Windows). The .sh scripts
#             are POSIX/bash, so Windows runs PowerShell ports (run_*.ps1) via
#             `powershell.exe -File` (task 050: ".ps1 equivalents where needed").
#
# Each script self-skips with exit 77 when its tool can't be fetched/installed
# (offline / package absent) or when no built artifact is present, so
# SKIP_RETURN_CODE 77 makes `ctest` GREEN on a tools-absent box. pluginval and
# clap-validator download PINNED releases into build/ (per-OS URL+digest, never
# committed); the LV2 tools come from the system package manager (docs/BUILDING.md).
# LABEL `validators` lets the task's command
#   ctest --preset <preset> -L validators --no-tests=error
# select exactly these; the `validators:`-prefixed names also satisfy a
# `ctest -R validators` name selector (backlog/README silent-pass rule).
#
# MWS_BUILD_DIR is passed so the scripts locate the artefacts regardless of the
# ctest working directory. DISABLED is left OFF (acceptance criterion) — the
# self-skip is the gate, not a CTest DISABLED flag.
set(MWS_VALIDATOR_SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(MWS_VALIDATOR_NAMES "")   # collected to set shared properties once

if(WIN32)
    # PowerShell host: prefer pwsh (PowerShell 7+) if present, else the built-in
    # Windows PowerShell. -File runs the script; -ExecutionPolicy Bypass avoids a
    # machine policy blocking an unsigned local script.
    find_program(MWS_POWERSHELL NAMES pwsh powershell)
    if(NOT MWS_POWERSHELL)
        set(MWS_POWERSHELL "powershell")  # always present on Windows
    endif()
    add_test(
        NAME "validators: pluginval VST3 (strictness 10)"
        COMMAND "${MWS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${MWS_VALIDATOR_SCRIPT_DIR}/run_pluginval.ps1"
    )
    add_test(
        NAME "validators: clap-validator CLAP"
        COMMAND "${MWS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${MWS_VALIDATOR_SCRIPT_DIR}/run_clap_validator.ps1"
    )
    list(APPEND MWS_VALIDATOR_NAMES
        "validators: pluginval VST3 (strictness 10)"
        "validators: clap-validator CLAP"
    )
elseif(APPLE)
    add_test(
        NAME "validators: pluginval VST3+AU (strictness 10)"
        COMMAND "${MWS_VALIDATOR_SCRIPT_DIR}/run_pluginval.sh"
    )
    add_test(
        NAME "validators: auval -v aumf MwS1 MwSt"
        COMMAND "${MWS_VALIDATOR_SCRIPT_DIR}/run_auval.sh"
    )
    add_test(
        NAME "validators: clap-validator CLAP"
        COMMAND "${MWS_VALIDATOR_SCRIPT_DIR}/run_clap_validator.sh"
    )
    list(APPEND MWS_VALIDATOR_NAMES
        "validators: pluginval VST3+AU (strictness 10)"
        "validators: auval -v aumf MwS1 MwSt"
        "validators: clap-validator CLAP"
    )
elseif(UNIX)   # Linux (task 049) — VST3 + CLAP + LV2; no AU/auval (macOS-only).
    add_test(
        NAME "validators: pluginval VST3 (strictness 10)"
        COMMAND "${MWS_VALIDATOR_SCRIPT_DIR}/run_pluginval.sh"
    )
    add_test(
        NAME "validators: clap-validator CLAP"
        COMMAND "${MWS_VALIDATOR_SCRIPT_DIR}/run_clap_validator.sh"
    )
    add_test(
        NAME "validators: LV2 lv2lint+lv2_validate+Carla"
        COMMAND "${MWS_VALIDATOR_SCRIPT_DIR}/run_lv2_checks.sh"
    )
    list(APPEND MWS_VALIDATOR_NAMES
        "validators: pluginval VST3 (strictness 10)"
        "validators: clap-validator CLAP"
        "validators: LV2 lv2lint+lv2_validate+Carla"
    )
endif()

if(MWS_VALIDATOR_NAMES)
    set_tests_properties(
        ${MWS_VALIDATOR_NAMES}
        PROPERTIES
            LABELS validators
            SKIP_RETURN_CODE 77
            # pluginval --repeat 3 --randomise at strictness 10, the auval render
            # matrix, and the Carla load smoke can take a while; generous headroom.
            TIMEOUT 1200
            # Pass the active build dir so the scripts find the artefacts no
            # matter what directory ctest invokes them from.
            ENVIRONMENT "MWS_BUILD_DIR=${CMAKE_BINARY_DIR}"
    )
endif()

# ---------------------------------------------------------------------------
# Host smoke matrix (tests/plugin/host-matrix.md — task 048b; testing-strategy.md
# §6). The REAPER row is the one DAW row that CAN be scripted ("scripted where
# possible" — §6): REAPER has a batch/ReaScript runner. run_reaper_smoke.sh
# probes for REAPER via PATH / /Applications ONLY (never launches a GUI, never
# installs — task 048b CRITICAL execution constraints) and self-skips with exit
# 77 when REAPER is absent, so SKIP_RETURN_CODE 77 keeps `ctest -L host-smoke`
# GREEN on a REAPER-absent box (the norm here and in CI). Cross-platform (macOS +
# Linux REAPER); the script picks the right probe per OS. LABEL `host-smoke`
# selects it together with the headless Standalone row registered in
# tests/CMakeLists.txt; the `host-smoke:`-prefixed name also satisfies a
# `ctest -R host-smoke` name selector (backlog/README silent-pass rule).
#
# Guarded to non-Windows (task 050): the runner is a POSIX/bash script and the
# REAPER host-smoke row is macOS/Linux only (Windows host smoke is out of scope —
# task 050). Registering a .sh CTest on Windows would hard-fail (CTest cannot
# exec it) instead of cleanly skipping, so it is simply not registered there.
if(NOT WIN32)
    add_test(
        NAME "host-smoke: REAPER scripted smoke (insert/automation/state/render/PDC)"
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_reaper_smoke.sh"
    )
    set_tests_properties(
        "host-smoke: REAPER scripted smoke (insert/automation/state/render/PDC)"
        PROPERTIES
            LABELS host-smoke
            SKIP_RETURN_CODE 77
            TIMEOUT 600
            ENVIRONMENT "MWS_BUILD_DIR=${CMAKE_BINARY_DIR}"
    )
endif()
