# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#
# The JUCE app: the LIFT panel (native JUCE drawing) as Standalone and VST3
# (AU on macOS), plus the headless panel check. These targets compile JUCE's
# GUI/audio modules themselves, so they take the tape sources directly
# instead of linking lift_core/lift_juce (which would compile JUCE twice).

set(LIFT_TAPE_SOURCES
    ${LIFT_ROOT}/src/tape/character.cpp
    ${LIFT_ROOT}/src/tape/model.cpp
    ${LIFT_ROOT}/src/tape/transport.cpp
    ${LIFT_ROOT}/src/tape/edit.cpp
    ${LIFT_ROOT}/src/tape/engine.cpp
    ${LIFT_ROOT}/src/tape/engine_prepare.cpp
    ${LIFT_ROOT}/src/audio/prepare.cpp
    ${LIFT_ROOT}/src/audio/process_block.cpp
)
set(LIFT_PANEL_SOURCES
    ${LIFT_ROOT}/panel/Fonts.cpp
    ${LIFT_ROOT}/panel/LiftPanel.cpp
    ${LIFT_ROOT}/panel/LiftPanelPaint.cpp
    ${LIFT_ROOT}/panel/LiftPanelScreen.cpp
    ${LIFT_ROOT}/panel/LiftShift.cpp
    ${LIFT_ROOT}/panel/LiftProcessor.cpp
    ${LIFT_ROOT}/panel/LiftSlots.cpp
    ${LIFT_ROOT}/panel/SlotStore.cpp
    ${LIFT_ROOT}/panel/UiState.cpp
)
juce_add_binary_data(lift_fonts
    HEADER_NAME LiftFontData.h
    NAMESPACE LiftFontData
    SOURCES
        ${LIFT_ROOT}/assets/fonts/Jost-Regular.ttf
        ${LIFT_ROOT}/assets/fonts/Jost-Medium.ttf
        ${LIFT_ROOT}/assets/fonts/Jost-SemiBold.ttf
        ${LIFT_ROOT}/assets/fonts/Jost-Bold.ttf
        ${LIFT_ROOT}/assets/fonts/SpaceMono-Regular.ttf
        ${LIFT_ROOT}/assets/fonts/SpaceMono-Bold.ttf
)
set_target_properties(lift_fonts PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(LIFT_APP_FORMATS Standalone VST3)
if(APPLE)
    list(APPEND LIFT_APP_FORMATS AU)
endif()
juce_add_plugin(LiftApp
    VERSION 0.1.0
    COMPANY_NAME "Martial Systems"
    PRODUCT_NAME "LIFT"
    BUNDLE_ID com.martialsystems.lift
    PLUGIN_MANUFACTURER_CODE Mrtl
    PLUGIN_CODE Lft1
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS ${LIFT_APP_FORMATS}
)
target_sources(LiftApp PRIVATE ${LIFT_TAPE_SOURCES} ${LIFT_PANEL_SOURCES})
target_include_directories(LiftApp PRIVATE ${LIFT_ROOT}/src ${LIFT_ROOT}/panel)
target_compile_definitions(LiftApp PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(LiftApp PRIVATE lift_fonts juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Headless check: renders the panel screens to PNG and drives the panel
# actions (REC+PLAY with a note, STOP, LIFT, DROP) against the engine.
juce_add_console_app(lift_panel_check VERSION 0.1.0 PRODUCT_NAME "lift_panel_check")
target_sources(lift_panel_check PRIVATE ${LIFT_ROOT}/tests/panel_check.cpp ${LIFT_ROOT}/tests/panel_state.cpp ${LIFT_TAPE_SOURCES} ${LIFT_PANEL_SOURCES})
target_include_directories(lift_panel_check PRIVATE ${LIFT_ROOT}/src ${LIFT_ROOT}/panel)
target_compile_definitions(lift_panel_check PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(lift_panel_check PRIVATE lift_fonts juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)
add_test(NAME lift_panel_check COMMAND lift_panel_check)
