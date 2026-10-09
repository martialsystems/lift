// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Motion primitives for the screen: springs with overshoot for anything a
// knob drives, decaying "kicks" for hits, and a couple of easing curves. The
// motion language is flat and elastic: things overshoot, squash and settle.

#include <array>
#include <cmath>
#include <vector>

namespace lift {

// Damped spring toward a target. Settles exactly on the target, so a screen
// at rest draws the same pixels as the static design.
struct Spring {
    float x = 0.f;
    float v = 0.f;
    bool init = false;
    void step(float target, float dt, bool snap, float k = 420.f, float c = 16.f) {
        if (!init || snap) {
            x = target;
            v = 0.f;
            init = true;
            return;
        }
        const int n = dt > 0.f ? static_cast<int>(std::ceil(dt * 240.f)) : 0;
        const float h = n > 0 ? dt / static_cast<float>(n) : 0.f;
        for (int i = 0; i < n; ++i) {
            v += (k * (target - x) - c * v) * h;
            x += v * h;
        }
        if (std::abs(target - x) < 1e-4f && std::abs(v) < 1e-3f) {
            x = target;
            v = 0.f;
        }
    }
    bool moving(float target) const { return x != target || v != 0.f; }
};

// Damped wobble after an event, 0 at rest. since = seconds since the event.
inline float kick(double since, float freq = 26.f, float decay = 9.f) {
    if (since < 0.0 || since > 1.2) {
        return 0.f;
    }
    const float t = static_cast<float>(since);
    return std::exp(-t * decay) * std::cos(t * freq);
}

// Plain exponential decay after an event, 1 at the event.
inline float decayFrom(double since, float rate) {
    if (since < 0.0 || since > 3.0) {
        return 0.f;
    }
    return std::exp(-static_cast<float>(since) * rate);
}

inline float easeOutBack(float p, float s = 1.9f) {
    p = p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
    const float q = p - 1.f;
    return 1.f + q * q * ((s + 1.f) * q + s);
}

inline float easeOutBounce(float p) {
    p = p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
    const float n = 7.5625f, d = 2.75f;
    if (p < 1.f / d) {
        return n * p * p;
    }
    if (p < 2.f / d) {
        p -= 1.5f / d;
        return n * p * p + 0.75f;
    }
    if (p < 2.5f / d) {
        p -= 2.25f / d;
        return n * p * p + 0.9375f;
    }
    p -= 2.625f / d;
    return n * p * p + 0.984375f;
}

// Static screen data shared by the panel logic and the painters.
namespace screen {
constexpr float kMixLevels[4] = {0.72f, 0.55f, 0.8f, 0.4f};
constexpr float kMixPans[4] = {0.5f, 0.3f, 0.62f, 0.8f};
// PLACEHOLDER pattern: there is no drum engine yet; this is the prototype's
// demo pattern, swept by the real tape clock.
constexpr int kDrumPat[16] = {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0};
constexpr float kDrumVel[16] = {1, 0, 0, 0, 0.7f, 0, 0, 0.4f, 1, 0, 0, 0, 0.7f, 0, 0.5f, 0};
constexpr double kTempoBpm = 120.0;  // placeholder project tempo
constexpr int kScopeRing = 4096;
}  // namespace screen

// Everything the screen animates. Owned by LiftPanel, touched only on the
// message thread.
struct ScreenAnim {
    // view transitions (0..4 = mode, 5 = BAY)
    int view = 2;
    int prevView = 2;
    int viewDir = 1;
    double viewT = -10.0;
    double labelT = -10.0;
    // knob-driven
    std::array<Spring, 4> foot{};
    std::array<Spring, 4> fader{};
    std::array<Spring, 4> pan{};
    Spring eqLo, eqHi, needle, gain;
    // tape
    float reelL = 0.f;
    float reelR = 0.f;
    float omega = 0.f;  // rad/s, for the motion ghosts
    double lastPos = -1.0;
    float headGlow = 0.f;
    float speedAbs = 0.f;
    float wowPhase = 0.f;
    float wowDepth = 0.f;
    double liftT = -10.0;
    double dropT = -10.0;
    // synth
    std::vector<float> scope = std::vector<float>(screen::kScopeRing, 0.f);
    std::vector<float> scratch = std::vector<float>(8192, 0.f);
    int scopeW = 0;
    float env = 0.f;
    int note = -1;
    double noteT = -10.0;
    int engine = 0;
    int prevEngine = 0;
    double engineT = -10.0;
    // drum
    double drumPhase = 0.0;  // in 16th steps
    int lastStep = -1;
    int lastSlice = -1;
    std::array<double, 16> hitT{};
    std::array<double, 24> sliceT{};
    // mix
    std::array<float, 5> meter{};
    std::array<float, 5> peak{};
    std::array<double, 5> peakT{};
    std::array<double, 4> muteT{};
    std::array<bool, 4> lastMute{};
    // in (placeholder input motion)
    float inL = 0.92f;
    float inR = 0.84f;
    float inTL = 0.92f;
    float inTR = 0.84f;
    double inNext = 0.0;
    bool above = false;
    double threshT = -10.0;
    unsigned rng = 0x1234567u;
    // bay
    double bayT = -10.0;
    size_t lastCords = 0;
    double cordT = -10.0;
    // status
    double msgT = -10.0;

    float rand01() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xffffff) / 16777216.f;
    }
};

}  // namespace lift
