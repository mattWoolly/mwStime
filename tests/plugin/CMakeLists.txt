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
