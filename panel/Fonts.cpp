// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "Fonts.h"

#include "LiftFontData.h"

namespace lift::ui {

namespace {

juce::Typeface::Ptr load(const char* data, int size) {
    return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
}

struct Faces {
    juce::Typeface::Ptr jost[4];
    juce::Typeface::Ptr mono[2];
    Faces() {
        jost[0] = load(LiftFontData::JostRegular_ttf, LiftFontData::JostRegular_ttfSize);
        jost[1] = load(LiftFontData::JostMedium_ttf, LiftFontData::JostMedium_ttfSize);
        jost[2] = load(LiftFontData::JostSemiBold_ttf, LiftFontData::JostSemiBold_ttfSize);
        jost[3] = load(LiftFontData::JostBold_ttf, LiftFontData::JostBold_ttfSize);
        mono[0] = load(LiftFontData::SpaceMonoRegular_ttf, LiftFontData::SpaceMonoRegular_ttfSize);
        mono[1] = load(LiftFontData::SpaceMonoBold_ttf, LiftFontData::SpaceMonoBold_ttfSize);
    }
};

Faces& faces() {
    static Faces f;
    return f;
}

juce::Font make(juce::Typeface::Ptr tf, float px, float lsEm) {
    juce::Font f(juce::FontOptions(tf).withPointHeight(px));
    if (lsEm != 0.f) {
        f = juce::Font(juce::FontOptions(tf).withPointHeight(px).withKerningFactor(lsEm * px / f.getHeight()));
    }
    return f;
}

int slot(int weight) {
    return weight >= 700 ? 3 : weight >= 600 ? 2 : weight >= 500 ? 1 : 0;
}

}  // namespace

juce::Font jost(int weight, float px, float lsEm) {
    return make(faces().jost[slot(weight)], px, lsEm);
}

juce::Font mono(bool bold, float px, float lsEm) {
    return make(faces().mono[bold ? 1 : 0], px, lsEm);
}

float cssWidth(int weight, bool isMono, float px, float lsEm, const juce::String& s) {
    const juce::Font f = isMono ? mono(weight >= 700, px) : jost(weight, px);
    return juce::GlyphArrangement::getStringWidth(f, s) + static_cast<float>(s.length()) * lsEm * px;
}

std::array<juce::Rectangle<float>, 9> topBarRects() {
    std::array<juce::Rectangle<float>, 9> r;
    float x = 16.f + cssWidth(600, false, 12.f, 0.14f, "CABLE COLOR") + 8.f;
    for (int k = 0; k < 6; ++k) {
        r[static_cast<size_t>(k)] = {x, 14.f, 26.f, 26.f};
        x += 34.f;
    }
    x += 18.f - 8.f;
    const char* names[3] = {"STACK", "CLEAR CABLES", "RESET KNOBS"};
    for (int k = 0; k < 3; ++k) {
        const float w = cssWidth(600, false, 12.f, 0.12f, names[k]) + 24.f + 2.f;
        r[static_cast<size_t>(6 + k)] = {x, 14.f, w, 26.f};
        x += w + 18.f;
    }
    return r;
}

}  // namespace lift::ui
