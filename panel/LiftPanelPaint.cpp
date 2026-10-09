// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Native drawing of the LIFT panel. Each block cites the prototype CSS/SVG it
// reproduces; numbers are the prototype's CSS pixels.

#include "LiftPanel.h"

#include "Fonts.h"
#include "PanelData.h"

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

namespace {

constexpr float kPi = juce::MathConstants<float>::pi;

struct Stop {
    float p;
    Colour c;
};

FillType radial(float cx, float cy, float rx, float ry, std::initializer_list<Stop> stops) {
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

FillType linear(float x1, float y1, float x2, float y2, std::initializer_list<Stop> stops) {
    ColourGradient gr;
    gr.isRadial = false;
    gr.point1 = {x1, y1};
    gr.point2 = {x2, y2};
    for (const auto& s : stops) {
        gr.addColour(juce::jlimit(0.0, 1.0, static_cast<double>(s.p)), s.c);
    }
    return FillType(gr);
}

Colour W(float a) {
    return Colour::fromFloatRGBA(1.f, 1.f, 1.f, a);
}
Colour B(float a) {
    return Colour::fromFloatRGBA(0.f, 0.f, 0.f, a);
}
Colour rgba(int r, int g, int b, float a) {
    return Colour(static_cast<juce::uint8>(r), static_cast<juce::uint8>(g), static_cast<juce::uint8>(b), a);
}

Path rrect(Rectangle<float> r, float rad) {
    Path p;
    p.addRoundedRectangle(r, rad);
    return p;
}

Path circle(float cx, float cy, float r) {
    Path p;
    p.addEllipse(cx - r, cy - r, 2.f * r, 2.f * r);
    return p;
}

// CSS box-shadow (outer). Blur is the CSS blur radius.
void shadow(Graphics& g, const Path& p, Colour c, float dx, float dy, float blur) {
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
void inset(Graphics& g, const Path& shape, Colour c, float dx, float dy, float blur) {
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

void fill(Graphics& g, const Path& p, const FillType& f) {
    g.setFillType(f);
    g.fillPath(p);
}

void fill(Graphics& g, const Path& p, Colour c) {
    g.setColour(c);
    g.fillPath(p);
}

void text(Graphics& g, const juce::String& s, const juce::Font& f, Colour c, Rectangle<float> r, Justification j) {
    g.setFont(f);
    g.setColour(c);
    g.drawText(s, r, j, false);
}

// SVG <text>: baseline at y; anchor -1 start, 0 middle, 1 end.
void svgText(Graphics& g, const juce::String& s, float x, float y, float size, Colour c, int anchor = -1,
             bool bold = false, float ls = 0.f) {
    const juce::Font f = mono(bold, size, ls / size);
    g.setFont(f);
    g.setColour(c);
    const float w = cssWidth(bold ? 700 : 400, true, size, ls / size, s);
    float x0 = x;
    if (anchor == 0) {
        x0 = x - w * 0.5f;
    } else if (anchor > 0) {
        x0 = x - w;
    }
    juce::GlyphArrangement ga;
    ga.addLineOfText(f, s, x0, y);
    ga.draw(g);
}

void line(Graphics& g, float x1, float y1, float x2, float y2, Colour c, float w) {
    g.setColour(c);
    g.drawLine(x1, y1, x2, y2, w);
}

Colour grayed(Colour c) {
    // CSS filter: grayscale(1) brightness(.8)
    const float l = 0.2126f * c.getFloatRed() + 0.7152f * c.getFloatGreen() + 0.0722f * c.getFloatBlue();
    const float v = juce::jlimit(0.f, 1.f, l * 0.8f);
    return Colour::fromFloatRGBA(v, v, v, c.getFloatAlpha());
}

// conic-gradient(from 15deg, ...) chrome nut, cached per scale
const juce::Image& nutImage() {
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

}  // namespace

// ------------------------------------------------------------------ grain

void LiftPanel::buildGrain() {
    // feTurbulence fractalNoise (0.95, 2 octaves) through the colour matrix,
    // multiplied at 55 % opacity: a fine dark speckle on the beige case.
    grain_ = juce::Image(juce::Image::ARGB, 1360, 866, true);
    juce::Image::BitmapData bd(grain_, juce::Image::BitmapData::writeOnly);
    juce::Random rnd(4);
    std::vector<float> a(static_cast<size_t>(1360 * 867), 0.f);
    for (auto& v : a) {
        v = rnd.nextFloat();
    }
    for (int y = 0; y < 866; ++y) {
        for (int x = 0; x < 1360; ++x) {
            const size_t i = static_cast<size_t>(y * 1360 + x);
            const float n1 = a[i];
            const float n2 = 0.5f * (a[i] + a[x + 1 < 1360 ? i + 1 : i]) * 0.5f + 0.5f * a[i + 1360] * 0.5f;
            const float n = 0.5f + (n1 - 0.5f) * 0.36f + (n2 - 0.25f) * 0.18f;
            const float alpha = juce::jlimit(0.f, 1.f, 0.55f * n - 0.18f) * 0.55f;
            bd.setPixelColour(x, y, rgba(97, 80, 51, alpha));
        }
    }
}

// ------------------------------------------------------------------ paint

void LiftPanel::paint(Graphics& g) {
    g.fillAll(hex(0xc2bdb3));
    paintTopBar(g);
    {
        Graphics::ScopedSaveState s(g);
        g.addTransform(AffineTransform::translation(kDevX, kDevY));
        paintCase(g);
        paintBay(g);
        paintBrand(g);
        paintScreen(g);
        paintKnobs(g);
        paintPads(g);
        paintMembrane(g);
        paintKeys(g);
        paintCables(g);
    }
    paintMenu(g);
}

void LiftPanel::paintTopBar(Graphics& g) {
    const Colour fg = hex(0x24221e);
    text(g, "CABLE COLOR", jost(600, 12.f, 0.14f), fg, {16.f, 14.f, 200.f, 26.f}, Justification::centredLeft);
    const auto r = topBarRects();
    for (int k = 0; k < 6; ++k) {
        const auto b = r[static_cast<size_t>(k)];
        const float cx = b.getCentreX(), cy = b.getCentreY();
        fill(g, circle(cx, cy, 13.f), hex(CLOTH[k].c));
        if (stack_) {
            fill(g, circle(cx, cy, 6.25f), k == 0 ? hex(0xe8ddc3) : hex(0x1a1a1a));
            fill(g, circle(cx, cy, 3.25f), hex(0x000000));
        }
        g.setColour(B(0.35f));
        g.drawEllipse(cx - 10.5f, cy - 10.5f, 21.f, 21.f, 1.f);
        if (k == color_) {
            g.setColour(fg);
            g.drawEllipse(cx - 12.f, cy - 12.f, 24.f, 24.f, 2.f);
        }
    }
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
    text(g, info_, jost(400, 13.f), hex(0x57524a), {16.f, 48.f, 1400.f, 19.5f}, Justification::centredLeft);
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

void LiftPanel::paintBay(Graphics& g) {
    const Path bay = rrect({44.f, 24.f, 1272.f, 156.f}, 10.f);
    fill(g, bay, hex(0xddd1b5));
    inset(g, bay, rgba(60, 50, 30, 0.26f), 0.f, 2.f, 5.f);
    inset(g, bay, W(0.7f), 0.f, -1.f, 0.f);

    std::vector<bool> used(32, false);
    for (const auto& c : cords_) {
        used[static_cast<size_t>(c.o)] = true;
        used[static_cast<size_t>(16 + c.i)] = true;
    }
    for (int r = 0; r < 2; ++r) {
        const char rc = r == 0 ? 'o' : 'i';
        for (int i = 0; i < 16; ++i) {
            const float cx = jx(i), cy = jy(rc);
            const Path body = circle(cx, cy, 16.f);
            const bool picked = pick_.valid() && pick_.r == rc && pick_.i == i;
            const bool ok = drag_.active && drag_.started && validTarget(rc, i);
            if (picked) {
                shadow(g, body, hex(YEL), 0.f, 0.f, 14.f);
                fill(g, circle(cx, cy, 20.f), hex(YEL));
            } else if (ok) {
                fill(g, circle(cx, cy, 21.f), hex(0x1b1b1b));
                fill(g, circle(cx, cy, 19.f), hex(0xf6f2e9));
            } else {
                shadow(g, body, rgba(40, 30, 10, 0.3f), 0.f, 4.f, 6.f);
                shadow(g, body, B(0.35f), 0.f, 2.f, 1.f);
                fill(g, circle(cx, cy + 1.f, 16.f), W(0.6f));
            }
            fill(g, body, hex(rc == 'o' ? RED : BLU));
            fill(g, body, radial(cx - 16.f + 10.88f, cy - 16.f + 8.96f, 31.2f, 31.2f, {{0.f, W(0.5f)}, {0.42f, W(0.f)}}));
            fill(g, body, radial(cx, cy, 22.63f, 22.63f, {{0.6f, B(0.f)}, {1.f, B(0.35f)}}));
            // nut
            shadow(g, circle(cx, cy, 11.f), B(0.45f), 0.f, 1.f, 1.f);
            g.drawImage(nutImage(), Rectangle<float>(cx - 11.f, cy - 11.f, 22.f, 22.f));
            g.setColour(B(0.35f));
            g.drawEllipse(cx - 10.5f, cy - 10.5f, 21.f, 21.f, 1.f);
            fill(g, circle(cx, cy, 7.5f), hex(0x4a4a47));
            fill(g, circle(cx, cy, 6.5f), hex(0xb9b9b5));
            fill(g, circle(cx, cy, 5.f), hex(0x050505));
            // label: symbol + name
            const JackDef& jd = rc == 'o' ? OUTS[i] : INS[i];
            const juce::String name = juce::String::fromUTF8(jd.n);
            const float tw = cssWidth(600, false, 9.f, 0.08f, name);
            const float sw = jd.sym == 'p' ? 10.f : jd.sym == 'a' ? 7.f : 8.f;
            const float x0 = cx - (sw + 5.f + tw) * 0.5f;
            const float ly = cy + 29.f;
            const Colour ink = hex(0x1b1b1b);
            switch (jd.sym) {
            case 'p': {
                Path t;
                t.addTriangle(x0, ly + 4.f, x0 + 10.f, ly + 4.f, x0 + 5.f, ly - 4.f);
                fill(g, t, ink);
                break;
            }
            case 'g':
                g.setColour(ink);
                g.fillRect(x0, ly - 4.f, 8.f, 8.f);
                break;
            case 'm':
                g.setColour(ink);
                g.drawEllipse(x0 + 1.f, ly - 3.f, 6.f, 6.f, 2.f);
                break;
            case 'a': {
                Path d;
                d.addRectangle(-3.5f, -3.5f, 7.f, 7.f);
                d.applyTransform(AffineTransform::rotation(kPi / 4.f).translated(x0 + 3.5f, ly));
                fill(g, d, ink);
                break;
            }
            case 'c': {
                Path d;
                d.addRectangle(-3.f, -3.f, 6.f, 6.f);
                g.setColour(ink);
                g.strokePath(d, juce::PathStrokeType(2.f), AffineTransform::rotation(kPi / 4.f).translated(x0 + 4.f, ly));
                break;
            }
            default: break;
            }
            text(g, name, jost(600, 9.f, 0.08f), hex(0x2a2925), {x0 + sw + 5.f, ly - 7.f, tw + 4.f, 14.f},
                 Justification::centredLeft);
        }
    }
}

void LiftPanel::paintBrand(Graphics& g) {
    const float cy = 210.f;
    const Colour ink = hex(0x1b1b1b);
    const juce::Font small = jost(600, 11.f, 0.2f);
    auto ring = [&](float x, juce::uint32 c) {
        g.setColour(hex(c));
        g.drawEllipse(x + 2.f, cy - 4.5f, 9.f, 9.f, 4.f);
    };
    float x = 44.f;
    ring(x, RED);
    x += 13.f + 10.f;
    text(g, "OUT", small, ink, {x, cy - 8.f, 60.f, 16.f}, Justification::centredLeft);
    x += cssWidth(600, false, 11.f, 0.2f, "OUT") + 10.f + 10.f;
    ring(x, BLU);
    x += 13.f + 10.f;
    text(g, "IN", small, ink, {x, cy - 8.f, 60.f, 16.f}, Justification::centredLeft);

    const float liftW = cssWidth(700, false, 34.f, 0.3f, "LIFT") - 0.3f * 34.f;
    const float total = 22.f + 12.f + liftW + 12.f + 19.f;
    float lx = 680.f - total * 0.5f;
    Path tri;
    tri.addTriangle(lx, cy + 9.5f, lx + 22.f, cy + 9.5f, lx + 11.f, cy - 9.5f);
    fill(g, tri, hex(BLU));
    lx += 34.f;
    text(g, "LIFT", jost(700, 34.f, 0.3f), ink, {lx, cy - 17.f, liftW + 20.f, 34.f}, Justification::centredLeft);
    lx += liftW + 12.f;
    fill(g, circle(lx + 9.5f, cy, 9.5f), hex(RED));

    const juce::String key = juce::String::fromUTF8("PITCH \xc2\xb7 GATE \xc2\xb7 MOD \xc2\xb7 AUDIO \xc2\xb7 CLOCK");
    const float kw = cssWidth(600, false, 11.f, 0.2f, key);
    text(g, key, small, ink, {1316.f - kw, cy - 8.f, kw + 10.f, 16.f}, Justification::centredLeft);
}

void LiftPanel::paintScreen(Graphics& g) {
    const Path wrap = rrect({44.f, 240.f, 632.f, 438.f}, 10.f);
    fill(g, wrap, hex(0xd9cdb1));
    inset(g, wrap, rgba(60, 50, 30, 0.28f), 0.f, 2.f, 4.f);
    inset(g, wrap, W(0.7f), 0.f, -1.f, 0.f);

    const Rectangle<float> S(60.f, 256.f, 600.f, 406.f);
    const Path scr = rrect(S, 4.f);
    fill(g, rrect(S.expanded(2.f), 6.f), hex(0x121212));
    fill(g, scr, hex(0x0b0b0b));
    {
        const float dx = std::sin(122.f * kPi / 180.f), dy = -std::cos(122.f * kPi / 180.f);
        const float len = 600.f * std::abs(dx) + 406.f * std::abs(dy);
        const float cx = S.getCentreX(), cy = S.getCentreY();
        fill(g, scr,
             linear(cx - dx * len * 0.5f, cy - dy * len * 0.5f, cx + dx * len * 0.5f, cy + dy * len * 0.5f,
                    {{0.f, W(0.075f)}, {0.34f, W(0.02f)}, {0.345f, W(0.f)}, {1.f, W(0.f)}}));
    }
    inset(g, scr, B(0.85f), 0.f, 0.f, 50.f);
    inset(g, scr, B(0.9f), 0.f, 2.f, 6.f);

    Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(scr);
    g.addTransform(AffineTransform::translation(S.getX(), S.getY()));
    paintStatus(g);
    paintView(g);
    paintFoot(g);
}

void LiftPanel::paintStatus(Graphics& g) {
    const juce::Font reg = mono(false, 11.f, 0.06f);
    const juce::Font bold = mono(true, 11.f, 0.06f);
    const Rectangle<float> row(0.f, 0.f, 600.f, 32.f);
    static const char* labelsM[5] = {"SYNTH", "DRUM", "TAPE", "MIX", "IN"};
    const juce::String label = bay_ ? "BAY" : labelsM[mode_];
    juce::String sub;
    if (bay_) {
        sub = "PATCH";
    } else if (mode_ == Synth) {
        sub = ENGINES[sel_[Synth]].n;
    } else if (mode_ == Drum) {
        sub = KITS[sel_[Drum]];
    } else if (mode_ == Tape) {
        sub = "DECK";
    } else if (mode_ == Mix) {
        sub = "T" + juce::String(arm_ + 1);
    } else {
        sub = INPUTS[sel_[In]];
    }
    float x = 16.f;
    text(g, label, bold, bay_ ? hex(0x4c82e6) : hex(0xf4be2a), row.withX(x), Justification::centredLeft);
    x += cssWidth(700, true, 11.f, 0.06f, label) + 10.f;
    text(g, sub, reg, hex(0x8c877b), row.withX(x), Justification::centredLeft);

    juce::String noteText;
    if (note_ >= 0) {
        noteText = juce::String(NAMES[note_ % 12]) + juce::String(3 + oct_ + note_ / 12);
    }
    juce::String pickText;
    if (pick_.valid()) {
        pickText = pick_.r == 'o' ? juce::String(OUTS[pick_.i].n) + juce::String::fromUTF8(" \xe2\x86\x92 PICK AN IN")
                                  : juce::String::fromUTF8("PICK AN OUT \xe2\x86\x92 ") + INS[pick_.i].n;
    }
    juce::String centre = pickText.isNotEmpty() ? pickText
                          : msg_.isNotEmpty()   ? msg_
                          : noteText.isNotEmpty() ? "NOTE " + noteText
                                                  : counterText();
    const Colour cc = (pick_.valid() || msg_.isNotEmpty()) ? hex(0xf4be2a)
                      : (rec_ && playing_)                ? hex(0xe8473a)
                                                          : hex(0xede6d6);
    const float cw = cssWidth(400, true, 11.f, 0.06f, centre);
    text(g, centre, reg, cc, {300.f - cw * 0.5f, 0.f, cw + 10.f, 32.f}, Justification::centredLeft);

    struct Item {
        juce::String s;
        Colour c;
    };
    std::vector<Item> items = {{"120", hex(0x8c877b)},
                               {juce::String(speedOf(), 2) + juce::String::fromUTF8("\xc3\x97"), hex(0xede6d6)},
                               {"T" + juce::String(arm_ + 1), hex(SCR[arm_])}};
    if (rev_) {
        items.push_back({"REV", hex(0xf4be2a)});
    }
    if (shift_) {
        items.push_back({"SHIFT", hex(0xede6d6)});
    }
    float rx = 584.f;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const float w = cssWidth(400, true, 11.f, 0.06f, it->s);
        text(g, it->s, reg, it->c, {rx - w, 0.f, w + 10.f, 32.f}, Justification::centredLeft);
        rx -= w + 12.f;
    }
}

void LiftPanel::paintView(Graphics& g) {
    Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(Rectangle<int>(0, 32, 600, 324));
    const float sc = 600.f / 720.f;
    g.addTransform(AffineTransform::scale(sc).translated(0.f, 32.f + (324.f - 319.f * sc) * 0.5f));
    if (bay_) {
        paintViewBay(g);
    } else if (mode_ == Tape) {
        paintViewTape(g);
    } else if (mode_ == Synth) {
        paintViewSynth(g);
    } else if (mode_ == Drum) {
        paintViewDrum(g);
    } else if (mode_ == Mix) {
        paintViewMix(g);
    } else {
        paintViewIn(g);
    }
}

void LiftPanel::paintViewTape(Graphics& g) {
    const Colour cr = hex(0xede6d6);
    Path p;
    p.startNewSubPath(200.f, 218.f);
    p.lineTo(300.f, 262.f);
    p.lineTo(420.f, 262.f);
    p.lineTo(520.f, 218.f);
    g.setColour(cr);
    g.strokePath(p, juce::PathStrokeType(6.f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    fill(g, circle(300.f, 262.f, 13.f), cr);
    fill(g, circle(420.f, 262.f, 13.f), cr);
    g.setColour(rec_ ? hex(0xe8473a) : cr);
    g.fillRect(348.f, 246.f, 24.f, 24.f);
    Path tri;
    tri.addTriangle(360.f, 214.f, 374.f, 236.f, 346.f, 236.f);
    fill(g, tri, hex(0xf4be2a));
    auto reel = [&](float x, float pack) {
        fill(g, circle(x, 130.f, pack), hex(0x2a2925));
        const AffineTransform rot = AffineTransform::rotation(reelAngle_).translated(x, 130.f);
        g.setColour(cr);
        Path ring;
        ring.addEllipse(-80.f, -80.f, 160.f, 160.f);
        g.strokePath(ring, juce::PathStrokeType(9.f), rot);
        for (int k = 0; k < 3; ++k) {
            Path sp;
            sp.addRectangle(-6.f, -78.f, 12.f, 62.f);
            g.fillPath(sp, AffineTransform::rotation(static_cast<float>(k) * 2.f * kPi / 3.f).followedBy(rot));
        }
        fill(g, circle(x, 130.f, 20.f), cr);
        fill(g, circle(x, 130.f, 7.f), hex(0x0b0b0b));
    };
    reel(200.f, 66.f);
    reel(520.f, 46.f);
    svgText(g, "TAPE", 40.f, 300.f, 13.f, cr, -1, false, 1.f);
    line(g, 200.f, 296.f, 620.f, 296.f, hex(0x3a3833), 3.f);
    const int frames = proc_.uiFrames.load();
    const float frac = frames > 0 ? juce::jlimit(0.f, 1.f, static_cast<float>(proc_.uiPos.load() / frames)) : 0.f;
    if (frac > 0.f) {
        line(g, 200.f, 296.f, 200.f + 420.f * frac, 296.f, cr, 3.f);
    }
    svgText(g, "0" + juce::String(sel_[Tape] + 1), 680.f, 300.f, 13.f, cr, 1);
}

void LiftPanel::paintViewSynth(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 40.f));
    const EngineDef& e = ENGINES[sel_[Synth]];
    const auto& k = enc_[Synth];
    svgText(g, e.n, 24.f, 38.f, 30.f, hex(0xede6d6), -1, true);
    svgText(g, juce::String::fromUTF8(e.d), 24.f, 58.f, 10.f, hex(0x8c877b), -1, false, 1.f);
    line(g, 24.f, 150.f, 696.f, 150.f, hex(0x262626), 1.f);
    auto f = [&](double t) -> double {
        const double tau = 2.0 * juce::MathConstants<double>::pi;
        switch (sel_[Synth]) {
        case 0: {
            const double p = std::fmod(t * 3.0, 1.0);
            const double p2 = std::fmod(t * 3.0 * 1.02 + 0.13, 1.0);
            return 0.5 * (2.0 * p - 1.0) + 0.38 * (p2 < 0.5 ? 1.0 : -1.0);
        }
        case 1: {
            const double p = std::fmod(t * 3.0, 1.0);
            const double d = 0.08 + 0.4 * (1.0 - k[0]);
            const double q = p < d ? p * 0.5 / d : 0.5 + (p - d) * 0.5 / (1.0 - d);
            return std::cos(tau * q) * 0.9;
        }
        case 2: {
            double x = (1.0 + 2.5 * k[0]) * std::sin(tau * 3.0 * t);
            int n = 0;
            while ((x > 1.0 || x < -1.0) && n < 12) {
                x = x > 1.0 ? 2.0 - x : -2.0 - x;
                ++n;
            }
            return x * 0.9;
        }
        case 3: return std::sin(tau * 3.0 * t + (0.5 + 3.0 * k[2]) * std::sin(tau * 9.0 * t)) * 0.9;
        case 4: {
            double v = 0.0;
            for (int j = 1; j < 8; ++j) {
                v += std::sin(tau * j * 7.0 * t) / j;
            }
            return v * 0.62 * std::exp(-3.0 * t);
        }
        case 5: {
            double v = 0.0;
            for (int j = 0; j < 5; ++j) {
                v += std::sin(tau * t * 8.0 * (1.0 + j * 0.035));
            }
            return v / 5.0;
        }
        case 6: return std::exp(-4.0 * std::fmod(t * 3.0, 1.0)) * std::sin(t * 620.0) * std::sin(t * 91.3 + 1.0);
        default: return 0.0;
        }
    };
    Path w;
    for (int i = 0; i <= 260; ++i) {
        const double t = i / 260.0;
        const float x = static_cast<float>(24.0 + t * 672.0);
        const float y = static_cast<float>(150.0 - juce::jlimit(-1.0, 1.0, f(t)) * 70.0);
        if (i == 0) {
            w.startNewSubPath(x, y);
        } else {
            w.lineTo(x, y);
        }
    }
    g.setColour(hex(0xf4be2a));
    g.strokePath(w, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    for (int i = 0; i < 6; ++i) {
        g.setColour(i < (note_ >= 0 ? 3 : 2) ? hex(0xf4be2a) : hex(0x262626));
        g.fillRect(546.f + static_cast<float>(i) * 25.f, 26.f, 22.f, 12.f);
    }
}

void LiftPanel::paintViewDrum(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 40.f));
    svgText(g, "KIT " + juce::String(sel_[Drum] + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + (sel_[Drum] == 0 ? "TAP" : "EMPTY"),
            24.f, 30.f, 18.f, hex(0xede6d6), -1, true);
    for (int i = 0; i < 112; ++i) {
        const double hv = (0.12 + 0.88 * std::exp(-(i % 7) / 2.1)) * (0.45 + 0.55 * std::abs(std::sin(i * 1.7))) * 62.0;
        const float h = static_cast<float>(juce::jmax(2, static_cast<int>(std::floor(hv + 0.5))));
        g.setColour(i % 7 == 0 ? hex(0xede6d6) : hex(0x4c82e6));
        g.fillRect(24.f + static_cast<float>(i) * 6.f, 77.f - h / 2.f, 3.f, h);
    }
    for (int i = 1; i < 24; ++i) {
        line(g, 24.f + i * 28.f, 42.f, 24.f + i * 28.f, 112.f, hex(0xf4be2a, 0.45f), 1.f);
    }
    const int pat[16] = {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0};
    const float vel[16] = {1, 0, 0, 0, 0.7f, 0, 0, 0.4f, 1, 0, 0, 0, 0.7f, 0, 0.5f, 0};
    for (int i = 0; i < 16; ++i) {
        const float x = 24.f + i * 42.4f;
        g.setColour(pat[i] ? rgba(232, 71, 58, 0.3f) : hex(0x141414));
        g.fillRect(x, 142.f, 36.f, 36.f);
        g.setColour(pat[i] ? hex(0xe8473a) : (i % 4 == 0 ? hex(0x444444) : hex(0x262626)));
        g.drawRect(x - 0.5f, 141.5f, 37.f, 37.f, 1.f);
        if (pat[i]) {
            g.setColour(hex(0xe8473a));
            g.fillRect(x, 184.f, 36.f, static_cast<float>(juce::jmax(2, juce::roundToInt(vel[i] * 12.f))));
        }
    }
}

void LiftPanel::paintViewMix(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 52.f));
    const float lvls[4] = {0.72f, 0.55f, 0.8f, 0.4f}, pans[4] = {0.5f, 0.3f, 0.62f, 0.8f};
    const auto& k = enc_[Mix];
    for (int t = 0; t < 4; ++t) {
        const float x = 60.f + t * 110.f;
        const float v = arm_ == t ? k[0] : lvls[t];
        const float p = arm_ == t ? k[1] : pans[t];
        const float cy = static_cast<float>(juce::roundToInt(176.f - v * 126.f));
        svgText(g, "T" + juce::String(t + 1), x, 22.f, 12.f, hex(SCR[t]), 0, true);
        line(g, x - 18.f, 34.f, x + 18.f, 34.f, hex(0x333333), 3.f);
        fill(g, circle(x - 18.f + p * 36.f, 34.f, 4.f), hex(SCR[t]));
        line(g, x, 50.f, x, 176.f, hex(0x333333), 4.f);
        line(g, x, cy, x, 176.f, hex(SCR[t]), 4.f);
        g.setColour(hex(0xede6d6));
        g.fillRect(x - 18.f, cy - 5.f, 36.f, 10.f);
        if (mutes_[static_cast<size_t>(t)]) {
            g.setColour(hex(0xf4be2a));
            g.fillRect(x - 12.f, 186.f, 24.f, 18.f);
        }
        g.setColour(hex(0x444444));
        g.drawRect(x - 12.5f, 185.5f, 25.f, 19.f, 1.f);
        svgText(g, "M", x, 199.f, 9.f, mutes_[static_cast<size_t>(t)] ? hex(0x0a0a0a) : hex(0x8c877b), 0);
    }
    svgText(g, "MST", 500.f, 22.f, 12.f, hex(0xede6d6), 0, true);
    line(g, 500.f, 50.f, 500.f, 176.f, hex(0x333333), 4.f);
    line(g, 500.f, 74.f, 500.f, 176.f, hex(0xede6d6), 4.f);
    g.setColour(hex(0xede6d6));
    g.fillRect(482.f, 69.f, 36.f, 10.f);
    g.setColour(hex(0x262626));
    g.drawRect(559.5f, 33.5f, 137.f, 111.f, 1.f);
    Path eq;
    eq.startNewSubPath(560.f, 90.f - (k[2] - 0.5f) * 40.f);
    eq.cubicTo(584.f, 88.f - (k[2] - 0.5f) * 40.f, 596.f, 80.f, 610.f, 89.f);
    eq.cubicTo(628.f, 100.f, 646.f, 98.f, 662.f, 86.f);
    eq.cubicTo(674.f, 78.f, 686.f, 90.f - (k[3] - 0.5f) * 40.f, 696.f, 90.f - (k[3] - 0.5f) * 44.f);
    g.setColour(hex(0xf4be2a));
    g.strokePath(eq, juce::PathStrokeType(2.f));
    svgText(g, juce::String::fromUTF8("LOW \xc2\xb7 MID \xc2\xb7 HIGH"), 628.f, 166.f, 9.f, hex(0x8c877b), 0, false, 1.f);
}

void LiftPanel::paintViewIn(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 46.f));
    const auto& k = enc_[In];
    const int st = juce::jmin(4, static_cast<int>(std::floor(k[0] * 5.f)));
    const juce::String name = sel_[In] == 2
                                  ? juce::String::fromUTF8("RADIO \xc2\xb7 STATION ") + juce::String(st + 1).paddedLeft('0', 2)
                                  : juce::String::fromUTF8("INPUT \xc2\xb7 ") + INPUTS[sel_[In]];
    svgText(g, name, 24.f, 30.f, 18.f, hex(0xede6d6), -1, true);
    g.setColour(hex(0x121212));
    g.fillRect(24.f, 48.f, 672.f, 78.f);
    g.setColour(hex(0x262626));
    g.drawRect(23.5f, 47.5f, 673.f, 79.f, 1.f);
    for (int i = 0; i <= 41; ++i) {
        line(g, 40.f + i * 16.f, i % 5 == 0 ? 82.f : 92.f, 40.f + i * 16.f, 104.f, hex(0x4a4740), 1.f);
    }
    for (int i = 0; i < 5; ++i) {
        svgText(g, "STATION " + juce::String(i + 1).paddedLeft('0', 2), 104.f + i * 128.f, 72.f, 10.f,
                i == st ? hex(0xede6d6) : hex(0x5e5a50), 0);
    }
    const float nx = static_cast<float>(juce::roundToInt(40.f + k[0] * 640.f));
    line(g, nx, 54.f, nx, 120.f, hex(0xe8473a), 3.f);
    g.setColour(hex(0x1a1a1a));
    g.fillRect(64.f, 146.f, 560.f, 12.f);
    g.fillRect(64.f, 170.f, 560.f, 12.f);
    g.setColour(hex(0x4c82e6));
    g.fillRect(64.f, 146.f, static_cast<float>(juce::roundToInt(560.f * k[2] * 0.92f)), 12.f);
    g.fillRect(64.f, 170.f, static_cast<float>(juce::roundToInt(560.f * k[2] * 0.84f)), 12.f);
    const float tx = static_cast<float>(juce::roundToInt(64.f + k[3] * 560.f));
    {
        Path l;
        l.startNewSubPath(tx, 140.f);
        l.lineTo(tx, 188.f);
        Path d;
        const float dash[2] = {3.f, 2.f};
        juce::PathStrokeType(2.f).createDashedStroke(d, l, dash, 2);
        fill(g, d, hex(0xe8473a));
    }
    svgText(g, "L", 24.f, 156.f, 9.f, hex(0x8c877b));
    svgText(g, "R", 24.f, 180.f, 9.f, hex(0x8c877b));
}

void LiftPanel::paintViewBay(Graphics& g) {
    svgText(g, juce::String::fromUTF8("CORDS ") + juce::String(static_cast<int>(cords_.size())) +
                   juce::String::fromUTF8(" \xc2\xb7 INPUTS SUM"),
            24.f, 28.f, 11.f, hex(0x8c877b), -1, false, 1.f);
    svgText(g, "32-SAMPLE BLOCK", 696.f, 28.f, 11.f, hex(0x8c877b), 1, false, 1.f);
    const int n = juce::jmin(9, static_cast<int>(cords_.size()));
    for (int k = 0; k < n; ++k) {
        const Cord& c = cords_[static_cast<size_t>(k)];
        const float y = 48.f + k * 28.f;
        g.setColour(hex(CLOTH[c.c].c));
        g.fillRect(24.f, y, 14.f, 14.f);
        g.setColour(hex(0xede6d6, 0.3f));
        g.drawRect(23.5f, y - 0.5f, 15.f, 15.f, 1.f);
        svgText(g, juce::String::fromUTF8(OUTS[c.o].n), 52.f, y + 12.f, 14.f, hex(0xede6d6));
        svgText(g, juce::String::fromUTF8("\xe2\x86\x92"), 290.f, y + 12.f, 14.f, hex(0x5e5a50));
        svgText(g, juce::String::fromUTF8(INS[c.i].n), 330.f, y + 12.f, 14.f, hex(0xede6d6));
        if (isFb(c.o, c.i)) {
            svgText(g, juce::String::fromUTF8("FEEDBACK z\xe2\x81\xbb\xc2\xb9"), 600.f, y + 12.f, 11.f, hex(0xf4be2a), 1);
        }
        svgText(g, juce::String::fromUTF8(AMTS[k % 8]), 696.f, y + 12.f, 14.f, hex(0x8c877b), 1);
    }
    if (cords_.empty()) {
        svgText(g, juce::String::fromUTF8("NO CORDS \xc2\xb7 DRAG FROM AN OUT TO AN IN"), 24.f, 70.f, 14.f, hex(0x8c877b));
    }
    const juce::String foot =
        (cords_.size() > 9 ? "+" + juce::String(static_cast<int>(cords_.size()) - 9) + juce::String::fromUTF8(" MORE \xc2\xb7 ")
                           : juce::String()) +
        "SOFT CLIP + DC BLOCK ON EVERY LOOP";
    svgText(g, foot, 24.f, 308.f, 10.f, hex(0x5e5a50), -1, false, 1.f);
}

void LiftPanel::paintFoot(Graphics& g) {
    const auto L = labels();
    const float cw = (568.f - 42.f) / 4.f;
    for (int i = 0; i < 4; ++i) {
        const float x = 16.f + static_cast<float>(i) * (cw + 14.f);
        const juce::Font f = mono(false, 9.f, 0.06f);
        text(g, L[i], f, hex(SCR[i]), {x, 364.f, cw, 13.5f}, Justification::centredLeft);
        const juce::String v = encDisplay(i);
        const float vw = cssWidth(400, true, 9.f, 0.06f, v);
        text(g, v, f, hex(0xede6d6), {x + cw - vw, 364.f, vw + 6.f, 13.5f}, Justification::centredLeft);
        g.setColour(hex(0x1c1c1c));
        g.fillRect(x, 383.5f, cw, 6.f);
        g.setColour(hex(SCR[i]));
        g.fillRect(x, 383.5f, cw * static_cast<float>(juce::roundToInt(enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)] * 100.f)) / 100.f, 6.f);
    }
}

// ------------------------------------------------------------------ knobs

void LiftPanel::paintKnobs(Graphics& g) {
    if (!g.clipRegionIntersects(Rectangle<int>(684, 240, 632, 198))) {
        return;
    }
    // BODY: M70 18 C78 18 88 38 96 51.4 A32 32 0 1 1 44 51.4 C52 38 62 18 70 18 Z
    Path body;
    body.startNewSubPath(70.f, 18.f);
    body.cubicTo(78.f, 18.f, 88.f, 38.f, 96.f, 51.4f);
    const float a0 = std::atan2(26.f, 70.05f - 51.4f);
    body.addCentredArc(70.f, 70.05f, 32.f, 32.f, 0.f, a0, 2.f * kPi - a0, false);
    body.cubicTo(52.f, 38.f, 62.f, 18.f, 70.f, 18.f);
    body.closeSubPath();

    static const char* glyphNames = nullptr;
    juce::ignoreUnused(glyphNames);
    const auto L = labels();
    for (int i = 0; i < 4; ++i) {
        const float v = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
        const float ox = 684.f + static_cast<float>(i) * 160.f + 6.f, oy = 255.75f;
        Graphics::ScopedSaveState s(g);
        g.addTransform(AffineTransform::translation(ox, oy));
        // printed tick ring
        for (int t = 0; t <= 10; ++t) {
            const float h = t % 5 == 0 ? 10.f : 7.f;
            Path tk;
            tk.addRectangle(-1.5f, -67.f, 3.f, h);
            g.setColour(static_cast<float>(t) / 10.f <= v + 0.001f ? hex(BLK) : hex(0xb9ab8d));
            g.fillPath(tk, AffineTransform::rotation((-135.f + t * 27.f) * kPi / 180.f).translated(70.f, 70.f));
        }
        const float deg = -135.f + v * 270.f;
        const float rad = deg * kPi / 180.f;
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
        const AffineTransform rot = AffineTransform::rotation(rad, 70.f, 70.f);
        Path rb(body);
        rb.applyTransform(rot);
        Path contact(rb);
        contact.applyTransform(AffineTransform::translation(2.f, 3.f));
        juce::DropShadow(B(0.45f), 4, {}).drawForPath(g, contact);
        fill(g, rb, kc);
        // gloss and rim stay fixed to the light (gradientTransform counter-rotates)
        fill(g, rb, radial(56.f, 50.f, 46.f, 46.f, {{0.f, W(0.55f)}, {0.3f, W(0.12f)}, {0.7f, W(0.f)}, {1.f, B(0.25f)}}));
        fill(g, rb, linear(38.f, 30.f, 102.f, 110.f, {{0.f, W(0.38f)}, {0.45f, W(0.f)}, {1.f, B(0.25f)}}));
        Path inlay;
        inlay.addRoundedRectangle(68.4f, 22.f, 3.2f, 26.f, 1.2f);
        inlay.applyTransform(rot);
        fill(g, inlay, i == 1 ? hex(0x151515) : hex(0xf7f7f4));
        g.setColour(i == 1 ? hex(0xf7f7f4) : B(0.5f));
        g.strokePath(inlay, juce::PathStrokeType(0.6f));
        // spun aluminium cap
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
    // labels under the knobs
    for (int i = 0; i < 4; ++i) {
        const float cx = 684.f + static_cast<float>(i) * 160.f + 76.f, cy = 405.75f + 8.25f;
        const float tw = cssWidth(600, false, 11.f, 0.12f, L[i]);
        const float gw = i == 2 || i == 3 ? 12.f : 11.f;
        const float x0 = cx - (gw + 7.f + tw) * 0.5f;
        switch (i) {
        case 0: fill(g, circle(x0 + 5.5f, cy, 5.5f), hex(RED)); break;
        case 1:
            g.setColour(hex(YEL));
            g.fillRect(x0, cy - 5.5f, 11.f, 11.f);
            break;
        case 2: {
            Path t;
            t.addTriangle(x0, cy + 5.5f, x0 + 12.f, cy + 5.5f, x0 + 6.f, cy - 5.5f);
            fill(g, t, hex(BLU));
            break;
        }
        default: {
            Path h;
            h.addPieSegment(x0, cy - 3.f, 12.f, 12.f, -kPi * 0.5f, kPi * 0.5f, 0.f);
            fill(g, h, hex(BLK));
            break;
        }
        }
        text(g, L[i], jost(600, 11.f, 0.12f), hex(0x1b1b1b), {x0 + gw + 7.f, cy - 8.25f, tw + 8.f, 16.5f},
             Justification::centredLeft);
    }
}

// ------------------------------------------------------------------ pads

namespace {

void drawPad(Graphics& g, Rectangle<float> r, juce::uint32 fillCol, bool lit, Colour ink, const juce::String& big,
             float bigSize, const juce::String& label) {
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
    if (big.isNotEmpty()) {
        text(g, big, jost(bigSize > 20.f ? 500 : 600, bigSize), ink, {r.getX() + 9.f, r.getY() + 8.f, 60.f, bigSize},
             Justification::centredLeft);
    }
    text(g, label, jost(600, 10.f, 0.1f), ink, {r.getX() + 9.f, r.getBottom() - 8.f - 12.f, 62.f, 12.f},
         Justification::centredLeft);
}

}  // namespace

void LiftPanel::paintPads(Graphics& g) {
    if (!g.clipRegionIntersects(Rectangle<int>(680, 440, 640, 245))) {
        return;
    }
    const char* modeNames[5] = {"SYNTH", "DRUM", "TAPE", "MIX", "IN"};
    for (int k = 0; k < 24; ++k) {
        Rectangle<float> r = padRect(k);
        if (pressedPad_ == k) {
            r = r.translated(0.f, 2.f);
        }
        juce::uint32 col = 0;
        bool lit = false;
        Colour ink = hex(BLK);
        juce::String big, label;
        float bigSize = 0.f;
        bool unused = false;
        if (k < 5) {
            lit = mode_ == k && !bay_;
            col = YEL;
            label = modeNames[k];
        } else if (k == 5) {
            lit = fx_;
            col = BLK;
            ink = lit ? hex(CRM) : hex(BLK);
            label = "FX";
        } else if (k == 6) {
            lit = bay_;
            col = BLU;
            ink = lit ? hex(CRM) : hex(BLK);
            label = "BAY";
        } else if (k == 7) {
            lit = mode_ == In && sel_[In] == 2 && !bay_;
            col = RED;
            ink = lit ? hex(CRM) : hex(BLK);
            label = "RADIO";
        } else if (k < 16) {
            const int i = k - 8;
            unused = mode_ == Mix || (mode_ == In && INPUTS[i][0] == 0);
            lit = !unused && sel_[static_cast<size_t>(mode_)] == i;
            col = RED;
            ink = lit ? hex(CRM) : hex(BLK);
            big = juce::String(i + 1);
            bigSize = 24.f;
            juce::String sub = mode_ == Synth ? juce::String(ENGINES[i].n)
                               : mode_ == Drum ? juce::String(KITS[i])
                               : mode_ == Tape ? juce::String("TAPE")
                               : mode_ == In   ? juce::String(INPUTS[i])
                                               : juce::String();
            label = sub.isNotEmpty() ? sub : juce::String::fromUTF8("\xc2\xb7");
        } else {
            const int t = (k - 16) % 4;
            const bool arm = k < 20;
            lit = arm ? arm_ == t : mutes_[static_cast<size_t>(t)];
            col = HW[t];
            ink = lit ? (t == 1 ? hex(BLK) : hex(CRM)) : hex(BLK);
            big = (arm ? "T" : "M") + juce::String(t + 1);
            bigSize = 18.f;
            label = arm ? "ARM" : "MUTE";
        }
        if (unused) {
            g.beginTransparencyLayer(0.45f);
        }
        drawPad(g, r, col, lit, ink, big, bigSize, label);
        if (unused) {
            g.endTransparencyLayer();
        }
    }
}

// ------------------------------------------------------------------ membrane

void LiftPanel::paintMembrane(Graphics& g) {
    if (!g.clipRegionIntersects(Rectangle<int>(40, 694, 640, 160))) {
        return;
    }
    const Path frame = rrect({44.f, 698.f, 632.f, 150.f}, 10.f);
    fill(g, frame, hex(0xd4c7a9));
    inset(g, frame, rgba(60, 50, 30, 0.25f), 0.f, 2.f, 4.f);
    inset(g, frame, hex(0xffffff), 0.f, -1.f, 0.f);
    const Rectangle<float> M(52.f, 706.f, 616.f, 134.f);
    const Path mem = rrect(M, 6.f);
    shadow(g, mem, B(0.4f), 0.f, 2.f, 3.f);
    shadow(g, mem, W(0.5f), 0.f, 1.f, 0.f);
    fill(g, mem, hex(0x131313));
    g.setColour(hex(0xe6e6e2));
    g.drawRoundedRectangle(M.reduced(7.75f), 3.25f, 1.5f);
    const char* labels[10] = {"LIFT", "LOOP", "SHIFT", "REV", "DROP", "REC", "OCT", "PLAY", "OCT", "STOP"};
    const juce::Font lf = jost(500, 13.f, 0.06f);
    for (int k = 0; k < 10; ++k) {
        Rectangle<float> r = memRect(k);
        const bool red = k == 5 || k == 9;
        const bool down = pressedMem_ == k;
        Graphics::ScopedSaveState s(g);
        if (down) {
            g.addTransform(AffineTransform::scale(0.97f, 0.97f, r.getCentreX(), r.getCentreY()));
        }
        fill(g, rrect(r.expanded(4.f), 9.f), hex(0xe6e6e2));
        fill(g, rrect(r.expanded(2.5f), 7.5f), hex(0x131313));
        const Path key = rrect(r, 5.f);
        fill(g, key, down ? (red ? hex(0xd23434) : hex(0x3cb1dc)) : (red ? hex(0xea3e3e) : hex(0x4ec4ec)));
        if (down) {
            inset(g, key, B(0.3f), 0.f, 1.f, 3.f);
        }
        const float gw = (k == 2 || k == 9) ? 12.f : (k == 3 ? 11.f : (k == 7 ? 12.f : 14.f));
        const float tw = cssWidth(500, false, 13.f, 0.06f, labels[k]);
        const float x0 = r.getCentreX() - (gw + 9.f + tw) * 0.5f;
        const float cy = r.getCentreY();
        const Colour wh = juce::Colours::white;
        g.setColour(wh);
        switch (k) {
        case 0: {
            Path t;
            t.addTriangle(x0, cy + 5.5f, x0 + 14.f, cy + 5.5f, x0 + 7.f, cy - 5.5f);
            g.fillPath(t);
            break;
        }
        case 1: g.drawEllipse(x0 + 1.5f, cy - 5.5f, 11.f, 11.f, 3.f); break;
        case 2: g.drawRect(x0, cy - 6.f, 12.f, 12.f, 3.f); break;
        case 3: {
            Path t;
            t.addTriangle(x0 + 11.f, cy - 7.f, x0 + 11.f, cy + 7.f, x0, cy);
            g.fillPath(t);
            break;
        }
        case 4: {
            Path t;
            t.addTriangle(x0, cy - 5.5f, x0 + 14.f, cy - 5.5f, x0 + 7.f, cy + 5.5f);
            g.fillPath(t);
            break;
        }
        case 5: g.fillEllipse(x0, cy - 7.f, 14.f, 14.f); break;
        case 6:
        case 8:
            text(g, k == 6 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String("+"), jost(500, 20.f), wh,
                 {x0 - 10.f, cy - 10.f, 34.f, 20.f}, Justification::centred);
            break;
        case 7: {
            Path t;
            t.addTriangle(x0, cy - 7.f, x0, cy + 7.f, x0 + 12.f, cy);
            g.fillPath(t);
            break;
        }
        default: g.fillRect(x0, cy - 6.f, 12.f, 12.f); break;
        }
        text(g, labels[k], lf, wh, {x0 + gw + 9.f, cy - 10.f, tw + 10.f, 20.f}, Justification::centredLeft);
    }
}

// ------------------------------------------------------------------ keyboard

void LiftPanel::paintKeys(Graphics& g) {
    if (!g.clipRegionIntersects(Rectangle<int>(680, 694, 640, 172))) {
        return;
    }
    for (int o = 0; o < 2; ++o) {
        for (int b = 0; b < 5; ++b) {
            const int note = o * 12 + SHARP_DEF[b][0];
            const bool down = heldNote_ == note;
            juce::Point<float> c = sharpCentre(o, b);
            if (down) {
                c.y += 3.f;
            }
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
    }
    for (int j = 0; j < 14; ++j) {
        const int o = j / 7;
        const int note = o * 12 + NAT_SEMI[j % 7];
        const bool down = heldNote_ == note;
        Rectangle<float> r = natRect(j);
        if (down) {
            r = r.translated(0.f, 3.f);
        }
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
        // Only the C keys carry their octave (the prototype labelled every
        // natural after the first redraw; fixed here).
        if (j % 7 == 0) {
            text(g, "C" + juce::String(3 + o + oct_), jost(600, 10.f, 0.08f), hex(0x1b1b1b),
                 {r.getX(), r.getBottom() - 9.f - 10.f, r.getWidth() + 0.8f, 10.f}, Justification::centred);
        }
    }
}

// ------------------------------------------------------------------ cables

namespace {

Path cablePath(float x1, float y1, float x2, float y2) {
    const float sag = 60.f + std::hypot(x2 - x1, y2 - y1) * 0.22f;
    Path p;
    p.startNewSubPath(x1, y1);
    p.cubicTo(x1, y1 + sag, x2, y2 + sag, x2, y2);
    return p;
}

void drawPlug(Graphics& g, float x, float y, const Cloth& k, bool st, bool aside) {
    auto C = [&](Colour c) { return aside ? grayed(c) : c; };
    shadow(g, circle(x + 3.f, y + 6.f, 15.f), C(rgba(30, 20, 6, 0.4f)), 0.f, 0.f, 7.f);
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

void drawCable(Graphics& g, float x1, float y1, float x2, float y2, int ci, bool st, bool aside) {
    const Cloth& k = CLOTH[ci];
    auto C = [&](Colour c) { return aside ? grayed(c) : c; };
    const Path d = cablePath(x1, y1, x2, y2);
    const juce::PathStrokeType round(8.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    {
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
    drawPlug(g, x1, y1, k, st, aside);
    drawPlug(g, x2, y2, k, st, aside);
}

}  // namespace

void LiftPanel::paintCables(Graphics& g) {
    const bool moving = drag_.active && drag_.isMove && drag_.started;
    const auto lv = levels(moving ? drag_.n : -1);
    const bool aside = drag_.active && drag_.started;
    for (size_t n = 0; n < cords_.size(); ++n) {
        if (moving && static_cast<int>(n) == drag_.n) {
            continue;
        }
        const Cord& c = cords_[n];
        if (aside) {
            g.beginTransparencyLayer(0.5f);
        }
        drawCable(g, jx(c.o), jy('o') - 6.f * static_cast<float>(lv[n].o), jx(c.i), jy('i') - 6.f * static_cast<float>(lv[n].i),
                  c.c, c.st, aside);
        if (aside) {
            g.endTransparencyLayer();
        }
    }
    if (drag_.active && drag_.started) {
        const float fx = jx(drag_.fixed.i), fy = jy(drag_.fixed.r) - 6.f * static_cast<float>(drag_.flv);
        drawCable(g, fx, fy, drag_.p.x, drag_.p.y, drag_.c, drag_.st, false);
    }
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
