// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// How the panel gets onto the screen. The approach is the one RONIN
// (PatchBayView: a backdrop image at physical pixels, a frame signature per
// part and dirty-rectangle repaints) and BUSHIDO (RackPanel: the face cached
// at device resolution and only changed controls invalidated; CableLayer: the
// cables on their own transparent layer) use:
//
//  - art_: everything that never changes (the case from the SVG art, bay
//    labels, brand, screen bezel, membrane sheet), one image at device pixels
//    per window size, blitted 1:1;
//  - parts: the top bar, 32 jacks, 4 knobs, 24 pads, 10 membrane keys and 24
//    keyboard keys. Each frame every part's look is hashed; only parts whose
//    hash changed are repainted. Parts with blurred shadows come from sprites
//    rendered once at device resolution (keyed by look and sub-pixel phase);
//    each knob is one image (ticks, cached skirt, rotated body, cached cap)
//    rebuilt only when its value moves;
//  - Screen: the display. A fixed 1200 x 812 framebuffer (2x the 600 x 406
//    glass) redrawn at a steady 60 Hz while anything on it moves, blitted to
//    the glass. State changes only mark it dirty;
//  - Cables: the patch cables over everything. Settled cables are one cached
//    image; the cable in hand is drawn live with a cheap offset-stroke shadow
//    and only the union of its old and new bounds is repainted.
//
// Frames come from the display (juce::VBlankAttachment), with wall-clock time,
// so a late frame never slows an animation down.

#include "LiftPanel.h"
#include "LiftArtData.h"
#include "PanelLayers.h"
#include "LiftProcessor.h"
#include "PanelData.h"

#include <cmath>
#include <cstring>

namespace lift {

using namespace ui;
using juce::AffineTransform;
using juce::Graphics;
using juce::Rectangle;

namespace {

constexpr int kFbW = 1200, kFbH = 812;  // the display's native resolution
constexpr double kDisplayHz = 60.0;

juce::uint64 mix(juce::uint64 h, juce::uint64 v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}
juce::uint64 fbits(float f) {
    juce::uint32 u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
juce::uint64 shash(const juce::String& s) {
    return static_cast<juce::uint64>(s.hashCode64());
}

// Area-average a bitmap down to w x h (one pass per axis; exact box filter).
juce::Image boxResample(const juce::Image& src, int w, int h) {
    const juce::Image::BitmapData in(src, juce::Image::BitmapData::readOnly);
    const int sw = src.getWidth(), sh = src.getHeight();
    if (w >= sw || h >= sh) {
        return src.rescaled(w, h, Graphics::highResamplingQuality);
    }
    const double rx = static_cast<double>(sw) / w, ry = static_cast<double>(sh) / h;
    std::vector<float> tmp(static_cast<size_t>(w) * static_cast<size_t>(sh) * 3u);
    for (int y = 0; y < sh; ++y) {
        float* t = tmp.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; ++x) {
            const double x0 = x * rx, x1 = (x + 1) * rx;
            float a[3] = {0, 0, 0};
            for (int sx = static_cast<int>(x0); sx < juce::jmin(sw, static_cast<int>(std::ceil(x1))); ++sx) {
                const float cov = static_cast<float>(juce::jmin(x1, sx + 1.0) - juce::jmax(x0, static_cast<double>(sx)));
                const auto c = in.getPixelColour(sx, y);
                a[0] += cov * c.getRed();
                a[1] += cov * c.getGreen();
                a[2] += cov * c.getBlue();
            }
            for (int k = 0; k < 3; ++k) {
                t[x * 3 + k] = a[k] / static_cast<float>(rx);
            }
        }
    }
    juce::Image dst(juce::Image::ARGB, w, h, false);
    juce::Image::BitmapData out(dst, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y) {
        const double y0 = y * ry, y1 = (y + 1) * ry;
        for (int x = 0; x < w; ++x) {
            float a[3] = {0, 0, 0};
            for (int sy = static_cast<int>(y0); sy < juce::jmin(sh, static_cast<int>(std::ceil(y1))); ++sy) {
                const float cov = static_cast<float>(juce::jmin(y1, sy + 1.0) - juce::jmax(y0, static_cast<double>(sy)));
                const float* t = tmp.data() + (static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3u;
                a[0] += cov * t[0];
                a[1] += cov * t[1];
                a[2] += cov * t[2];
            }
            auto q = [&](float v) { return static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(v / static_cast<float>(ry)))); };
            out.setPixelColour(x, y, juce::Colour(q(a[0]), q(a[1]), q(a[2])));
        }
    }
    return dst;
}

}  // namespace

// ------------------------------------------------------------------ layers

void LiftPanel::initLayers() {
    screen_ = std::make_unique<Screen>(*this);
    cables_ = std::make_unique<Cables>(*this);
    addAndMakeVisible(*screen_);
    addAndMakeVisible(*cables_);
    vblank_ = std::make_unique<juce::VBlankAttachment>(this, [this](double ts) { onVBlank(ts); });
    resized();
}

Rectangle<float> LiftPanel::screenGlass() {
    return {kDevX + 60.f, kDevY + 256.f, 600.f, 406.f};
}

juce::Rectangle<int> LiftPanel::toLocal(Rectangle<float> c) const {
    return (c * scale_).getSmallestIntegerContainer().expanded(1);
}

void LiftPanel::resized() {
    scale_ = juce::jmax(0.1f, static_cast<float>(getWidth()) / static_cast<float>(kW));
    if (screen_ != nullptr) {
        const auto s = screenGlass() * scale_;
        const int x0 = juce::roundToInt(s.getX()), y0 = juce::roundToInt(s.getY());
        screen_->setBounds(x0, y0, juce::roundToInt(s.getRight()) - x0, juce::roundToInt(s.getBottom()) - y0);
        cables_->setBounds(getLocalBounds());
    }
    shown_.clear();
    cableKey_ = 0;
    juce::Component::repaint();
}

// ------------------------------------------------------------------ invalidation

void LiftPanel::repaint() {
    screenDirty_ = true;
    if (screen_ != nullptr) {
        screen_->repaint();
    }
    if (!inFrame_) {
        refreshParts();
    }
}

void LiftPanel::repaint(juce::Rectangle<int> canvasArea) {
    if (canvasArea == screenArea()) {
        screenDirty_ = true;
        if (screen_ != nullptr) {
            screen_->repaint();
        }
        return;
    }
    juce::Component::repaint(toLocal(canvasArea.toFloat()));
}

int LiftPanel::partCount() const {
    return 1 + 32 + 4 + 24 + 10 + 14 + 10;
}

Rectangle<float> LiftPanel::partRect(int p) const {
    const juce::Point<float> o(kDevX, kDevY);
    if (p == 0) {
        return {0.f, 0.f, static_cast<float>(kW), kDevY};
    }
    p -= 1;
    if (p < 32) {
        const char rc = p < 16 ? 'o' : 'i';
        const float cx = jx(p % 16), cy = jy(rc);
        return Rectangle<float>(cx - 26.f, cy - 26.f, 52.f, 56.f) + o;
    }
    p -= 32;
    if (p < 4) {
        const auto k = knobOrigin(p);
        return Rectangle<float>(k.x - 14.f, k.y - 14.f, 168.f, 170.f).getUnion({684.f + p * 160.f, 400.f, 160.f, 26.f}) + o;
    }
    p -= 4;
    if (p < 24) {
        return padRect(p).withTrimmedTop(-14.f).withTrimmedLeft(-14.f).withTrimmedRight(-14.f).withTrimmedBottom(-28.f) + o;
    }
    p -= 24;
    if (p < 10) {
        auto r = memRect(p).expanded(8.f) + o;
        if (p == kShiftSlot) {
            r = r.getUnion(shiftKeyArea().toFloat());
        }
        return r;
    }
    p -= 10;
    if (p < 14) {
        return natRect(p).withTrimmedTop(-14.f).withTrimmedLeft(-14.f).withTrimmedRight(-14.f).withTrimmedBottom(-28.f) + o;
    }
    p -= 14;
    const auto c = sharpCentre(p / 5, p % 5);
    return Rectangle<float>(c.x - 36.f, c.y - 36.f, 72.f, 84.f) + o;
}

juce::uint64 LiftPanel::partSig(int p) const {
    juce::uint64 h = 1469598103934665603ULL;
    if (p == 0) {
        h = mix(h, static_cast<juce::uint64>(color_));
        h = mix(h, stack_ ? 1 : 0);
        return mix(h, shash(info_));
    }
    p -= 1;
    if (p < 32) {
        const char rc = p < 16 ? 'o' : 'i';
        const int i = p % 16;
        const bool picked = pick_.valid() && pick_.r == rc && pick_.i == i;
        const bool ok = drag_.active && drag_.started && drag_.need == rc;
        return mix(h, (picked ? 1 : 0) | (ok ? 2 : 0));
    }
    p -= 32;
    if (p < 4) {
        h = mix(h, fbits(enc_[static_cast<size_t>(mode_)][static_cast<size_t>(p)]));
        return mix(h, shash(labels()[p]));
    }
    p -= 4;
    if (p < 24) {
        const PadLook L = padLook(p);
        h = mix(h, fbits(L.r.getY()));
        h = mix(h, L.col);
        h = mix(h, (L.lit ? 1 : 0) | (L.unused ? 2 : 0));
        h = mix(h, L.ink.getARGB());
        h = mix(h, shash(L.big));
        return mix(h, shash(L.label));
    }
    p -= 24;
    if (p < 10) {
        h = mix(h, pressedMem_ == p ? 1 : 0);
        if (p == kShiftSlot) {
            h = mix(h, fbits(shiftAmt_));
        }
        return h;
    }
    p -= 10;
    if (p < 14) {
        h = mix(h, heldNote_ == (p / 7) * 12 + NAT_SEMI[p % 7] ? 1 : 0);
        return mix(h, p % 7 == 0 ? static_cast<juce::uint64>(oct_ + 100) : 0);
    }
    p -= 14;
    return mix(h, heldNote_ == (p / 5) * 12 + SHARP_DEF[p % 5][0] ? 1 : 0);
}

void LiftPanel::refreshParts() {
    const int n = partCount();
    if (static_cast<int>(shown_.size()) != n) {
        shown_.assign(static_cast<size_t>(n), 0);
        for (int p = 0; p < n; ++p) {
            shown_[static_cast<size_t>(p)] = partSig(p);
        }
        juce::Component::repaint();
    } else {
        for (int p = 0; p < n; ++p) {
            const juce::uint64 s = partSig(p);
            if (s != shown_[static_cast<size_t>(p)]) {
                shown_[static_cast<size_t>(p)] = s;
                juce::Component::repaint(toLocal(partRect(p)));
            }
        }
    }
    refreshOverlay();
}

void LiftPanel::refreshOverlay() {
    if (cables_ == nullptr) {
        return;
    }
    const juce::Point<float> o(kDevX, kDevY);
    // settled cables: one key for the patch and how it is shown
    const bool moving = drag_.active && drag_.isMove && drag_.started;
    juce::uint64 k = mix(7, cords_.size());
    for (const Cord& c : cords_) {
        k = mix(k, static_cast<juce::uint64>(((c.o * 16 + c.i) * 8 + c.c) * 2 + (c.st ? 1 : 0)));
    }
    k = mix(k, static_cast<juce::uint64>((drag_.active && drag_.started ? 1 : 0) + (moving ? 2 + 4 * (drag_.n + 1) : 0)));
    if (k != cableKey_) {
        cableKey_ = k;
        cableImg_ = {};
        Rectangle<float> u;
        for (const Cord& c : cords_) {
            const float y1 = jy('o') - 6.f * 4.f, y2 = jy('i') - 6.f * 4.f;
            const auto b = cableBounds(jx(c.o), y1, jx(c.i), y2).getUnion(cableBounds(jx(c.o), jy('o'), jx(c.i), jy('i')));
            u = u.isEmpty() ? b : u.getUnion(b);
        }
        const auto r = toLocal(u + o).getUnion(cableShown_);
        cableShown_ = toLocal(u + o);
        if (!r.isEmpty()) {
            cables_->repaint(r);
        }
    }
    const auto live = drag_.active && drag_.started ? toLocal(liveCableBounds() + o) : juce::Rectangle<int>();
    if (live != liveShown_) {
        cables_->repaint(live.getUnion(liveShown_));
        liveShown_ = live;
    }
    juce::uint64 mk = mix(11, menu_.open ? 1 : 0);
    mk = mix(mk, static_cast<juce::uint64>(menu_.hover + 1));
    mk = mix(mk, fbits(menu_.bounds.getX()) ^ (fbits(menu_.bounds.getY()) << 32));
    mk = mix(mk, menu_.items.size());
    if (mk != menuKey_) {
        menuKey_ = mk;
        const auto m = menu_.open ? toLocal(menu_.bounds.expanded(34.f, 30.f).withTrimmedBottom(-14.f)) : juce::Rectangle<int>();
        cables_->repaint(m.getUnion(menuShown_));
        menuShown_ = m;
    }
}

// ------------------------------------------------------------------ frame loop

void LiftPanel::onVBlank(double ts) {
    const double dt = lastVBlank_ < 0.0 ? 1.0 / 60.0 : juce::jlimit(0.0, 0.1, ts - lastVBlank_);
    lastVBlank_ = ts;
    frame(dt);
}

void LiftPanel::frame(double dt) {
    inFrame_ = true;
    advance(dt);
    inFrame_ = false;
    refreshParts();
    // idle frames prepare the greyed cables, so picking one up costs no frame
    if (!drag_.active) {
        for (auto& kv : cableSprites_) {
            if (!kv.second.gray.isValid()) {
                makeGray(kv.second);
                break;
            }
        }
    }
    // A real display: it refreshes at a fixed rate, never faster, whatever
    // the host display runs at (ProMotion 120 Hz), and only when something on
    // it changed.
    screenClock_ += dt;
    if (screenDirty_ && screenClock_ >= 1.0 / kDisplayHz - 0.002) {
        screenClock_ = 0.0;
        screen_->repaint();
    }
}

// ------------------------------------------------------------------ static art

void LiftPanel::ensureArt(float dps) {
    if (art_.isValid() && artDps_ == dps) {
        return;
    }
    artDps_ = dps;
    const int w = static_cast<int>(std::ceil(static_cast<float>(kW) * dps));
    const int h = static_cast<int>(std::ceil(static_cast<float>(kH) * dps));
    juce::Image src;
    if (dps <= 2.f) {
        src = juce::ImageFileFormat::loadFrom(LiftArtData::case2x_jpg, static_cast<size_t>(LiftArtData::case2x_jpgSize));
    } else {
        src = juce::ImageFileFormat::loadFrom(LiftArtData::case3x_jpg, static_cast<size_t>(LiftArtData::case3x_jpgSize));
    }
    const bool svg = src.isValid();
    if (svg) {
        // the art covers the canvas exactly: map it to whole device pixels
        const float sx = static_cast<float>(kW) * dps, sy = static_cast<float>(kH) * dps;
        const juce::Image fit = boxResample(src, juce::roundToInt(sx), juce::roundToInt(sy));
        art_ = juce::Image(juce::Image::RGB, w, h, false);
        Graphics ag(art_);
        ag.fillAll(hex(0xc2bdb3));
        ag.drawImageAt(fit, 0, 0);
    } else {
        art_ = juce::Image(juce::Image::RGB, w, h, false);
        Graphics ag(art_);
        ag.fillAll(hex(0xc2bdb3));
        ag.addTransform(AffineTransform::translation(kDevX, kDevY).scaled(dps));
        paintCase(ag);
    }
    Graphics ag(art_);
    ag.addTransform(AffineTransform::translation(kDevX, kDevY).scaled(dps));
    paintBayStatic(ag, !svg);
    paintBrand(ag);
    drawScreenBezel(ag, !svg);
    paintMembraneStatic(ag, !svg);
}

// ------------------------------------------------------------------ sprites

void LiftPanel::blit(Graphics& g, float phys, juce::uint64 variant, Rectangle<float> devBounds, juce::Point<float> devAnchor,
                     const std::function<void(Graphics&)>& draw, float opacity) {
    const float dps = scale_ * phys;
    if (spriteDps_ != dps) {
        spriteDps_ = dps;
        sprites_.clear();
    }
    const juce::Point<float> A = (devAnchor + juce::Point<float>(kDevX, kDevY)) * dps;
    const juce::Point<int> ia(static_cast<int>(std::floor(A.x)), static_cast<int>(std::floor(A.y)));
    // sub-pixel phase in eighths: sprites are shared by parts on the same phase
    const int qx = juce::jlimit(0, 7, static_cast<int>(std::floor((A.x - static_cast<float>(ia.x)) * 8.f)));
    const int qy = juce::jlimit(0, 7, static_cast<int>(std::floor((A.y - static_cast<float>(ia.y)) * 8.f)));
    const juce::uint64 key = mix(mix(variant, static_cast<juce::uint64>(qx * 8 + qy)), 0x51ULL);
    auto it = sprites_.find(key);
    if (it == sprites_.end()) {
        const juce::Point<float> ph(static_cast<float>(qx) / 8.f + 1.f / 16.f, static_cast<float>(qy) / 8.f + 1.f / 16.f);
        const auto rel = (devBounds.getTopLeft() - devAnchor) * dps + ph;
        Sprite s;
        s.off = {static_cast<int>(std::floor(rel.x)), static_cast<int>(std::floor(rel.y))};
        const int w = static_cast<int>(std::ceil(devBounds.getWidth() * dps)) + 2;
        const int h = static_cast<int>(std::ceil(devBounds.getHeight() * dps)) + 2;
        s.img = juce::Image(juce::Image::ARGB, w, h, true);
        {
            Graphics ig(s.img);
            ig.addTransform(AffineTransform::translation(-devAnchor.x, -devAnchor.y)
                                .scaled(dps)
                                .translated(ph.x - static_cast<float>(s.off.x), ph.y - static_cast<float>(s.off.y)));
            draw(ig);
        }
        it = sprites_.emplace(key, std::move(s)).first;
    }
    const Sprite& s = it->second;
    g.setOpacity(opacity);
    g.drawImageTransformed(s.img, AffineTransform::translation(static_cast<float>(ia.x + s.off.x), static_cast<float>(ia.y + s.off.y))
                                      .scaled(1.f / phys));
    g.setOpacity(1.f);
}

void LiftPanel::paintKnob(Graphics& g, float phys, int i) {
    KnobImg& K = knobImg_[static_cast<size_t>(i)];
    const float dps = scale_ * phys;
    const float v = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
    const juce::Point<float> org = knobOrigin(i) + juce::Point<float>(kDevX, kDevY);
    if (K.dps != dps) {
        K.dps = dps;
        K.v = -1.f;
        for (auto& r : K.recent) {
            r = {-1.f, {}};
        }
        K.org = {static_cast<int>(std::floor((org.x - 14.f) * dps)), static_cast<int>(std::floor((org.y - 14.f) * dps))};
        const int sz = static_cast<int>(std::ceil(168.f * dps)) + 2;
        const auto tf = AffineTransform::translation(org.x, org.y).scaled(dps).translated(static_cast<float>(-K.org.x),
                                                                                          static_cast<float>(-K.org.y));
        K.img = juce::Image(juce::Image::ARGB, sz, sz, true);
        K.skirt = juce::Image(juce::Image::ARGB, sz, sz, true);
        K.cap = juce::Image(juce::Image::ARGB, sz, sz, true);
        {
            Graphics sg(K.skirt);
            sg.addTransform(tf);
            drawKnobSkirt(sg, i);
        }
        {
            Graphics cg(K.cap);
            cg.addTransform(tf);
            drawKnobCap(cg);
        }
        K.contact = juce::Image(juce::Image::ARGB, static_cast<int>(std::ceil(160.f * dps)), static_cast<int>(std::ceil(160.f * dps)), true);
        {
            Graphics sg(K.contact);
            sg.addTransform(AffineTransform::translation(10.f, 10.f).scaled(dps));
            drawKnobContact(sg);
        }
    }
    if (K.v != v) {
        K.v = v;
        bool found = false;
        for (auto& r : K.recent) {
            if (r.first == v && r.second.isValid()) {
                K.img = r.second;
                found = true;
                break;
            }
        }
        if (!found) {
            K.img = juce::Image(juce::Image::ARGB, K.skirt.getWidth(), K.skirt.getHeight(), true);
            const auto tf = AffineTransform::translation(org.x, org.y).scaled(dps).translated(static_cast<float>(-K.org.x),
                                                                                              static_cast<float>(-K.org.y));
            Graphics kg(K.img);
            {
                Graphics::ScopedSaveState s(kg);
                kg.addTransform(tf);
                drawKnobTicks(kg, v);
            }
            kg.drawImageAt(K.skirt, 0, 0);
            {
                // the cached contact shadow, turned with the body; its offset stays to the light
                const float rad = (-135.f + v * 270.f) * juce::MathConstants<float>::pi / 180.f;
                Graphics::ScopedSaveState s(kg);
                kg.setImageResamplingQuality(Graphics::mediumResamplingQuality);
                kg.drawImageTransformed(K.contact, AffineTransform::scale(1.f / dps)
                                                       .translated(-10.f, -10.f)
                                                       .rotated(rad, 70.f, 70.f)
                                                       .translated(2.f, 3.f)
                                                       .followedBy(tf));
                kg.addTransform(tf);
                drawKnobBody(kg, i, v, false);
            }
            kg.drawImageAt(K.cap, 0, 0);
            K.recent[static_cast<size_t>(K.next)] = {v, K.img};
            K.next = (K.next + 1) % static_cast<int>(K.recent.size());
        }
    }
    g.drawImageTransformed(K.img, AffineTransform::translation(static_cast<float>(K.org.x), static_cast<float>(K.org.y)).scaled(1.f / phys));
}

// ------------------------------------------------------------------ panel paint

void LiftPanel::paint(Graphics& g) {
    ScopedPaintStats st;
    const float phys = juce::jmax(0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const float dps = scale_ * phys;
    ensureArt(dps);
    g.setImageResamplingQuality(Graphics::lowResamplingQuality);
    g.drawImageTransformed(art_, AffineTransform::scale(1.f / phys));

    const juce::Point<float> o(kDevX, kDevY);
    auto hits = [&](Rectangle<float> canvas) { return g.clipRegionIntersects(toLocal(canvas)); };
    auto dev = [&](const std::function<void()>& fn) {
        Graphics::ScopedSaveState s(g);
        g.addTransform(AffineTransform::translation(kDevX, kDevY).scaled(scale_));
        fn();
    };
    if (hits(partRect(0))) {
        Graphics::ScopedSaveState s(g);
        g.addTransform(AffineTransform::scale(scale_));
        paintTopBar(g);
    }
    // jacks
    for (int p = 0; p < 32; ++p) {
        if (!hits(partRect(1 + p))) {
            continue;
        }
        const char rc = p < 16 ? 'o' : 'i';
        const int i = p % 16;
        const bool picked = pick_.valid() && pick_.r == rc && pick_.i == i;
        const bool ok = !picked && drag_.active && drag_.started && drag_.need == rc;
        const juce::uint64 var = mix(0x1a, static_cast<juce::uint64>((rc == 'o' ? 1 : 0) | (picked ? 2 : 0) | (ok ? 4 : 0)));
        const juce::Point<float> c(jx(i), jy(rc));
        blit(g, phys, var, {c.x - 26.f, c.y - 26.f, 52.f, 56.f}, c, [rc, i, picked, ok](Graphics& sg) { drawJack(sg, rc, i, picked, ok); });
    }
    // knobs
    for (int i = 0; i < 4; ++i) {
        if (hits(partRect(33 + i))) {
            paintKnob(g, phys, i);
        }
    }
    if (hits(Rectangle<float>(684.f, 400.f, 640.f, 26.f) + o)) {
        dev([&] { paintKnobLabels(g); });
    }
    // pads
    for (int k = 0; k < 24; ++k) {
        if (!hits(partRect(37 + k))) {
            continue;
        }
        const PadLook L = padLook(k);
        const auto b = L.r.withTrimmedTop(-14.f).withTrimmedLeft(-14.f).withTrimmedRight(-14.f).withTrimmedBottom(-28.f);
        if (L.unused) {
            juce::uint64 var = mix(0x2b, shash(L.label));
            var = mix(var, shash(L.big));
            blit(g, phys, var, b, L.r.getTopLeft(), [L](Graphics& sg) {
                drawPadBody(sg, L.r, L.col, L.lit);
                drawPadText(sg, L, 1.f);
            }, 0.45f);
            continue;
        }
        const juce::uint64 var = mix(mix(0x2c, L.col), L.lit ? 1 : 0);
        blit(g, phys, var, b, L.r.getTopLeft(), [L](Graphics& sg) { drawPadBody(sg, L.r, L.col, L.lit); });
        dev([&] { drawPadText(g, L, 1.f); });
    }
    // membrane keys (their own light), keyboard
    if (hits(Rectangle<float>(40.f, 660.f, 640.f, 230.f) + o)) {
        dev([&] { paintMemKeys(g); });
    }
    for (int p = 0; p < 10; ++p) {
        if (!hits(partRect(85 + p))) {
            continue;
        }
        const int oc = p / 5, b = p % 5;
        const bool down = heldNote_ == oc * 12 + SHARP_DEF[b][0];
        const auto c = sharpPos(oc, b);
        blit(g, phys, mix(0x3d, down ? 1 : 0), {c.x - 36.f, c.y - 36.f, 72.f, 84.f}, c,
             [c, down](Graphics& sg) { drawSharp(sg, c, down); });
    }
    for (int j = 0; j < 14; ++j) {
        if (!hits(partRect(71 + j))) {
            continue;
        }
        const bool down = heldNote_ == (j / 7) * 12 + NAT_SEMI[j % 7];
        const auto r = naturalRect(j);
        const auto b = r.withTrimmedTop(-14.f).withTrimmedLeft(-14.f).withTrimmedRight(-14.f).withTrimmedBottom(-28.f);
        blit(g, phys, mix(0x4e, down ? 1 : 0), b, r.getTopLeft(), [r, down](Graphics& sg) { drawNatural(sg, r, down); });
    }
    if (hits(Rectangle<float>(684.f, 754.f, 640.f, 100.f) + o)) {
        dev([&] { paintKeyLabels(g); });
    }
}

// ------------------------------------------------------------------ screen

void LiftPanel::renderFb() {
    if (!fb_.isValid()) {
        fb_ = juce::Image(juce::Image::RGB, kFbW, kFbH, false);  // opaque: blits are copies
    }
    Graphics fg(fb_);
    const float k = static_cast<float>(kFbW) / 600.f;
    fg.addTransform(AffineTransform::translation(-60.f, -256.f).scaled(k));
    paintScreen(fg);
}

void LiftPanel::paintScreenLayer(Graphics& g) {
    if (screenDirty_ || !fb_.isValid()) {
        screenDirty_ = false;
        renderFb();
    }
    // the framebuffer maps onto the glass; at 100 % on a 2x display it is 1:1
    const auto b = screen_->getLocalBounds().toFloat();
    const float phys = juce::jmax(0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const bool exact = juce::roundToInt(b.getWidth() * phys) == kFbW && juce::roundToInt(b.getHeight() * phys) == kFbH;
    g.setImageResamplingQuality(exact ? Graphics::lowResamplingQuality : Graphics::highResamplingQuality);
    g.drawImage(fb_, b, juce::RectanglePlacement::stretchToFit);
}

// ------------------------------------------------------------------ cables

// greyed copy: the CSS grayscale(1) brightness(.8) of the prototype
void LiftPanel::makeGray(CableSprite& cs) {
    cs.gray = cs.img.createCopy();
    juce::Image::BitmapData bd(cs.gray, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < bd.height; ++y) {
        for (int x = 0; x < bd.width; ++x) {
            auto* px = reinterpret_cast<juce::PixelARGB*>(bd.getPixelPointer(x, y));
            const float l = 0.8f * (0.2126f * px->getRed() + 0.7152f * px->getGreen() + 0.0722f * px->getBlue());
            const auto v = static_cast<juce::uint8>(juce::jmin(static_cast<float>(px->getAlpha()), l + 0.5f));
            px->setARGB(px->getAlpha(), v, v, v);
        }
    }
}

// The settled cables, composed from one cached image per cable (its look at
// its jacks and stack levels). Picking a cable up only re-composes them
// (greyed: the same image through the CSS grayscale(1) brightness(.8) matrix,
// which is linear, so it applies to the finished pixels exactly).
void LiftPanel::buildCableImage(float phys) {
    const float dps = scale_ * phys;
    const bool moving = drag_.active && drag_.isMove && drag_.started;
    const bool aside = drag_.active && drag_.started;
    const auto lv = levels(moving ? drag_.n : -1);
    const auto r = cableShown_.isEmpty() ? getLocalBounds() : cableShown_;
    cableImgDev_ = (r.toFloat() * phys).getSmallestIntegerContainer();
    cableImg_ = juce::Image(juce::Image::ARGB, juce::jmax(1, cableImgDev_.getWidth()), juce::jmax(1, cableImgDev_.getHeight()), true);
    for (auto& kv : cableSprites_) {
        kv.second.used = false;
    }
    Graphics cg(cableImg_);
    for (size_t n = 0; n < cords_.size(); ++n) {
        if (moving && static_cast<int>(n) == drag_.n) {
            continue;
        }
        const Cord& c = cords_[n];
        const float x1 = jx(c.o), y1 = jy('o') - 6.f * static_cast<float>(lv[n].o);
        const float x2 = jx(c.i), y2 = jy('i') - 6.f * static_cast<float>(lv[n].i);
        juce::uint64 key = mix(0xcab1e, static_cast<juce::uint64>(((c.o * 16 + c.i) * 8 + c.c) * 2 + (c.st ? 1 : 0)));
        key = mix(key, static_cast<juce::uint64>(lv[n].o * 64 + lv[n].i));
        key = mix(key, fbits(dps));
        auto it = cableSprites_.find(key);
        if (it == cableSprites_.end()) {
            CableSprite cs;
            cs.dev = ((cableBounds(x1, y1, x2, y2) + juce::Point<float>(kDevX, kDevY)) * dps).getSmallestIntegerContainer();
            cs.img = juce::Image(juce::Image::ARGB, juce::jmax(1, cs.dev.getWidth()), juce::jmax(1, cs.dev.getHeight()), true);
            Graphics ig(cs.img);
            ig.addTransform(AffineTransform::translation(kDevX, kDevY)
                                .scaled(dps)
                                .translated(static_cast<float>(-cs.dev.getX()), static_cast<float>(-cs.dev.getY())));
            drawSettledCable(ig, x1, y1, x2, y2, c.c, c.st);
            it = cableSprites_.emplace(key, std::move(cs)).first;
        }
        CableSprite& cs = it->second;
        cs.used = true;
        if (aside && !cs.gray.isValid()) {
            makeGray(cs);
        }
        cg.setOpacity(aside ? 0.5f : 1.f);
        cg.drawImageAt(aside ? cs.gray : cs.img, cs.dev.getX() - cableImgDev_.getX(), cs.dev.getY() - cableImgDev_.getY());
    }
    // drop images of cables no longer in the patch (keep the one in hand)
    for (auto it = cableSprites_.begin(); it != cableSprites_.end();) {
        it = !it->second.used && cableSprites_.size() > cords_.size() + 4 ? cableSprites_.erase(it) : std::next(it);
    }
}


void LiftPanel::paintCableLayer(Graphics& g) {
    const float phys = juce::jmax(0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const float dps = scale_ * phys;
    if (!cords_.empty()) {
        if (!cableImg_.isValid() || cableDps_ != dps) {
            cableDps_ = dps;
            buildCableImage(phys);
        }
        g.setImageResamplingQuality(Graphics::lowResamplingQuality);
        g.drawImageTransformed(cableImg_, AffineTransform::translation(static_cast<float>(cableImgDev_.getX()),
                                                                        static_cast<float>(cableImgDev_.getY()))
                                              .scaled(1.f / phys));
    }
    if (drag_.active && drag_.started) {
        Graphics::ScopedSaveState s(g);
        g.addTransform(AffineTransform::translation(kDevX, kDevY).scaled(scale_));
        paintLiveCable(g);
    }
    if (menu_.open) {
        Graphics::ScopedSaveState s(g);
        g.addTransform(AffineTransform::scale(scale_));
        paintMenu(g);
    }
}

}  // namespace lift
