// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// The Mini screen's layout as data. Every rectangle and font size the screen
// draws comes from here, so the native 800 x 480 panel is one change:
// set kScreen = kScreen800 (or build with -DLIFT_MINI_SCREEN=800).
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace mini {

struct ScreenLayout {
    int w, h;                       // must match kScreenW / kScreenH below                       // logical pixels (scaled to the window)
    juce::Rectangle<float> status;  // top bar: mode, tempo, octave, transport
    juce::Rectangle<float> body;    // the mode view
    juce::Rectangle<float> toast;   // message strip (over the bottom of body)
    juce::Rectangle<float> foot;    // four knob cells
    float font;                     // base font px (labels)
    float big;                      // values
    float stroke;                   // line width
    float pad;                      // inner padding
};

inline const ScreenLayout kScreen480{480, 272, {10, 4, 460, 18}, {8, 26, 464, 186}, {0, 190, 480, 22},
                                         {0, 214, 480, 58}, 12.f, 14.f, 1.f, 6.f};
inline const ScreenLayout kScreen800{800, 480, {16, 6, 768, 30}, {12, 44, 776, 328}, {0, 336, 800, 36},
                                         {0, 376, 800, 104}, 20.f, 24.f, 1.6f, 10.f};

#if defined(LIFT_MINI_SCREEN) && LIFT_MINI_SCREEN == 800
inline const ScreenLayout& kScreen = kScreen800;
inline constexpr int kScreenW = 800, kScreenH = 480;
#else
inline constexpr int kScreenW = 480, kScreenH = 272;
inline const ScreenLayout& kScreen = kScreen480;   // the spec's 4.3 in, 480 x 272
#endif

}  // namespace mini
