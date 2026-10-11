// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Native drawing of the LIFT panel. Each block cites the prototype CSS/SVG it
// reproduces; numbers are the prototype's CSS pixels.

#include "LiftPanel.h"

#include "Fonts.h"
#include "PanelData.h"
#include "PaintUtil.h"
#include "PanelLayers.h"

#include <cmath>

namespace lift {

using namespace ui;
using juce::AffineTransform;
using juce::Colour;
using juce::ColourGradient;
using juce::FillType;
using juce::Graphics;
using juce::Justification;
using juce::Path;
using juce::Rectangle;

using namespace draw;


// ------------------------------------------------------------------ grain

void LiftPanel::buildGrain() {
    // feTurbulence fractalNoise (0.95, 2 octaves) through the colour matrix,
    // multiplied at 55 % opacity: a fine dark speckle on the beige case.
    constexpr int GW = 1328, GH = 994;
    grain_ = juce::Image(juce::Image::ARGB, GW, GH, true);
    juce::Image::BitmapData bd(grain_, juce::Image::BitmapData::writeOnly);
    juce::Random rnd(4);
    std::vector<float> a(static_cast<size_t>(GW * (GH + 1)), 0.f);
    for (auto& v : a) {
        v = rnd.nextFloat();
    }
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            const size_t i = static_cast<size_t>(y * GW + x);
            const float n1 = a[i];
            const float n2 = 0.5f * (a[i] + a[x + 1 < GW ? i + 1 : i]) * 0.5f + 0.5f * a[i + GW] * 0.5f;
            const float n = 0.5f + (n1 - 0.5f) * 0.36f + (n2 - 0.25f) * 0.18f;
            const float alpha = juce::jlimit(0.f, 1.f, 0.55f * n - 0.18f) * 0.55f;
            bd.setPixelColour(x, y, rgba(97, 80, 51, alpha));
        }
    }
}

// ------------------------------------------------------------------ paint

void LiftPanel::paintTopBar(Graphics& g) {
    // v3.1: no colour swatches: a cable takes the colour of its source signal
    const Colour fg = hex(0x24221e);
    text(g, "CABLES", jost(600, 12.f, 0.14f), fg, {16.f, 12.f, 200.f, 26.f}, Justification::centredLeft);
    text(g, "take the color of their source signal", jost(400, 12.f), hex(0x57524a),
         {16.f + cssWidth(600, false, 12.f, 0.14f, "CABLES") + 10.f, 12.f, 260.f, 26.f}, Justification::centredLeft);
    const auto r = topBarRects();
    const char* names[3] = {"STACK", "CLEAR CABLES", "RESET KNOBS"};
    for (int k = 0; k < 3; ++k) {
        const auto b = r[static_cast<size_t>(6 + k)];
        const bool on = k == 0 && stack_;
        fill(g, rrect(b, 6.f), on ? hex(BLU) : hex(0xefe9dc));
        g.setColour(on ? hex(BLU) : hex(0x9d968a));
        g.drawRoundedRectangle(b.reduced(0.5f), 5.5f, 1.f);
        text(g, names[k], jost(600, 12.f, 0.12f), on ? juce::Colours::white : fg, b.translated(0.12f * 6.f, 0.f),
             Justification::centred);
    }
    text(g, info_, jost(400, 13.f), hex(0x57524a), {16.f, 44.f, 1330.f, 19.5f}, Justification::centredLeft);
}

// The type symbols: every shape fills the same 10 x 10 box (one top line,
// one baseline). (x, y) = the box's top-left, z = its size.
void drawSym(Graphics& g, char t, float x, float y, float z, Colour col) {
    const float k = z / 10.f;
    Path p;
    switch (t) {
    case 'p': p.addTriangle(0.6f, 9.4f, 5.f, 0.6f, 9.4f, 9.4f); break;
    case 'g': p.addRectangle(1.f, 1.f, 8.f, 8.f); break;
    case 'm': p.addEllipse(0.4f, 0.4f, 9.2f, 9.2f); break;
    case 'a':
        p.startNewSubPath(5.f, 0.4f);
        p.lineTo(9.6f, 5.f);
        p.lineTo(5.f, 9.6f);
        p.lineTo(0.4f, 5.f);
        p.closeSubPath();
        break;
    default: {
        Path d;
        d.startNewSubPath(5.f, 1.3f);
        d.lineTo(8.7f, 5.f);
        d.lineTo(5.f, 8.7f);
        d.lineTo(1.3f, 5.f);
        d.closeSubPath();
        g.setColour(col);
        g.strokePath(d, juce::PathStrokeType(1.7f), AffineTransform::scale(k).translated(x, y));
        return;
    }
    }
    g.setColour(col);
    g.fillPath(p, AffineTransform::scale(k).translated(x, y));
}

void LiftPanel::paintCase(Graphics& g) {
    const Rectangle<float> R(0.f, 0.f, kDevW, kDevH);
    const Path rr = rrect(R, 16.f);
    shadow(g, rr, rgba(30, 24, 12, 0.3f), 0.f, 16.f, 24.f);
    shadow(g, rr, rgba(30, 24, 12, 0.45f), 0.f, 44.f, 64.f);
    shadow(g, rr, B(0.3f), 0.f, 9.f, 2.f);
    fill(g, rrect(R.translated(0.f, 8.f), 16.f), hex(0xa59572));
    fill(g, rrect(R.translated(0.f, 5.f), 16.f), hex(0xc3b38f));
    fill(g, rrect(R.expanded(1.f), 17.f), hex(0xcbbd9d));
    fill(g, rr, hex(0xe8ddc3));
    fill(g, rr,
         linear(0.f, 0.f, 0.f, kDevH,
                {{0.f, W(0.3f)}, {0.26f, W(0.f)}, {0.74f, B(0.f)}, {1.f, rgba(70, 50, 15, 0.07f)}}));
    inset(g, rr, hex(0xfff8e6), 0.f, 1.f, 0.f);
    inset(g, rr, W(0.5f), 1.f, 0.f, 0.f);
    inset(g, rr, hex(0xd6c8a8), 0.f, -3.f, 0.f);
    {
        Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(rr);
        g.drawImageAt(grain_, 0, 0);
    }
}

void LiftPanel::paintBayStatic(Graphics& g, bool recess) {
    if (recess) {
        // the black patch panel (normally in the case art)
        const Path bay = rrect({kBayX, kBayY, kBayW, kBayH}, 9.f);
        fill(g, rrect({kBayX - 1.f, kBayY - 1.f, kBayW + 2.f, kBayH + 2.f}, 10.f), hex(0xb9ab8d));
        fill(g, bay, linear(0.f, kBayY, 0.f, kBayY + kBayH, {{0.f, hex(0x0c0c0c)}, {0.4f, hex(0x151515)}, {1.f, hex(0x121212)}}));
        inset(g, bay, B(0.85f), 0.f, 3.f, 7.f);
    }
    const Colour lab = hex(0xd9d2c2), dim = hex(0x8c877b);
    // --- matrix: SRC/DST key, column heads, row labels, symbols, grid
    text(g, juce::String::fromUTF8("SRC \xe2\x86\x93"), jost(700, 8.f, 0.16f), dim, {kBayX, 31.f, 82.f, 10.f}, Justification::centredRight);
    text(g, juce::String::fromUTF8("DST \xe2\x86\x92"), jost(700, 8.f, 0.16f), dim, {kBayX, 43.f, 82.f, 10.f}, Justification::centredRight);
    for (int j = 0; j < 16; ++j) {
        text(g, juce::String::fromUTF8(MXC[j].n), jost(600, 7.5f, 0.02f), lab, {kMxX + j * kMxW - 4.f, 31.f, kMxW + 8.f, 10.f},
             Justification::centred);
        drawSym(g, MXC[j].sym, kMxX + j * kMxW + 10.f, 42.f, 10.f, hex(typeOf(MXC[j].sym).d));
    }
    const float W = 16.f * kMxW, H = 16.f * kMxH;
    for (int i = 0; i < 16; ++i) {
        const float y = kMxY + i * kMxH;
        text(g, juce::String::fromUTF8(MXR[i].n), jost(600, 8.f, 0.06f), lab, {kBayX, y, kMxX - 16.f - kBayX, kMxH},
             Justification::centredRight);
        drawSym(g, MXR[i].sym, kMxX - 13.f, y + 2.5f, 10.f, hex(typeOf(MXR[i].sym).d));
        text(g, juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)), mono(false, 8.f), dim,
             {kMxX + W + 15.f, y + 1.f, 12.f, kMxH}, Justification::centredLeft);
    }
    for (int k = 0; k <= 16; ++k) {
        const bool major = k % 4 == 0;
        g.setColour(major ? hex(0x3a3935) : hex(0x222222));
        const float sw = major ? 1.f : 0.7f;
        g.fillRect(kMxX + k * kMxW - sw * 0.5f, kMxY, sw, H);
        g.fillRect(kMxX, kMxY + k * kMxH - sw * 0.5f, W, sw);
    }
    // --- jack field: two heads, the row arrows, symbols and names
    auto head = [&](float x, const char* b, const char* rest) {
        const juce::Font f = jost(700, 8.f, 0.2f);
        const float wb = cssWidth(700, false, 8.f, 0.2f, b), wr = cssWidth(700, false, 8.f, 0.2f, rest);
        const float x0 = x + (280.f - (wb + 6.f + wr)) * 0.5f;
        text(g, b, f, hex(0xede6d6), {x0, 31.f, wb + 4.f, 10.f}, Justification::centredLeft);
        text(g, rest, f, dim, {x0 + wb + 6.f, 31.f, wr + 4.f, 10.f}, Justification::centredLeft);
    };
    head(668.f, "OUT", "SOURCES");
    head(1020.f, "IN", "DESTINATIONS");
    for (int r = 0; r < 4; ++r) {
        const float y = 69.75f + r * 60.f;
        g.setColour(hex(0x4a4843));
        g.fillRect(964.f, y - 0.75f, 33.f, 1.5f);
        Path a;
        a.addTriangle(995.f, y - 4.f, 995.f, y + 4.f, 1002.f, y);
        g.fillPath(a);
    }
    for (int r = 0; r < 2; ++r) {
        const char rc = r == 0 ? 'o' : 'i';
        for (int i = 0; i < 16; ++i) {
            const auto c = jackCentre(rc, i);
            const JackDef& jd = rc == 'o' ? OUTS[i] : INS[i];
            drawSym(g, jd.sym, c.x - 5.f, c.y + 18.f, 10.f, hex(typeOf(jd.sym).d));
            text(g, juce::String::fromUTF8(jd.n), jost(600, 8.5f, 0.08f), hex(0xede6d6), {c.x - 34.f, c.y + 32.f, 68.f, 9.f},
                 Justification::centred);
        }
    }
}

// One patch point (no label): plain, picked (yellow glow) or a valid drop
// target while a cable is carried.
void LiftPanel::drawJack(Graphics& g, char rc, int i, bool picked, bool ok) {
    const auto c = jackCentre(rc, i);
    const float cx = c.x, cy = c.y, R = kJackR;
    const Path body = circle(cx, cy, R);
    if (picked) {
        shadow(g, body, hex(YEL), 0.f, 0.f, 14.f);
        fill(g, circle(cx, cy, R + 4.f), hex(YEL));
    } else if (ok) {
        fill(g, circle(cx, cy, R + 5.f), hex(0xf6f2e9));
        fill(g, circle(cx, cy, R + 3.f), hex(0x050505));
    } else {
        fill(g, circle(cx, cy + 1.f, R + 3.f), W(0.18f));
        shadow(g, circle(cx, cy, R + 3.f), B(0.8f), 0.f, 2.f, 2.f);
        fill(g, circle(cx, cy, R + 3.f), hex(0x050505));
    }
    const JackDef& jd = rc == 'o' ? OUTS[i] : INS[i];
    fill(g, body, hex(typeOf(jd.sym).d));
    fill(g, body, radial(cx - R + 0.34f * 2.f * R, cy - R + 0.28f * 2.f * R, 0.42f * 2.f * R * 1.2f, 0.42f * 2.f * R * 1.2f,
                         {{0.f, W(0.45f)}, {1.f, W(0.f)}}));
    fill(g, body, radial(cx, cy, R * 1.414f, R * 1.414f, {{0.6f, B(0.f)}, {1.f, B(0.4f)}}));
    // nut (conic aluminium) and the hole
    shadow(g, circle(cx, cy, 9.f), B(0.45f), 0.f, 1.f, 1.f);
    g.drawImage(nutImage(), Rectangle<float>(cx - 9.f, cy - 9.f, 18.f, 18.f));
    g.setColour(B(0.35f));
    g.drawEllipse(cx - 8.5f, cy - 8.5f, 17.f, 17.f, 1.f);
    fill(g, circle(cx, cy, 7.5f), hex(0x4a4a47));
    fill(g, circle(cx, cy, 6.5f), hex(0xb9b9b5));
    fill(g, circle(cx, cy, 5.f), hex(0x050505));
}

// One matrix pin (k 1 = +100 %, 2 = +50 %, 3 = inverted), or the empty hole.
void drawPin(Graphics& g, int k, float x, float y, Colour light = hex(0xf6f2e9), Colour dark = hex(0x0b0b0b)) {
    if (k == 3) {
        fill(g, circle(x, y, 4.6f), light);
        fill(g, circle(x, y, 2.4f), dark);
    } else if (k == 1 || k == 2) {
        fill(g, circle(x, y, 4.6f), k == 1 ? light : hex(0x9b958a));
        fill(g, circle(x - 1.3f, y - 1.4f, 1.5f), W(k == 1 ? 0.9f : 0.5f));
    } else {
        fill(g, circle(x, y, 1.8f), hex(0x3d3d3d));
    }
}

// The pins, the hovered row and column, and the row LEDs (lit when a row is used).
void LiftPanel::paintMatrix(Graphics& g) {
    const float W = 16.f * kMxW, H = 16.f * kMxH;
    if (mxHover_ >= 0) {
        g.setColour(hex(0xede6d6, 0.07f));
        g.fillRect(kMxX, kMxY + (mxHover_ / 16) * kMxH, W, kMxH);
        g.fillRect(kMxX + (mxHover_ % 16) * kMxW, kMxY, kMxW, H);
    }
    for (int i = 0; i < 16; ++i) {
        bool used = false;
        for (int j = 0; j < 16; ++j) {
            const int k = pins_[static_cast<size_t>(i * 16 + j)];
            used = used || k != 0;
            const auto c = pinCell(i, j).getCentre();
            drawPin(g, k, c.x, c.y);
        }
        fill(g, circle(kMxX + W + 8.f, kMxY + i * kMxH + 7.5f, 2.2f), used ? hex(0xf6f2e9) : hex(0x3b3a37));
    }
}

void LiftPanel::paintBrand(Graphics& g) {
    // the brand row: the matrix key left, LIFT centred, the jack key right
    const float cy = 334.f;
    const Colour ink = hex(0x1b1b1b);
    const juce::Font small = jost(600, 9.5f, 0.14f);
    auto word = [&](float x, const juce::String& t, Colour c) {
        const float w = cssWidth(600, false, 9.5f, 0.14f, t);
        text(g, t, small, c, {x, cy - 7.f, w + 6.f, 14.f}, Justification::centredLeft);
        return x + w + 7.f;
    };
    float x = 28.f;
    x = word(x, "PINS", ink);
    const char* pv[3] = {"+100", "+50", "\xe2\x88\x92" "100"};
    for (int k = 1; k <= 3; ++k) {
        drawPin(g, k, x + 5.f, cy, ink, hex(0xf1ebdd));
        x = word(x + 17.f, juce::String::fromUTF8(pv[k - 1]), ink);
    }
    x += 14.f;
    word(x, juce::String::fromUTF8("ROW \xe2\x86\x92 COLUMN \xc2\xb7 INPUTS SUM"), hex(0x6d675a));

    const float liftW = cssWidth(700, false, 34.f, 0.3f, "LIFT") - 0.3f * 34.f;
    const float total = 22.f + 12.f + liftW + 12.f + 19.f;
    float lx = 664.f - total * 0.5f;
    Path tri;
    tri.addTriangle(lx, cy + 9.5f, lx + 22.f, cy + 9.5f, lx + 11.f, cy - 9.5f);
    fill(g, tri, hex(YEL));  // v3.1: yellow triangle, blue dot
    lx += 34.f;
    text(g, "LIFT", jost(700, 34.f, 0.3f), ink, {lx, cy - 17.f, liftW + 20.f, 34.f}, Justification::centredLeft);
    lx += liftW + 12.f;
    fill(g, circle(lx + 9.5f, cy, 9.5f), hex(BLU));

    const char types[5] = {'p', 'g', 'c', 'm', 'a'};
    float w = 0.f;
    for (char t : types) {
        w += 9.f + 7.f + cssWidth(600, false, 9.5f, 0.14f, typeOf(t).n) + 7.f;
    }
    x = 1300.f - w + 7.f;
    for (char t : types) {
        drawSym(g, t, x, cy - 4.5f, 9.f, hex(typeOf(t).c));
        x = word(x + 16.f, typeOf(t).n, ink);
    }
}

// ------------------------------------------------------------------ knobs

namespace {

Path knobBody() {
    // BODY: M70 18 C78 18 88 38 96 51.4 A32 32 0 1 1 44 51.4 C52 38 62 18 70 18 Z
    Path body;
    body.startNewSubPath(70.f, 18.f);
    body.cubicTo(78.f, 18.f, 88.f, 38.f, 96.f, 51.4f);
    const float a0 = std::atan2(26.f, 70.05f - 51.4f);
    body.addCentredArc(70.f, 70.05f, 32.f, 32.f, 0.f, a0, 2.f * kPi - a0, false);
    body.cubicTo(52.f, 38.f, 62.f, 18.f, 70.f, 18.f);
    body.closeSubPath();
    return body;
}

}  // namespace

// Knob i's own box (dev): its 140 x 140 drawing origin.
juce::Point<float> LiftPanel::knobOrigin(int i) {
    return {674.f + static_cast<float>(i) * 160.f, 379.75f};
}

// The printed tick ring (lit up to the value). Knob-local coordinates.
void LiftPanel::drawKnobTicks(Graphics& g, float v) {
    for (int t = 0; t <= 10; ++t) {
        const float h = t % 5 == 0 ? 10.f : 7.f;
        Path tk;
        tk.addRectangle(-1.5f, -67.f, 3.f, h);
        g.setColour(static_cast<float>(t) / 10.f <= v + 0.001f ? hex(BLK) : hex(0xb9ab8d));
        g.fillPath(tk, AffineTransform::rotation((-135.f + t * 27.f) * kPi / 180.f).translated(70.f, 70.f));
    }
}

// Skirt and its cast shadow: the same at any value.
void LiftPanel::drawKnobSkirt(Graphics& g, int i) {
    const Colour kc = hex(KNOB[i]);
    const Path skirt = circle(70.f, 70.f, 54.f);
    shadow(g, circle(74.f, 77.f, 54.f), rgba(30, 20, 6, 0.5f), 0.f, 0.f, 12.f);
    fill(g, skirt, kc);
    fill(g, skirt, radial(70.f, 70.f, 54.f, 54.f,
                          {{0.f, B(0.3f)}, {0.55f, B(0.3f)}, {0.7f, W(0.14f)}, {0.9f, W(0.f)}, {1.f, B(0.35f)}}));
    fill(g, skirt, linear(35.44f, 24.64f, 102.4f, 118.6f, {{0.f, W(0.42f)}, {0.3f, W(0.1f)}, {0.55f, B(0.f)}, {1.f, B(0.4f)}}));
    fill(g, skirt, radial(40.f, 40.f, 34.f, 34.f, {{0.f, W(0.42f)}, {0.5f, W(0.12f)}, {1.f, W(0.f)}}));
    fill(g, skirt, radial(102.f, 102.f, 28.f, 28.f, {{0.f, W(0.14f)}, {1.f, W(0.f)}}));
    g.setColour(B(0.45f));
    g.drawEllipse(16.f, 16.f, 108.f, 108.f, 1.f);
}

// The pointer body's contact shadow at rest (unrotated, no offset): the
// renderer caches it once and turns it with the body.
void LiftPanel::drawKnobContact(Graphics& g) {
    static const Path body = knobBody();
    juce::DropShadow(B(0.45f), 4, {}).drawForPath(g, body);
}

// The pointer body, rotated to the value (continuous: 270 degrees of travel).
void LiftPanel::drawKnobBody(Graphics& g, int i, float v, bool contactShadow) {
    static const Path body = knobBody();
    const float deg = -135.f + v * 270.f;
    const float rad = deg * kPi / 180.f;
    const Colour kc = hex(KNOB[i]);
    const AffineTransform rot = AffineTransform::rotation(rad, 70.f, 70.f);
    Path rb(body);
    rb.applyTransform(rot);
    if (contactShadow) {
        Path contact(rb);
        contact.applyTransform(AffineTransform::translation(2.f, 3.f));
        juce::DropShadow(B(0.45f), 4, {}).drawForPath(g, contact);
    }
    fill(g, rb, kc);
    // gloss and rim stay fixed to the light (gradientTransform counter-rotates)
    fill(g, rb, radial(56.f, 50.f, 46.f, 46.f, {{0.f, W(0.55f)}, {0.3f, W(0.12f)}, {0.7f, W(0.f)}, {1.f, B(0.25f)}}));
    fill(g, rb, linear(38.f, 30.f, 102.f, 110.f, {{0.f, W(0.38f)}, {0.45f, W(0.f)}, {1.f, B(0.25f)}}));
    Path inlay;
    inlay.addRoundedRectangle(68.4f, 22.f, 3.2f, 26.f, 1.2f);
    inlay.applyTransform(rot);
    fill(g, inlay, i == 0 ? hex(0x151515) : hex(0xf7f7f4));  // the yellow knob has the dark inlay
    g.setColour(i == 0 ? hex(0xf7f7f4) : B(0.5f));
    g.strokePath(inlay, juce::PathStrokeType(0.6f));
}

// Spun aluminium cap (fixed to the light).
void LiftPanel::drawKnobCap(Graphics& g) {
    fill(g, circle(70.f, 70.f, 21.f), B(0.55f));
    for (int w = 0; w < 24; ++w) {
        const float w0 = w * 15.f * kPi / 180.f, w1 = (w + 1) * 15.f * kPi / 180.f, am = (w0 + w1) * 0.5f;
        const int gv = juce::roundToInt(178.f + 66.f * std::cos(2.f * (am - kPi * 1.25f)));
        Path wd;
        wd.startNewSubPath(70.f, 70.f);
        // SVG angle (from +x, clockwise) -> JUCE angle (from 12 o'clock, clockwise): +pi/2
        wd.addCentredArc(70.f, 70.f, 19.f, 19.f, 0.f, w0 + kPi * 0.5f, w1 + kPi * 0.5f + 0.004f, false);
        wd.closeSubPath();
        fill(g, wd, Colour(static_cast<juce::uint8>(gv), static_cast<juce::uint8>(gv), static_cast<juce::uint8>(juce::jmin(255, gv + 2))));
    }
    g.setColour(W(0.75f));
    g.drawEllipse(51.f, 51.f, 38.f, 38.f, 0.8f);
    g.setColour(B(0.5f));
    g.drawEllipse(50.2f, 50.2f, 39.6f, 39.6f, 0.9f);
}

void LiftPanel::paintKnobLabels(Graphics& g) {
    // labels under the knobs: the colour's symbol, then the name
    const auto L = labels();
    for (int i = 0; i < 4; ++i) {
        const juce::String name = L[slotOf(i)];
        const float cx = knobCentre(i).x, cy = 538.f;
        const float tw = cssWidth(600, false, 11.f, 0.12f, name);
        const float x0 = cx - (11.f + 7.f + tw) * 0.5f;
        drawSym(g, KSYM[i], x0, cy - 5.5f, 11.f, hex(typeOf(KSYM[i]).c));
        text(g, name, jost(600, 11.f, 0.12f), hex(0x1b1b1b), {x0 + 18.f, cy - 8.25f, tw + 8.f, 16.5f},
             Justification::centredLeft);
    }
}

// ------------------------------------------------------------------ pads

// Pad body (cap, skirt, shadows, light): what a pad looks like without its
// printing. The same for every pad of a colour and state, so it is cached.
void LiftPanel::drawPadBody(Graphics& g, Rectangle<float> r, juce::uint32 fillCol, bool lit) {
    const Path p = rrect(r, 7.f);
    shadow(g, p, rgba(40, 30, 10, 0.28f), 0.f, 10.f, 12.f);
    shadow(g, p, B(0.28f), 0.f, 5.f, 1.f);
    fill(g, rrect(r.translated(0.f, 4.f), 7.f), lit ? hex(psk(fillCol)) : hex(0xc6baa0));
    fill(g, p, lit ? hex(fillCol) : hex(0xf4eedf));
    fill(g, p, linear(r.getX(), r.getY(), r.getX(), r.getBottom(), {{0.f, W(lit ? 0.2f : 0.55f)}, {0.36f, W(0.f)}, {1.f, W(0.f)}}));
    fill(g, p, radial(r.getCentreX(), r.getY() + 43.2f, 51.84f, 41.76f, {{0.f, B(0.07f)}, {0.72f, B(0.f)}}));
    inset(g, p, W(lit ? 0.4f : 0.85f), 0.f, 1.f, 0.f);
    inset(g, p, B(0.12f), 0.f, -2.f, 1.f);
    inset(g, p, W(0.25f), 1.f, 0.f, 0.f);
}

void LiftPanel::drawPadText(Graphics& g, const PadLook& L, float alpha) {
    const Rectangle<float> r = L.r;
    const Colour ink = L.ink.withMultipliedAlpha(alpha);
    if (L.big.isNotEmpty()) {
        text(g, L.big, jost(L.bigSize > 20.f ? 500 : 600, L.bigSize), ink, {r.getX() + 9.f, r.getY() + 8.f, 60.f, L.bigSize},
             Justification::centredLeft);
    }
    text(g, L.label, jost(600, 10.f, 0.1f), ink, {r.getX() + 9.f, r.getBottom() - 8.f - 12.f, 62.f, 12.f},
         Justification::centredLeft);
}

LiftPanel::PadLook LiftPanel::padLook(int k) const {
    static const char* modeNames[5] = {"SYNTH", "DRUM", "TAPE", "MIX", "IN"};
    PadLook L;
    L.r = padRect(k);
    if (pressedPad_ == k) {
        L.r = L.r.translated(0.f, 2.f);
    }
    L.col = BLK;  // v3.1: a lit pad is black (ARM: red)
    if (k < 5) {
        L.lit = mode_ == k && !bay_;
        L.label = modeNames[k];
    } else if (k == 5) {
        L.lit = fx_;
        L.label = "FX";
    } else if (k == 6) {
        L.lit = bay_;
        L.label = "BAY";
    } else if (k == 7) {
        L.lit = mode_ == In && sel_[In] == 2 && !bay_;
        L.label = "RADIO";
    } else if (k < 16) {
        const int i = k - 8;
        L.unused = mode_ == Mix || (mode_ == In && INPUTS[i][0] == 0);
        L.lit = !L.unused && sel_[static_cast<size_t>(mode_)] == i;
        L.big = juce::String(i + 1);
        L.bigSize = 24.f;
        juce::String sub = mode_ == Synth ? juce::String(ENGINES[i].n)
                           : mode_ == Drum ? juce::String(KITS[i])
                           : mode_ == Tape ? juce::String("TAPE")
                           : mode_ == In   ? juce::String(INPUTS[i])
                                           : juce::String();
        L.label = sub.isNotEmpty() ? sub : juce::String::fromUTF8("\xc2\xb7");
    } else {
        const int t = (k - 16) % 4;
        const bool arm = k < 20;
        L.lit = arm ? arm_ == t : mutes_[static_cast<size_t>(t)];
        L.col = arm ? RED : BLK;
        L.big = (arm ? "T" : "M") + juce::String(t + 1);
        L.bigSize = 18.f;
        L.label = arm ? "ARM" : "MUTE";
    }
    L.ink = L.lit ? hex(CRM) : hex(BLK);
    return L;
}

// ------------------------------------------------------------------ membrane

void LiftPanel::paintMembraneStatic(Graphics& g, bool) {
    // v3.1: the push-encoders sit on the case; their labels are printed on it
    static const char* labels[10] = {"LIFT", "LOOP", "SHIFT", "REV", "DROP", "REC", "OCT \xe2\x88\x92", "PLAY", "OCT +", "STOP"};
    for (int k = 0; k < 10; ++k) {
        const auto c = encCentre(k);
        const juce::String t = juce::String::fromUTF8(labels[MEM_FN[k]]);
        const float tw = cssWidth(600, false, 10.f, 0.12f, t);
        const float x0 = c.x - (tw + 6.f + 5.f) * 0.5f;
        text(g, t, jost(600, 10.f, 0.12f), hex(0x1b1b1b), {x0, c.y + 30.f, tw + 6.f, 12.f}, Justification::centredLeft);
    }
}

namespace {

// encoder kinds: 0 machined aluminium, 1 writes (red line), 2 red anodised (REC), 3 black (SHIFT)
int encKind(int fn) { return fn == 5 ? 2 : fn == 2 ? 3 : fn == 4 ? 1 : 0; }

void drawEncoder(Graphics& g, int fn, float v, juce::Point<float> c, bool down) {
    const int kind = encKind(fn);
    const Colour f0 = kind == 2 ? hex(0xd9463a) : kind == 3 ? hex(0x3a3936) : hex(0xe9e8e4);
    const Colour f1 = kind == 2 ? hex(0xa8281f) : kind == 3 ? hex(0x161615) : hex(0xa9a8a3);
    const Colour ink = kind >= 2 ? hex(0xf6f2e9) : hex(0x1b1b1b);
    const Colour lineC = kind == 1 ? hex(RED) : ink;
    Graphics::ScopedSaveState ss(g);
    g.addTransform(AffineTransform::translation(c.x - 24.f, c.y - 24.f));
    if (down) {
        g.addTransform(AffineTransform::scale(0.965f, 0.965f, 24.f, 24.f).translated(0.f, 1.f));
    }
    fill(g, circle(24.f, 24.f, 26.f), rgba(70, 52, 22, 0.16f));
    g.setColour(W(0.75f));
    g.drawEllipse(24.f - 25.2f, 23.4f - 25.2f, 50.4f, 50.4f, 0.8f);
    shadow(g, circle(25.5f, 27.5f, 23.6f), rgba(50, 36, 12, 0.32f), 0.f, 0.f, 3.f);
    const auto face = [&](float r) { return linear(24.f - r + 0.2f * 2.f * r, 24.f - r + 0.1f * 2.f * r, 24.f - r + 0.8f * 2.f * r,
                                                   24.f - r + 0.95f * 2.f * r, {{0.f, f0}, {1.f, f1}}); };
    const auto sheen = [&](float r, float op) {
        return linear(24.f - r, 24.f - r, 24.f + r, 24.f + r, {{0.f, W(0.55f * op)}, {0.5f, W(0.f)}, {1.f, B(0.35f * op)}});
    };
    fill(g, circle(24.f, 24.f, 23.6f), face(23.6f));
    g.setColour(kind <= 1 ? B(0.42f) : B(0.55f));
    for (int k = 0; k < 72; ++k) {
        const float a = static_cast<float>(k) * 5.f * kPi / 180.f, cs = std::cos(a), sn = std::sin(a);
        g.drawLine(24.f + 20.2f * cs, 24.f + 20.2f * sn, 24.f + 23.4f * cs, 24.f + 23.4f * sn, 1.15f);
    }
    fill(g, circle(24.f, 24.f, 23.6f), sheen(23.6f, 0.6f));
    fill(g, circle(24.f, 24.f, 19.4f), face(19.4f));
    g.setColour(B(0.35f));
    g.drawEllipse(4.6f, 4.6f, 38.8f, 38.8f, 0.8f);
    for (int r = 3; r <= 19; r += 2) {
        g.setColour(r % 4 == 1 ? W(0.16f) : B(0.07f));
        g.drawEllipse(24.f - r, 24.f - r, 2.f * r, 2.f * r, 0.6f);
    }
    fill(g, circle(24.f, 24.f, 19.4f), sheen(19.4f, 0.35f));
    Path ptr;
    ptr.addRoundedRectangle(23.1f, 6.2f, 1.8f, 7.4f, 0.5f);
    g.setColour(lineC);
    g.fillPath(ptr, AffineTransform::rotation((-135.f + v * 270.f) * kPi / 180.f, 24.f, 24.f));
    // the action mark
    g.setColour(ink);
    const auto T = AffineTransform::translation(24.f, 24.f);
    const juce::PathStrokeType st(1.6f);
    Path m;
    switch (fn) {
    case 0: m.startNewSubPath(-4.f, 2.5f); m.lineTo(0.f, -3.f); m.lineTo(4.f, 2.5f); g.strokePath(m, st, T); break;
    case 4: m.startNewSubPath(-4.f, -2.5f); m.lineTo(0.f, 3.f); m.lineTo(4.f, -2.5f); g.strokePath(m, st, T); break;
    case 1: m.addEllipse(-3.6f, -3.6f, 7.2f, 7.2f); g.strokePath(m, st, T); break;
    case 3: m.addTriangle(3.f, -3.5f, -3.5f, 0.f, 3.f, 3.5f); g.fillPath(m, T); break;
    case 7: m.addTriangle(-3.f, -3.5f, 3.5f, 0.f, -3.f, 3.5f); g.fillPath(m, T); break;
    case 9: g.fillRect(21.f, 21.f, 6.f, 6.f); break;
    case 5: g.fillEllipse(20.6f, 20.6f, 6.8f, 6.8f); break;
    case 2:
        m.startNewSubPath(0.f, -4.f); m.lineTo(4.f, 0.f); m.lineTo(1.6f, 0.f); m.lineTo(1.6f, 3.5f);
        m.lineTo(-1.6f, 3.5f); m.lineTo(-1.6f, 0.f); m.lineTo(-4.f, 0.f); m.closeSubPath();
        g.fillPath(m, T);
        break;
    case 6: g.fillRect(20.4f, 23.2f, 7.2f, 1.6f); break;
    default: g.fillRect(20.4f, 23.2f, 7.2f, 1.6f); g.fillRect(23.2f, 20.4f, 1.6f, 7.2f); break;
    }
}

}  // namespace

void LiftPanel::paintMemKeys(Graphics& g) {
    static const char* labels[10] = {"LIFT", "LOOP", "SHIFT", "REV", "DROP", "REC", "OCT \xe2\x88\x92", "PLAY", "OCT +", "STOP"};
    for (int k = 0; k < 10; ++k) {
        const int f = MEM_FN[k];
        const auto c = encCentre(k);
        drawEncoder(g, f, mk_[static_cast<size_t>(k)], c, pressedMem_ == k);
        // the status LED after the printed label
        const bool lit = (f == 1 && loop_) || (f == 2 && shiftActive()) || (f == 3 && rev_) || (f == 5 && rec_) || (f == 7 && playing_);
        const float tw = cssWidth(600, false, 10.f, 0.12f, juce::String::fromUTF8(labels[f]));
        const float lx = c.x - (tw + 6.f + 5.f) * 0.5f + tw + 6.f + 2.5f, ly = c.y + 36.f;
        if (lit) {
            const Colour on = (f == 5) ? hex(0xff3b2b) : hex(0xfffaf0);
            fill(g, circle(lx, ly, 6.f), on.withAlpha(0.35f));
            fill(g, circle(lx, ly, 2.5f), on);
        } else {
            fill(g, circle(lx, ly, 2.5f), hex(0xc9bca0));
            g.setColour(rgba(60, 45, 20, 0.45f));
            g.drawEllipse(lx - 2.5f, ly - 2.5f, 5.f, 5.f, 0.6f);
        }
    }
}

// ------------------------------------------------------------------ keyboard

void LiftPanel::drawSharp(Graphics& g, juce::Point<float> c, bool down) {
    const Path p = circle(c.x, c.y, 21.f);
    if (down) {
        shadow(g, p, B(0.4f), 0.f, 2.f, 2.f);
        fill(g, circle(c.x, c.y + 1.f, 21.f), hex(0x000000));
    } else {
        shadow(g, p, rgba(40, 30, 10, 0.32f), 0.f, 10.f, 12.f);
        shadow(g, p, B(0.35f), 0.f, 5.f, 1.f);
        fill(g, circle(c.x, c.y + 4.f, 21.f), hex(0x050505));
    }
    fill(g, p, hex(0x1d1d1d));
    fill(g, p, radial(c.x, c.y, 29.7f, 29.7f, {{0.58f, B(0.f)}, {1.f, B(0.5f)}}));
    fill(g, p, radial(c.x - 21.f + 15.12f, c.y - 21.f + 10.92f, 14.28f, 9.24f, {{0.f, W(0.42f)}, {1.f, W(0.f)}}));
    inset(g, p, W(down ? 0.15f : 0.18f), 0.f, 1.f, 0.f);
}

void LiftPanel::drawNatural(Graphics& g, Rectangle<float> r, bool down) {
    const Path p = rrect(r, 6.f);
    if (down) {
        shadow(g, p, B(0.25f), 0.f, 2.f, 2.f);
        fill(g, rrect(r.translated(0.f, 1.f), 6.f), hex(0xc9c0ab));
    } else {
        shadow(g, p, rgba(40, 30, 10, 0.28f), 0.f, 10.f, 12.f);
        shadow(g, p, B(0.28f), 0.f, 5.f, 1.f);
        fill(g, rrect(r.translated(0.f, 4.f), 6.f), hex(0xcbc2ad));
    }
    fill(g, p, hex(0xf7f4ec));
    fill(g, p, linear(r.getX(), r.getY(), r.getX(), r.getBottom(), {{0.f, W(0.75f)}, {0.32f, W(0.f)}, {1.f, W(0.f)}}));
    fill(g, p, radial(r.getCentreX(), r.getY() + 0.55f * r.getHeight(), 0.75f * r.getWidth(), 0.55f * r.getHeight(),
                      {{0.f, B(0.05f)}, {0.72f, B(0.f)}}));
    inset(g, p, down ? W(0.8f) : hex(0xffffff), 0.f, 1.f, 0.f);
    if (!down) {
        inset(g, p, B(0.08f), 0.f, -2.f, 1.f);
    }
}

juce::Point<float> LiftPanel::sharpPos(int o, int b) const {
    juce::Point<float> c = sharpCentre(o, b);
    if (heldNote_ == o * 12 + SHARP_DEF[b][0]) {
        c.y += 3.f;
    }
    return c;
}

Rectangle<float> LiftPanel::naturalRect(int j) const {
    Rectangle<float> r = natRect(j);
    if (heldNote_ == (j / 7) * 12 + NAT_SEMI[j % 7]) {
        r = r.translated(0.f, 3.f);
    }
    return r;
}

void LiftPanel::paintKeyLabels(Graphics& g) {
    // Only the C keys carry their octave (the prototype labelled every
    // natural after the first redraw; fixed here).
    for (int o = 0; o < 2; ++o) {
        const Rectangle<float> r = naturalRect(o * 7);
        text(g, "C" + juce::String(3 + o + oct_), jost(600, 10.f, 0.08f), hex(0x1b1b1b),
             {r.getX(), r.getBottom() - 9.f - 10.f, r.getWidth() + 0.8f, 10.f}, Justification::centred);
    }
}

// ------------------------------------------------------------------ cables

namespace {

Path cablePath(float x1, float y1, float x2, float y2) {
    const float sag = 36.f + std::fmin(std::hypot(x2 - x1, y2 - y1) * 0.06f, 28.f);
    Path p;
    p.startNewSubPath(x1, y1);
    p.cubicTo(x1, y1 + sag, x2, y2 + sag, x2, y2);
    return p;
}

void drawPlug(Graphics& g, float x, float y, const Cloth& k, bool st, bool aside, bool cheapShadow) {
    auto C = [&](Colour c) { return aside ? grayed(c) : c; };
    if (cheapShadow) {
        for (int n = 0; n < 3; ++n) {
            fill(g, circle(x + 3.f, y + 6.f, 15.f + 4.f - 2.f * static_cast<float>(n)), C(rgba(30, 20, 6, 0.09f + 0.04f * n)));
        }
    } else {
        shadow(g, circle(x + 3.f, y + 6.f, 15.f), C(rgba(30, 20, 6, 0.4f)), 0.f, 0.f, 7.f);
    }
    const Path boot = circle(x, y, 14.f);
    fill(g, boot, C(hex(k.boot)));
    fill(g, boot, radial(x - 14.f + 10.08f, y - 14.f + 8.4f, 20.16f, 20.16f, {{0.f, W(0.42f)}, {0.45f, W(0.f)}, {1.f, B(0.45f)}}));
    if (st) {
        g.setColour(C(hex(k.c)));
        g.drawEllipse(x - 10.f, y - 10.f, 20.f, 20.f, 2.2f);
        const Path sock = circle(x, y, 7.6f);
        fill(g, sock, C(juce::String(k.n) == "Black" ? hex(0xe8ddc3) : hex(0x151515)));
        g.setColour(hex(0x000000));
        g.drawEllipse(x - 7.6f, y - 7.6f, 15.2f, 15.2f, 0.8f);
        fill(g, sock, radial(x - 7.6f + 5.78f, y - 7.6f + 4.56f, 10.64f, 10.64f, {{0.f, W(0.28f)}, {0.55f, W(0.04f)}, {1.f, W(0.f)}}));
        fill(g, circle(x, y, 4.2f), hex(0x000000));
    } else {
        g.setColour(W(0.12f));
        g.drawEllipse(x - 9.5f, y - 9.5f, 19.f, 19.f, 1.5f);
        fill(g, circle(x, y, 5.f), C(hex(k.c)));
    }
}

void drawCable(Graphics& g, const Path& d, float x1, float y1, float x2, float y2, int ci, bool st, bool aside, bool cheapShadow) {
    const Cloth& k = CLOTH[ci];
    auto C = [&](Colour c) { return aside ? grayed(c) : c; };
    const juce::PathStrokeType round(8.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    if (cheapShadow) {
        // The cable in hand: its soft shadow as three widening offset strokes
        // (no blur pass per frame). Settled cables keep the blurred one.
        const AffineTransform off = AffineTransform::translation(5.f, 15.f);
        const float wid[3] = {22.f, 16.f, 11.f};
        const float al[3] = {0.07f, 0.1f, 0.14f};
        for (int n = 0; n < 3; ++n) {
            g.setColour(C(rgba(30, 20, 6, al[n])));
            g.strokePath(d, juce::PathStrokeType(wid[n], juce::PathStrokeType::curved, juce::PathStrokeType::rounded), off);
        }
    } else {
        Path sh;
        juce::PathStrokeType(10.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath(sh, d);
        shadow(g, sh, C(rgba(30, 20, 6, 0.38f)), 5.f, 15.f, 7.f);
    }
    g.setColour(C(hex(k.c)));
    g.strokePath(d, round);
    {
        Path dk;
        const float dash[2] = {1.7f, 1.5f};
        juce::PathStrokeType(8.5f).createDashedStroke(dk, d, dash, 2);
        fill(g, dk, C(hex(k.dk, 0.75f)));
        Path lt;
        const float dash2[2] = {1.f, 2.2f};
        juce::PathStrokeType(4.5f).createDashedStroke(lt, d, dash2, 2);
        fill(g, lt, C(hex(k.lt, 0.6f)));
    }
    g.setColour(B(0.35f));
    g.strokePath(d, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                 AffineTransform::translation(1.6f, 2.4f));
    g.setColour(W(0.14f));
    g.strokePath(d, juce::PathStrokeType(2.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                 AffineTransform::translation(-1.4f, -2.f));
    drawPlug(g, x1, y1, k, st, aside, cheapShadow);
    drawPlug(g, x2, y2, k, st, aside, cheapShadow);
}

}  // namespace

void LiftPanel::drawSettledCable(Graphics& g, const Path& d, float x1, float y1, float x2, float y2, int ci, bool st, bool cheap) {
    drawCable(g, d, x1, y1, x2, y2, ci, st, false, cheap);
}

// Area a cable can touch (dev): the path, its shadow and plugs.
Rectangle<float> LiftPanel::cableBounds(const Rectangle<float>& path) {
    return path.expanded(26.f).withTrimmedBottom(-20.f).withTrimmedRight(-8.f);
}

// ---- ropes (CableRope.h): where a cord's plugs sit, and the scene they avoid

LiftPanel::CordEnds LiftPanel::cordEnds(size_t n, const std::vector<Level>& lv) const {
    const Cord& c = cords_[n];
    const auto A = jackCentre('o', c.o), Bp = jackCentre('i', c.i);
    return {A.x, A.y - 6.f * static_cast<float>(lv[n].o), Bp.x, Bp.y - 6.f * static_cast<float>(lv[n].i)};
}

ui::Rope& LiftPanel::ropeFor(size_t n, const std::vector<Level>& lv) {
    const Cord& c = cords_[n];
    const auto e = cordEnds(n, lv);
    ui::Rope& R = ropes_[c.o * 16 + c.i];
    R.attach({e.x1, e.y1 + 4.f}, {e.x2, e.y2 + 4.f}, scene_);  // the rope starts 4 px below the plug centre
    return R;
}

void LiftPanel::buildRopeScene() {
    scene_.labels.clear();
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < 16; ++i) {
            const auto c = jackCentre(r == 0 ? 'o' : 'i', i);
            scene_.labels.push_back({c.x - 30.f, c.y + 32.f, 60.f, 9.f});
        }
    }
    for (int k = 0; k < 16; ++k) {
        scene_.labels.push_back({kBayX, kMxY + k * kMxH, kMxX - 16.f - kBayX, kMxH});
        scene_.labels.push_back({kMxX + k * kMxW, 31.f, kMxW, 10.f});
    }
    for (int i = 0; i < 4; ++i) {
        scene_.labels.push_back({knobCentre(i).x - 40.f, 529.75f, 80.f, 16.5f});
    }
    for (int k = 0; k < 10; ++k) {
        scene_.labels.push_back({encCentre(k).x - 26.f, encCentre(k).y + 30.f, 52.f, 12.f});
    }
    scene_.labels.push_back({28.f, 326.88f, 555.f, 14.25f});
    scene_.labels.push_back({583.34f, 317.f, 161.31f, 34.f});
    scene_.labels.push_back({744.66f, 326.88f, 555.f, 14.25f});
    scene_.labels.push_back({668.f, 31.f, 280.f, 10.f});
    scene_.labels.push_back({1020.f, 31.f, 280.f, 10.f});
    scene_.floor = 352.f - 2.f;
}

// CableLayer::track: pointer, hovered jack, hovered label (rect + 3 px).
void LiftPanel::trackRopes(juce::Point<float> dev, bool in) {
    const ui::RopeScene before = scene_;
    scene_.ptrIn = in;
    scene_.ptr = dev;
    const JackId j = in ? jackAt(dev) : JackId{};
    scene_.hasJack = j.valid();
    if (j.valid()) {
        scene_.jack = jackCentre(j.r, j.i);
    }
    scene_.hoverLabel = -1;
    if (in) {
        for (size_t k = 0; k < scene_.labels.size(); ++k) {
            if (scene_.labels[k].expanded(3.f).contains(dev)) {
                scene_.hoverLabel = static_cast<int>(k);
                break;
            }
        }
    }
    if (cords_.empty()) {
        return;
    }
    // wake the ropes only when the pointer (now or a moment ago), a hovered
    // jack or label can touch one: a press on the pads leaves them asleep
    bool near = scene_.hasJack != before.hasJack || scene_.hoverLabel != before.hoverLabel ||
                (scene_.hasJack && scene_.jack != before.jack);
    if (!near) {
        const auto lv = levels(-1);
        for (size_t n = 0; n < cords_.size() && !near; ++n) {
            const auto b = ropeFor(n, lv).bounds().expanded(45.f);
            near = (in && b.contains(dev)) || (before.ptrIn && b.contains(before.ptr));
        }
    }
    if (near) {
        ropeIdle_ = 0;
        ropeAwake_ = true;
    }
}

// One display frame of rope physics: step every cable until all settle.
void LiftPanel::stepRopes() {
    if (!ropeAwake_ || cords_.empty()) {
        ropeAwake_ = false;
        return;
    }
    const bool moving = drag_.active && drag_.isMove && drag_.started;
    const auto lv = levels(moving ? drag_.n : -1);
    float mv = 0.f;
    Rectangle<float> u;
    for (size_t n = 0; n < cords_.size(); ++n) {
        ui::Rope& R = ropeFor(n, lv);
        const auto b0 = R.bounds();
        mv = std::fmax(mv, R.step(scene_));
        const auto b = b0.getUnion(R.bounds());
        u = u.isEmpty() ? b : u.getUnion(b);
    }
    if (mv > 0.03f) {
        ropeIdle_ = 0;
    } else {
        ++ropeIdle_;
    }
    if (cables_ != nullptr && !u.isEmpty()) {
        const auto r = toLocal(cableBounds(u) + juce::Point<float>(kDevX, kDevY));
        cables_->repaint(r.getUnion(ropeShown_));
        ropeShown_ = r;
    }
    if (ropeIdle_ >= 20) {
        ropeAwake_ = false;  // settled: back to the cached image
        cableKey_ = 0;
        refreshOverlay();
    }
}

// Settled cables (all but the one in hand). While a cable is carried the
// others step aside: greyed at half opacity.
void LiftPanel::paintCables(Graphics& g) {
    const bool moving = drag_.active && drag_.isMove && drag_.started;
    const auto lv = levels(moving ? drag_.n : -1);
    const bool aside = drag_.active && drag_.started;
    // the cables stepping aside share one half-opacity layer (one composite a
    // frame, not one per cable)
    if (aside) {
        g.beginTransparencyLayer(0.5f);
    }
    for (size_t n = 0; n < cords_.size(); ++n) {
        if (moving && static_cast<int>(n) == drag_.n) {
            continue;
        }
        const Cord& c = cords_[n];
        const auto e = cordEnds(n, lv);
        drawCable(g, ropeFor(n, lv).path(), e.x1, e.y1, e.x2, e.y2, cableCloth(c.o), c.st, aside, ropeAwake_);
    }
    if (aside) {
        g.endTransparencyLayer();
    }
}

juce::Point<float> LiftPanel::liveFixed() const {
    const auto f = jackCentre(drag_.fixed.r, drag_.fixed.i);
    return {f.x, f.y - 6.f * static_cast<float>(drag_.flv)};
}

int LiftPanel::liveCloth() const {
    if (drag_.fixed.r == 'o') {
        return cableCloth(drag_.fixed.i);
    }
    return drag_.isMove && drag_.n >= 0 ? cableCloth(cords_[static_cast<size_t>(drag_.n)].o) : 0;
}

// The cable in hand.
void LiftPanel::paintLiveCable(Graphics& g) {
    if (drag_.active && drag_.started) {
        const auto f = liveFixed();
        drawCable(g, cablePath(f.x, f.y, drag_.p.x, drag_.p.y), f.x, f.y, drag_.p.x, drag_.p.y, liveCloth(), drag_.st, false, true);
    }
}

Rectangle<float> LiftPanel::liveCableBounds() const {
    if (!(drag_.active && drag_.started)) {
        return {};
    }
    const auto f = liveFixed();
    return cableBounds(cablePath(f.x, f.y, drag_.p.x, drag_.p.y).getBounds());
}

// ------------------------------------------------------------------ menu

void LiftPanel::paintMenu(Graphics& g) {
    if (!menu_.open) {
        return;
    }
    const auto b = menu_.bounds;
    const Path p = rrect(b, 8.f);
    shadow(g, p, B(0.45f), 0.f, 10.f, 30.f);
    fill(g, p, hex(0x1c1b19));
    text(g, menu_.header.toUpperCase(), jost(400, 11.f, 0.14f), hex(0xa8a194),
         {b.getX() + 14.f, b.getY() + 4.f + 7.f, b.getWidth() - 20.f, 14.3f}, Justification::centredLeft);
    for (size_t k = 0; k < menu_.items.size(); ++k) {
        const auto& it = menu_.items[k];
        const Rectangle<float> row(b.getX() + 4.f, b.getY() + 4.f + 26.3f + 32.9f * static_cast<float>(k), b.getWidth() - 8.f, 32.9f);
        if (menu_.hover == static_cast<int>(k)) {
            fill(g, rrect(row, 5.f), hex(0x33312c));
        }
        float x = row.getX() + 10.f;
        if (it.hasDot) {
            fill(g, circle(x + 6.f, row.getCentreY(), 6.f), it.dot);
            g.setColour(W(0.25f));
            g.drawEllipse(x + 0.5f, row.getCentreY() - 5.5f, 11.f, 11.f, 1.f);
            x += 21.f;
        }
        text(g, it.label, jost(400, 13.f), hex(0xede6d6), {x, row.getY(), row.getRight() - x, row.getHeight()},
             Justification::centredLeft);
    }
}

}  // namespace lift
