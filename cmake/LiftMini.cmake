# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#
# LIFT Mini: a separate standalone (plain JUCE GUI app, no plug-in wrapper)
# with the Mini's 480 x 272 screen. Built for Raspberry Pi 4/5 (Linux
# aarch64), also builds on Linux x86_64, macOS and Windows for desk work.
# Uses LIFT_TAPE_SOURCES / LIFT_ENGINE_SOURCES / LIFT_PANEL_SOURCES from LiftApp.cmake.
juce_add_gui_app(LIFT_Mini
    VERSION 0.1.0
    COMPANY_NAME "Martial Systems"
    PRODUCT_NAME "lift-mini"
    BUNDLE_ID com.martialsystems.liftmini
    NEEDS_CURL TRUE
    PLIST_TO_MERGE "${LIFT_PLIST_MERGE}")
target_sources(LIFT_Mini PRIVATE
    ${LIFT_ROOT}/mini/MiniApp.cpp ${LIFT_ROOT}/mini/MiniUi.cpp ${LIFT_ROOT}/mini/MiniControls.cpp
    ${LIFT_TAPE_SOURCES} ${LIFT_ENGINE_SOURCES} ${LIFT_PANEL_SOURCES})
target_include_directories(LIFT_Mini PRIVATE ${LIFT_ROOT}/src ${LIFT_ROOT}/panel ${LIFT_ROOT}/mini ${LIFT_ENGINE_INCLUDES})
target_compile_definitions(LIFT_Mini PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=1 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_STANDALONE_APPLICATION=1
    JUCE_ALSA=1 JUCE_JACK=1 JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)
if(APPLE)
    target_link_libraries(LIFT_Mini PRIVATE "-framework CoreAudio" "-framework Foundation")
endif()
target_link_libraries(LIFT_Mini PRIVATE lift_fonts lift_art juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)

# Mini checks + callback bench: ctest -R lift_mini_check
juce_add_console_app(lift_mini_check VERSION 0.1.0 PRODUCT_NAME "lift_mini_check" NEEDS_CURL TRUE)
target_sources(lift_mini_check PRIVATE ${LIFT_ROOT}/tests/mini_check.cpp
    ${LIFT_TAPE_SOURCES} ${LIFT_ENGINE_SOURCES} ${LIFT_PANEL_SOURCES})
target_include_directories(lift_mini_check PRIVATE ${LIFT_ROOT}/src ${LIFT_ROOT}/panel ${LIFT_ROOT}/mini ${LIFT_ROOT}/tests ${LIFT_ENGINE_INCLUDES})
target_compile_definitions(lift_mini_check PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=1 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(lift_mini_check PRIVATE lift_fonts lift_art juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)
add_test(NAME lift_mini_check COMMAND lift_mini_check)
if(APPLE)
    target_link_libraries(lift_mini_check PRIVATE "-framework CoreAudio" "-framework Foundation")
endif()

# Mini scenario book: plays the Mini UI (SHIFT, buttons, keys, encoders, Pico
# MIDI) against the real processor. tests/mini_recipes/README.md explains it.
#   ctest -R lift_mini_session      or      lift_mini_session --only "NAME"
juce_add_console_app(lift_mini_session VERSION 0.1.0 PRODUCT_NAME "lift_mini_session" NEEDS_CURL TRUE)
target_sources(lift_mini_session PRIVATE ${LIFT_ROOT}/tests/mini_session.cpp
    ${LIFT_ROOT}/mini/MiniUi.cpp ${LIFT_ROOT}/mini/MiniControls.cpp
    ${LIFT_TAPE_SOURCES} ${LIFT_ENGINE_SOURCES} ${LIFT_PANEL_SOURCES})
target_include_directories(lift_mini_session PRIVATE ${LIFT_ROOT}/src ${LIFT_ROOT}/panel ${LIFT_ROOT}/mini ${LIFT_ROOT}/tests ${LIFT_ENGINE_INCLUDES})
target_compile_definitions(lift_mini_session PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=1 JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_STANDALONE_APPLICATION=1 LIFT_SOURCE_DIR="${LIFT_ROOT}")
target_link_libraries(lift_mini_session PRIVATE lift_fonts lift_art juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)
add_test(NAME lift_mini_session COMMAND lift_mini_session ${LIFT_ROOT}/tests/mini_recipes/book.txt)
if(APPLE)
    target_link_libraries(lift_mini_session PRIVATE "-framework CoreAudio" "-framework Foundation")
endif()
