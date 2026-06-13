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
        test_lcdfont.cpp
        test_lcdpagemodel.cpp
        test_inputcluster.cpp
        test_waveformview.cpp
        test_controlpanel.cpp
        test_enginehost.cpp
        test_enginehost_fx.cpp
        test_fileloader.cpp
        "${CMAKE_SOURCE_DIR}/plugin/src/EngineHost.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/FileLoader.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/Parameters.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/StateTree.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/src/state/Migrations.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/ControlPanel.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/Faceplate.cpp"
        "${CMAKE_SOURCE_DIR}/plugin/ui/LcdDisplay.cpp"
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
)

target_compile_definitions(mwstime_plugin_tests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=0
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
