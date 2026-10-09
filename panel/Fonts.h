// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>

namespace lift::ui {

// Jost and Space Mono, embedded (SIL OFL 1.1, see assets/fonts). Sizes are CSS
// pixels (em size); letter spacing is in em, as in the prototype's CSS.
juce::Font jost(int weight, float px, float lsEm = 0.f);
juce::Font mono(bool bold, float px, float lsEm = 0.f);

// CSS layout width: glyph advances plus letter spacing after every glyph.
float cssWidth(int weight, bool isMono, float px, float lsEm, const juce::String& s);

// Top control bar (page pixels): 6 swatches, Stack, Clear cables, Reset knobs.
std::array<juce::Rectangle<float>, 9> topBarRects();

}  // namespace lift::ui
