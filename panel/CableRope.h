// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cmath>
#include <vector>

// Hanging patch cables: a Verlet rope per cable that parts around the pointer,
// the hovered jack and the hovered label, then settles back. Ported from
// BUSHIDO rack/CableModel.cpp (step, push, pins, initRope) and RONIN
// PatchBayView's rope (the repel() helper), same numbers as the v3.1 mockup:
// 36 nodes, gravity 0.45 px/step, damping 0.985, 10 constraint passes, slack
// d + min(0.3 d + 40, 140). Dev pixels. Message thread only.

namespace lift::ui {

struct RopeScene {
    std::vector<juce::Rectangle<float>> labels;  // printed labels the rope keeps off (hover)
    float floor = 1e9f;                          // cables rest on the brand line
    bool ptrIn = false;
    juce::Point<float> ptr;
    bool hasJack = false;
    juce::Point<float> jack;  // hovered jack centre
    int hoverLabel = -1;      // index into labels, -1 none
};

class Rope {
public:
    static constexpr int N = 36;
    bool valid() const { return init_; }

    // Pin the ends (a, b); a new rope is laid and settled before its first frame.
    void attach(juce::Point<float> a, juce::Point<float> b, const RopeScene& sc) {
        a_ = a;
        b_ = b;
        const float d = a.getDistanceFrom(b);
        L_ = d + std::fmin(d * 0.3f + 40.f, 140.f);
        if (!init_) {
            for (int i = 0; i < N; ++i) {
                const float t = static_cast<float>(i) / (N - 1);
                p_[i] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t + std::sin(3.14159265f * t) * 40.f};
                q_[i] = p_[i];
            }
            init_ = true;
            RopeScene calm = sc;
            calm.ptrIn = false;
            calm.hasJack = false;
            calm.hoverLabel = -1;
            for (int k = 0; k < 160; ++k) {
                step(calm);
            }
        }
    }

    // One step; returns how far the rope moved (settled below ~0.03).
    float step(const RopeScene& sc) {
        const float seg = L_ / (N - 1);
        float mv = 0.f;
        // BUSHIDO's damping while it moves; once it only sways (under ~0.4 px a
        // step) it is damped harder, so a rope comes to rest in well under a
        // second instead of swinging on for seconds
        const float damp = lastMv_ < 0.4f ? 0.9f : 0.985f;
        for (int i = 1; i < N - 1; ++i) {
            const auto v = p_[i];
            const float nx = v.x + (v.x - q_[i].x) * damp, ny = v.y + (v.y - q_[i].y) * damp + 0.45f;
            q_[i] = v;
            p_[i] = {nx, ny};
        }
        for (int it = 0; it < 10; ++it) {
            p_[0] = a_;
            p_[N - 1] = b_;
            for (int i = 0; i < N - 1; ++i) {
                auto& a = p_[i];
                auto& b = p_[i + 1];
                const float dx = b.x - a.x, dy = b.y - a.y, d = std::fmax(1e-3f, std::hypot(dx, dy)), k = (d - seg) / d;
                const float wa = i ? 1.f : 0.f, wb = i < N - 2 ? 1.f : 0.f, w = wa + wb;
                a.x += dx * k * wa / w;
                a.y += dy * k * wa / w;
                b.x -= dx * k * wb / w;
                b.y -= dy * k * wb / w;
            }
            for (int i = 1; i < N - 1; ++i) {
                push(p_[i], sc);
            }
        }
        // how far it really moved this step (after the constraints and the
        // floor: a cable resting on the brand line is still, gravity or not)
        for (int i = 1; i < N - 1; ++i) {
            mv = std::fmax(mv, std::fabs(p_[i].x - q_[i].x) + std::fabs(p_[i].y - q_[i].y));
        }
        lastMv_ = mv;
        return mv;
    }

    // Smooth through the nodes (midpoint quadratics).
    juce::Path path() const {
        juce::Path d;
        d.startNewSubPath(p_[0]);
        for (int i = 1; i < N - 1; ++i) {
            d.quadraticTo(p_[i], (p_[i] + p_[i + 1]) * 0.5f);
        }
        d.lineTo(p_[N - 1]);
        return d;
    }
    juce::Rectangle<float> bounds() const {
        float x0 = p_[0].x, x1 = x0, y0 = p_[0].y, y1 = y0;
        for (const auto& v : p_) {
            x0 = std::fmin(x0, v.x);
            x1 = std::fmax(x1, v.x);
            y0 = std::fmin(y0, v.y);
            y1 = std::fmax(y1, v.y);
        }
        return {x0, y0, x1 - x0, y1 - y0};
    }
    // A cheap fingerprint of the shape (cached images follow it).
    juce::uint64 shapeKey() const {
        juce::uint64 h = 1469598103934665603ULL;
        for (const auto& v : p_) {
            h = (h ^ static_cast<juce::uint64>(std::lround(v.x * 2.f) * 4099 + std::lround(v.y * 2.f))) * 1099511628211ULL;
        }
        return h;
    }

private:
    static void repel(juce::Point<float>& n, juce::Point<float> c, float r) {
        const float dx = n.x - c.x, dy = n.y - c.y, d = std::hypot(dx, dy);
        if (d < r) {
            if (d > 0.f) {
                const float f = (r - d) / d * 0.6f;
                n.x += dx * f;
                n.y += dy * f;
            } else {
                n.y += r * 0.6f;
            }
        }
    }
    static void push(juce::Point<float>& n, const RopeScene& sc) {
        if (sc.hasJack) {
            repel(n, sc.jack, 40.f);
        }
        if (sc.ptrIn) {
            repel(n, sc.ptr, 28.f);
        }
        if (sc.hoverLabel >= 0 && sc.hoverLabel < static_cast<int>(sc.labels.size())) {
            const auto r = sc.labels[static_cast<size_t>(sc.hoverLabel)];
            if (n.x > r.getX() - 10.f && n.x < r.getRight() + 10.f && n.y > r.getY() - 10.f && n.y < r.getBottom() + 10.f) {
                const float l = n.x - (r.getX() - 10.f), rr = r.getRight() + 10.f - n.x;
                const float t = n.y - (r.getY() - 10.f), b = r.getBottom() + 10.f - n.y;
                const float m = std::fmin(std::fmin(l, rr), std::fmin(t, b));
                if (m == l) n.x -= l * 0.6f;
                else if (m == rr) n.x += rr * 0.6f;
                else if (m == t) n.y -= t * 0.6f;
                else n.y += b * 0.6f;
            }
        }
        if (n.y > sc.floor) {
            n.y = sc.floor;
        }
    }

    std::array<juce::Point<float>, N> p_{}, q_{};
    juce::Point<float> a_, b_;
    float L_ = 0.f;
    float lastMv_ = 1.f;
    bool init_ = false;
};

}  // namespace lift::ui
