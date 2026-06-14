# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# Plugin shell (docs/design/architecture.md §3, §7; plan/decisions/002):
# one juce_add_plugin target exporting AU/VST3/LV2/Standalone, plus the CLAP
# via clap_juce_extensions_plugin.

# COPY_PLUGIN_AFTER_BUILD on macOS so the AU lands in
# ~/Library/Audio/Plug-Ins/Components and auval can find it.
if(APPLE)
    set(MWS_COPY_PLUGIN_AFTER_BUILD TRUE)
else()
    set(MWS_COPY_PLUGIN_AFTER_BUILD FALSE)
endif()

# Per-platform format set (architecture.md §3 ADR-002; tasks 049/050):
#   * macOS : AU + VST3 + LV2 + Standalone (CLAP added below for all platforms).
#   * Linux : VST3 + LV2 + Standalone (AU is macOS-only).
#   * Windows : VST3 + Standalone (AU is macOS-only; LV2 is OPTIONAL on Windows —
#     the Windows v1 set is VST3 + CLAP + Standalone per task 050, so LV2 is
#     dropped there rather than shipped unvalidated. JUCE's native LV2 exporter is
#     primarily exercised on Linux; there is no Windows LV2 validator wired here,
#     so building it would produce an unchecked artifact — see the PR for the
#     decision record).
# CLAP is appended by clap_juce_extensions_plugin(...) below on every platform.
if(APPLE)
    set(MWS_PLUGIN_FORMATS AU VST3 LV2 Standalone)
elseif(WIN32)
    set(MWS_PLUGIN_FORMATS VST3 Standalone)
else()  # Linux + other UNIX
    set(MWS_PLUGIN_FORMATS VST3 LV2 Standalone)
endif()

juce_add_plugin(mwStime
    PRODUCT_NAME                "mwStime"
    COMPANY_NAME                mattWoolly
    BUNDLE_ID                   com.mattwoolly.mwstime
    PLUGIN_MANUFACTURER_CODE    MwSt
    PLUGIN_CODE                 MwS1
    FORMATS                     ${MWS_PLUGIN_FORMATS}
    # Category minefield (architecture.md §7, ADR-002): IS_SYNTH FALSE +
    # NEEDS_MIDI_INPUT TRUE ⇒ the AU exports as `aumf` (music effect) so
    # Logic routes MIDI to it. auval must be invoked as:
    #   auval -v aumf MwS1 MwSt
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    LV2URI                      "https://github.com/mattWoolly/mwStime"
    COPY_PLUGIN_AFTER_BUILD     ${MWS_COPY_PLUGIN_AFTER_BUILD}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "mwStime Standalone needs audio input to pass audio through."
)

target_sources(mwStime
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/EngineHost.cpp
        src/ExportService.cpp
        src/FileLoader.cpp
        src/MidiVoice.cpp
        src/state/Parameters.cpp
        src/state/StateTree.cpp
        src/state/StateBlobCache.cpp
        src/state/Migrations.cpp
        src/state/FactoryPresets.cpp
        ui/ControlPanel.cpp
        ui/EditorActions.cpp
        ui/Faceplate.cpp
        ui/LcdDisplay.cpp
        ui/LcdFieldEditor.cpp
        ui/LcdPageBinding.cpp
        ui/LcdPageModel.cpp
        ui/JogWheel.cpp
        ui/ModelSelector.cpp
        ui/SoftKeyBar.cpp
        ui/WaveformView.cpp
        ui/lookandfeel/SeriesLookAndFeel.cpp
)

target_include_directories(mwStime PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(mwStime
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(mwStime
    PRIVATE
        mwstime_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# CLAP export via the pinned clap-juce-extensions shim
# (cmake/FetchClapExtensions.cmake — tag bumps are explicit backlog tasks).
clap_juce_extensions_plugin(
    TARGET          mwStime
    CLAP_ID         "com.mattwoolly.mwstime"
    CLAP_FEATURES   audio-effect stereo
)
