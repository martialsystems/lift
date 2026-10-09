// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Drawing helpers shared by the panel painters: CSS-like gradients, shadows,
// insets and SVG-style text, in the prototype's CSS pixels.

#include "Fonts.h"
#include "PanelData.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <initializer_list>
#include <unordered_map>

namespace lift {
namespace draw {

using juce::AffineTransform;
using juce::Colour;
using juce::ColourGradient;
using juce::FillType;
using juce::Graphics;
using juce::Justification;
using juce::Path;
using juce::Rectangle;
using namespace ui;

inline constexpr float kPi = juce::MathConstants<float>::pi;

struct Stop {
    float p;
    Colour c;
};

inline FillType radial(float cx, float cy, float rx, float ry, std::initializer_list<Stop> stops) {
    ColourGradient gr;
    gr.isRadial = true;
    gr.point1 = {cx, cy};
    gr.point2 = {cx + rx, cy};
    for (const auto& s : stops) {
        gr.addColour(juce::jlimit(0.0, 1.0, static_cast<double>(s.p)), s.c);
    }
    FillType ft(gr);
    if (ry != rx) {
        ft.transform = AffineTransform::scale(1.f, ry / rx, cx, cy);
    }
    return ft;
}

inline FillType linear(float x1, float y1, float x2, float y2, std::initializer_list<Stop> stops) {
    ColourGradient gr;
    gr.isRadial = false;
    gr.point1 = {x1, y1};
    gr.point2 = {x2, y2};
    for (const auto& s : stops) {
        gr.addColour(juce::jlimit(0.0, 1.0, static_cast<double>(s.p)), s.c);
    }
    return FillType(gr);
}

inline Colour W(float a) {
    return Colour::fromFloatRGBA(1.f, 1.f, 1.f, a);
}
inline Colour B(float a) {
    return Colour::fromFloatRGBA(0.f, 0.f, 0.f, a);
}
inline Colour rgba(int r, int g, int b, float a) {
    return Colour(static_cast<juce::uint8>(r), static_cast<juce::uint8>(g), static_cast<juce::uint8>(b), a);
}

inline Path rrect(Rectangle<float> r, float rad) {
    Path p;
    p.addRoundedRectangle(r, rad);
    return p;
}

inline Path circle(float cx, float cy, float r) {
    Path p;
    p.addEllipse(cx - r, cy - r, 2.f * r, 2.f * r);
    return p;
}

// CSS box-shadow (outer). Blur is the CSS blur radius.
inline void shadow(Graphics& g, const Path& p, Colour c, float dx, float dy, float blur) {
    if (blur < 0.75f) {
        g.setColour(c);
        g.fillPath(p, AffineTransform::translation(dx, dy));
        return;
    }
    Path q(p);
    q.applyTransform(AffineTransform::translation(dx, dy));
    juce::DropShadow(c, juce::jmax(1, juce::roundToInt(blur)), {}).drawForPath(g, q);
}

// CSS box-shadow inset.
inline void inset(Graphics& g, const Path& shape, Colour c, float dx, float dy, float blur) {
    Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(shape);
    Path inv;
    inv.addRectangle(shape.getBounds().expanded(blur * 2.f + std::abs(dx) + std::abs(dy) + 4.f));
    Path moved(shape);
    moved.applyTransform(AffineTransform::translation(dx, dy));
    inv.addPath(moved);
    inv.setUsingNonZeroWinding(false);
    if (blur < 0.75f) {
        g.setColour(c);
        g.fillPath(inv);
    } else {
        juce::DropShadow(c, juce::roundToInt(blur), {}).drawForPath(g, inv);
    }
}

inline void fill(Graphics& g, const Path& p, const FillType& f) {
    g.setFillType(f);
    g.fillPath(p);
}

inline void fill(Graphics& g, const Path& p, Colour c) {
    g.setColour(c);
    g.fillPath(p);
}

inline void text(Graphics& g, const juce::String& s, const juce::Font& f, Colour c, Rectangle<float> r, Justification j) {
    g.setFont(f);
    g.setColour(c);
    g.drawText(s, r, j, false);
}

// SVG <text>: baseline at y; anchor -1 start, 0 middle, 1 end.
// Shaped runs are memoised (message thread only): the screen redraws the same
// labels every frame and shaping dominates otherwise.
struct ShapedRun {
    juce::GlyphArrangement ga;
    float w = 0.f;
};
inline const ShapedRun& shaped(const juce::String& s, float size, bool bold, float ls) {
    static std::unordered_map<juce::String, ShapedRun> cache;
    const juce::String key = s + "|" + juce::String(size) + (bold ? "b" : "r") + juce::String(ls);
    auto it = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 1024) {
            cache.clear();
        }
        ShapedRun r;
        const juce::Font f = mono(bold, size, ls / size);
        r.ga.addLineOfText(f, s, 0.f, 0.f);
        r.w = cssWidth(bold ? 700 : 400, true, size, ls / size, s);
        it = cache.emplace(key, std::move(r)).first;
    }
    return it->second;
}

inline void svgText(Graphics& g, const juce::String& s, float x, float y, float size, Colour c, int anchor = -1,
                    bool bold = false, float ls = 0.f) {
    const ShapedRun& r = shaped(s, size, bold, ls);
    float x0 = x;
    if (anchor == 0) {
        x0 = x - r.w * 0.5f;
    } else if (anchor > 0) {
        x0 = x - r.w;
    }
    g.setColour(c);
    r.ga.draw(g, AffineTransform::translation(x0, y));
}

inline void line(Graphics& g, float x1, float y1, float x2, float y2, Colour c, float w) {
    g.setColour(c);
    g.drawLine(x1, y1, x2, y2, w);
}

inline Colour grayed(Colour c) {
    // CSS filter: grayscale(1) brightness(.8)
    const float l = 0.2126f * c.getFloatRed() + 0.7152f * c.getFloatGreen() + 0.0722f * c.getFloatBlue();
    const float v = juce::jlimit(0.f, 1.f, l * 0.8f);
    return Colour::fromFloatRGBA(v, v, v, c.getFloatAlpha());
}

// conic-gradient(from 15deg, ...) chrome nut, cached per scale
inline const juce::Image& nutImage() {
    static juce::Image img = [] {
        const int S = 96;  // 22 px nut at ~4.4x
        juce::Image im(juce::Image::ARGB, S, S, true);
        Graphics g(im);
        const juce::uint32 st[9] = {0xf5f5f3, 0x8d8d8a, 0xe9e9e6, 0x6c6c69, 0xfbfbf9,
                                    0x9d9d99, 0xdcdcd8, 0x777774, 0xf5f5f3};
        const float c = S * 0.5f;
        const int N = 180;
        for (int k = 0; k < N; ++k) {
            const float a0 = static_cast<float>(k) / N * 2.f * kPi;
            const float a1 = static_cast<float>(k + 1) / N * 2.f * kPi + 0.01f;
            const float deg = (static_cast<float>(k) + 0.5f) / N * 360.f;
            float t = std::fmod(deg - 15.f + 360.f, 360.f) / 45.f;
            const int i0 = juce::jmin(7, static_cast<int>(t));
            const float f = t - static_cast<float>(i0);
            const Colour col = hex(st[i0]).interpolatedWith(hex(st[i0 + 1]), f);
            Path w;
            w.startNewSubPath(c, c);
            w.addCentredArc(c, c, c, c, 0.f, a0, a1, false);
            w.closeSubPath();
            g.setColour(col);
            g.fillPath(w);
        }
        return im;
    }();
    return img;
}


}  // namespace draw
}  // namespace lift
