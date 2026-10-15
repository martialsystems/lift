// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// The Mini's SHIFT key (README-pi.md, "SHIFT"). Pure logic, times in ms, so
// tests/mini_check.cpp can drive it.
//
//  - Hold:         SHIFT is on while held.
//  - Tap:          press and release within kTapMs with nothing else used
//                  = CANCEL / back (release() returns Cancel).
//  - Double tap:   a second tap within kDoubleMs of the first = LATCH. The
//                  first tap's cancel has already happened (cancel is safe to
//                  repeat). While latched SHIFT is on without holding it.
//  - Unlatch:      tap SHIFT again (no cancel), or press a mode key (the new
//                  mode opens un-shifted).
//  - SHIFT + use:  any key/button/turn while held marks SHIFT used, so the
//                  release is not a tap (no cancel).
//  - Release mid-turn: detents of the encoder that was being SHIFT-turned
//                  still count as SHIFT for kGraceMs after the release, so a
//                  turn that outlasts the finger does not jump to the other
//                  parameter. Other encoders are un-shifted at once.
#pragma once

#include <cmath>

namespace mini {

class ShiftState {
public:
    static constexpr double kTapMs = 300, kDoubleMs = 350, kGraceMs = 150;
    enum class Out { None, Cancel };

    void press(double t) {
        held_ = true;
        used_ = false;
        pressT_ = t;
        unlatchOnRelease_ = latched_;
    }
    Out release(double t) {
        held_ = false;
        if (used_) {
            if (graceEnc_ >= 0 && t - lastTurnT_ <= kGraceMs) graceUntil_ = t + kGraceMs;
            return Out::None;
        }
        if (t - pressT_ > kTapMs) return Out::None;  // a long press with nothing else: nothing
        if (unlatchOnRelease_) {
            latched_ = false;
            unlatchOnRelease_ = false;
            lastTap_ = -1e9;
            return Out::None;
        }
        if (t - lastTap_ <= kDoubleMs) {
            latched_ = true;
            lastTap_ = -1e9;
            return Out::None;
        }
        lastTap_ = t;
        return Out::Cancel;
    }
    // another control used (key, button). Mode keys use modeKey().
    void used() { if (held_) used_ = true; }
    void modeKey() {
        used();
        latched_ = false;
        unlatchOnRelease_ = false;
    }
    // is this turn of encoder `enc` at time t shifted? (call once per detent)
    bool turn(int enc, double t) {
        const bool on = active() || (enc == graceEnc_ && t <= graceUntil_);
        if (held_) {
            used_ = true;
            graceEnc_ = enc;
            lastTurnT_ = t;
        }
        if (on && !held_ && enc == graceEnc_ && t <= graceUntil_) graceUntil_ = t + kGraceMs;  // still turning
        return on;
    }
    bool active() const { return held_ || latched_; }
    bool latched() const { return latched_; }
    bool held() const { return held_; }

private:
    bool held_ = false, used_ = false, latched_ = false, unlatchOnRelease_ = false;
    double pressT_ = 0, lastTap_ = -1e9, lastTurnT_ = -1e9, graceUntil_ = -1e9;
    int graceEnc_ = -1;
};

// Yellow knob in TAPE: 0..1 -> signed speed. Centre = stop (a small dead
// band), then 0 .. 0.25x linearly through the first fifth of each side, then
// 0.25x .. 4x exponentially, snapping to fifths and octaves (within 20 cents).
inline float bipolarKnobToSpeed(float k) {
    const float x = (k - 0.5f) * 2.f;  // -1 .. 1
    const float a = x < 0.f ? -x : x;
    constexpr float dead = 0.04f, lin = 0.2f;
    if (a < dead) return 0.f;
    const float u = (a - dead) / (1.f - dead);
    float s;
    if (u < lin) {
        s = 0.25f * u / lin;
    } else {
        s = 0.25f * std::pow(16.f, (u - lin) / (1.f - lin));
        static const float snaps[] = {0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 1.f, 1.5f, 2.f, 3.f, 4.f};
        for (float t : snaps)
            if (std::abs(1200.f * std::log2(s / t)) < 20.f) s = t;
    }
    return x < 0.f ? -s : s;
}

}  // namespace mini
