# Copyright (c) 2026 Martial Systems LLC. All rights reserved.

# Instrument DSP core: patch router, synth engines, drums (SHOGUN voices), FX.
# No JUCE, no GUI, no allocation on the audio path: the hardware-ready part.
add_library(lift_engine STATIC
    ${LIFT_ROOT}/src/engine/patch.cpp
    ${LIFT_ROOT}/src/engine/synth.cpp
    ${LIFT_ROOT}/src/engine/drums.cpp
    ${LIFT_ROOT}/src/engine/fx.cpp
    ${LIFT_ROOT}/src/engine/instrument.cpp
    ${LIFT_ROOT}/src/engine/resample.cpp
    ${LIFT_ROOT}/src/engine/clipvoice.cpp
    ${LIFT_ROOT}/src/engine/grain.cpp
    ${LIFT_ROOT}/third_party/jidai/shogun/shogun.cpp
)
target_include_directories(lift_engine PUBLIC ${LIFT_ROOT}/src)
target_include_directories(lift_engine PUBLIC ${LIFT_ROOT}/third_party/jidai/shogun
    ${LIFT_ROOT}/third_party/jidai/jidai-common/include)

# Offline engine checks: patch connections, feedback stability, drum decays,
# sequencer timing, synth aliasing, every effect, CPU per voice / engine / effect.
add_executable(lift_engine_check ${LIFT_ROOT}/tests/engine_check.cpp)
target_link_libraries(lift_engine_check PRIVATE lift_engine)
add_test(NAME lift_engine_check COMMAND lift_engine_check)

# Resampling core: capture ring, trim stage (loudness), selections, limiter,
# clip voices, keep / process round trips.
add_executable(lift_resample_check ${LIFT_ROOT}/tests/resample_check.cpp)
target_link_libraries(lift_resample_check PRIVATE lift_engine)
add_test(NAME lift_resample_check COMMAND lift_resample_check)


# Tuning helper (not shipped): one drum hit to a WAV for tools/drum_ref_analysis.py
add_executable(lift_drum_render ${LIFT_ROOT}/tools/drum_render.cpp)
target_link_libraries(lift_drum_render PRIVATE lift_engine)
