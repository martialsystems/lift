# Copyright (c) 2026 Martial Systems LLC. All rights reserved.

cmake_minimum_required(VERSION 3.22)
project(LIFT LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lift_core STATIC
    src/names/catalog.cpp
    src/project/project.cpp
    src/tape/character.cpp
    src/tape/model.cpp
    src/tape/transport.cpp
    src/tape/edit.cpp
    src/audio/prepare.cpp
    src/audio/process_block.cpp
)
target_include_directories(lift_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(lift_core PRIVATE -Wall -Wextra -Werror)

add_library(lift_radio STATIC src/radio/client.cpp)
target_include_directories(lift_radio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lift_radio PUBLIC lift_core)
target_compile_options(lift_radio PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lift_radio PUBLIC LIFT_RADIO=1)

# Standalone links the radio client. The window target comes with JUCE.
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

add_executable(lift_tests tests/test_main.cpp)
target_link_libraries(lift_tests PRIVATE lift_core)
target_compile_options(lift_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lift_tests PRIVATE LIFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
