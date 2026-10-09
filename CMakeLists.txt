# Copyright (c) 2026 Martial Systems LLC. All rights reserved.

cmake_minimum_required(VERSION 3.22)
project(LIFT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

find_package(Threads REQUIRED)

# JUCE (DSP modules only). Pinned to a JUCE 8 tag. Used under the JUCE 8
# licence; keep the plan's revenue tier note in mind before shipping.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(juce)

# JUCE module sources compile once here, without the project's -Werror.
add_library(lift_juce STATIC)
target_link_libraries(lift_juce PRIVATE juce::juce_audio_basics juce::juce_dsp)
target_include_directories(lift_juce SYSTEM PUBLIC ${juce_SOURCE_DIR}/modules)
target_compile_definitions(lift_juce PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODULE_AVAILABLE_juce_core=1
    JUCE_MODULE_AVAILABLE_juce_audio_basics=1
    JUCE_MODULE_AVAILABLE_juce_audio_formats=1
    JUCE_MODULE_AVAILABLE_juce_dsp=1
)
set_target_properties(lift_juce PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(LIFT_DSP_WARN /W4 /WX)
else()
    set(LIFT_DSP_WARN -Wall -Wextra -Werror)
endif()

# Tape emulation core. Platform free (Linux, Windows, macOS): no UI, no device
# code, no codec. The app, the panel and a future plugin wrapper link this.
add_library(lift_tape_dsp STATIC
    src/tape/character.cpp
    src/tape/engine.cpp
    src/tape/engine_prepare.cpp
)
target_include_directories(lift_tape_dsp PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lift_tape_dsp PUBLIC lift_juce)
target_compile_options(lift_tape_dsp PRIVATE ${LIFT_DSP_WARN})
find_path(OPUS_INCLUDE_DIR opus/opus.h HINTS /opt/homebrew/include /usr/local/include)
find_library(OPUS_LIBRARY opus HINTS /opt/homebrew/lib /usr/local/lib)
if(NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY)
    message(FATAL_ERROR "libopus was not found")
endif()

add_library(lift_vendor STATIC src/codec/vendor.cpp)
target_include_directories(lift_vendor PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(lift_vendor PRIVATE -w)

add_library(lift_vorbis STATIC third_party/stb_vorbis.c)
target_include_directories(lift_vorbis PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(lift_vorbis PRIVATE STB_VORBIS_IMPLEMENTATION)
target_compile_options(lift_vorbis PRIVATE -w)

add_library(lift_core STATIC
    src/names/catalog.cpp
    src/project/project.cpp
    src/tape/model.cpp
    src/tape/transport.cpp
    src/tape/edit.cpp
    src/audio/prepare.cpp
    src/audio/process_block.cpp
    src/codec/codec.cpp
    src/pool/pool.cpp
)
target_include_directories(lift_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(lift_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(lift_core PUBLIC lift_vendor lift_tape_dsp)
target_compile_options(lift_core PRIVATE -Wall -Wextra -Werror)

add_library(lift_radio STATIC
    src/radio/client.cpp
    src/radio/stream.cpp
)
target_include_directories(lift_radio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(lift_radio PRIVATE ${CMAKE_SOURCE_DIR}/third_party ${OPUS_INCLUDE_DIR})
target_link_libraries(lift_radio PUBLIC lift_core lift_vorbis ${OPUS_LIBRARY} Threads::Threads)
target_compile_options(lift_radio PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lift_radio PUBLIC LIFT_RADIO=1)

# Standalone links the radio client. The panel window is AppKit.
# The tape runtime stays the class a later wrapper can call.
add_library(lift_standalone_core STATIC src/app/standalone_entry.cpp)
target_include_directories(lift_standalone_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lift_standalone_core PUBLIC lift_core lift_radio)
target_compile_options(lift_standalone_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lift_standalone_core PUBLIC LIFT_TARGET_VST3=0 LIFT_RADIO=1)

# VST3 build has no radio. This target is the core a plugin wrapper links.
add_library(lift_vst3_core STATIC src/plugin/vst3_entry.cpp)
target_include_directories(lift_vst3_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lift_vst3_core PUBLIC lift_core)
target_compile_options(lift_vst3_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lift_vst3_core PUBLIC LIFT_TARGET_VST3=1 LIFT_RADIO=0)

add_executable(lift_tests tests/test_main.cpp tests/test_pool.cpp tests/test_window.cpp)
target_compile_options(lift_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lift_tests PRIVATE LIFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Offline renders through the tape engine with checks in code. Writes WAVs.
add_executable(lift_tape_check tests/tape_check.cpp)
target_link_libraries(lift_tape_check PRIVATE lift_core)
target_compile_options(lift_tape_check PRIVATE ${LIFT_DSP_WARN})
enable_testing()
add_test(NAME lift_tape_check COMMAND lift_tape_check)
add_test(NAME lift_tests COMMAND lift_tests)

add_library(lift_panel STATIC
    src/ui/audio.cpp
    src/ui/draw.cpp
    src/ui/panel.cpp
)
target_include_directories(lift_panel PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lift_panel PUBLIC lift_core)
target_compile_options(lift_panel PRIVATE -Wall -Wextra -Werror)

target_link_libraries(lift_tests PRIVATE lift_radio lift_panel)

if(APPLE)
    add_executable(lift_window src/app/window_mac.mm)
    target_include_directories(lift_window PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(lift_window PRIVATE lift_panel)
    target_link_libraries(lift_window PRIVATE
        "-framework Cocoa"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework AudioUnit"
    )
    target_compile_options(lift_window PRIVATE -fobjc-arc -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# JUCE app: the LIFT panel (native JUCE drawing) as Standalone and VST3 (AU on
# macOS). These targets compile JUCE's GUI/audio modules themselves, so they
# take the tape sources directly instead of linking lift_core/lift_juce (which
# would compile JUCE twice).
option(LIFT_BUILD_APP "Build the JUCE panel app and plugin" ON)
if(LIFT_BUILD_APP)
    set(LIFT_TAPE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/tape/character.cpp
        ${CMAKE_SOURCE_DIR}/src/tape/model.cpp
        ${CMAKE_SOURCE_DIR}/src/tape/transport.cpp
        ${CMAKE_SOURCE_DIR}/src/tape/edit.cpp
        ${CMAKE_SOURCE_DIR}/src/tape/engine.cpp
        ${CMAKE_SOURCE_DIR}/src/tape/engine_prepare.cpp
        ${CMAKE_SOURCE_DIR}/src/audio/prepare.cpp
        ${CMAKE_SOURCE_DIR}/src/audio/process_block.cpp
    )
    set(LIFT_PANEL_SOURCES
        ${CMAKE_SOURCE_DIR}/panel/Fonts.cpp
        ${CMAKE_SOURCE_DIR}/panel/LiftPanel.cpp
        ${CMAKE_SOURCE_DIR}/panel/LiftPanelPaint.cpp
        ${CMAKE_SOURCE_DIR}/panel/LiftPanelScreen.cpp
        ${CMAKE_SOURCE_DIR}/panel/LiftProcessor.cpp
    )
    juce_add_binary_data(lift_fonts
        HEADER_NAME LiftFontData.h
        NAMESPACE LiftFontData
        SOURCES
            assets/fonts/Jost-Regular.ttf
            assets/fonts/Jost-Medium.ttf
            assets/fonts/Jost-SemiBold.ttf
            assets/fonts/Jost-Bold.ttf
            assets/fonts/SpaceMono-Regular.ttf
            assets/fonts/SpaceMono-Bold.ttf
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
        PLUGIN_MANUFACTURER_CODE Mrtl
        PLUGIN_CODE Lft1
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        FORMATS ${LIFT_APP_FORMATS}
    )
    target_sources(LiftApp PRIVATE ${LIFT_TAPE_SOURCES} ${LIFT_PANEL_SOURCES})
    target_include_directories(LiftApp PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/panel)
    target_compile_definitions(LiftApp PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(LiftApp PRIVATE lift_fonts juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Headless check: renders the panel screens to PNG and drives the panel
    # actions (REC+PLAY with a note, STOP, LIFT, DROP) against the engine.
    juce_add_console_app(lift_panel_check VERSION 0.1.0 PRODUCT_NAME "lift_panel_check")
    target_sources(lift_panel_check PRIVATE tests/panel_check.cpp ${LIFT_TAPE_SOURCES} ${LIFT_PANEL_SOURCES})
    target_include_directories(lift_panel_check PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/panel)
    target_compile_definitions(lift_panel_check PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(lift_panel_check PRIVATE lift_fonts juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
    add_test(NAME lift_panel_check COMMAND lift_panel_check)
endif()

add_custom_target(lift_laws ALL
    COMMAND ${CMAKE_COMMAND} -E env LIFT_NO_REBUILD=1 python3 ${CMAKE_SOURCE_DIR}/tests/test_fences.py
    DEPENDS lift_tests lift_vst3_core lift_radio lift_core lift_standalone_core
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Five fences"
    VERBATIM
)
