// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/clipvoice.h"

#include <cmath>

namespace lift::eng {

void ClipPlayer::prepare(double fs) noexcept {
    att_ = static_cast<float>(1.0 / (0.002 * fs));
    relA_ = static_cast<float>(1.0 / (0.010 * fs));
    reset();
}

void ClipPlayer::reset() noexcept {
    for (auto& v : v_) v = Voice{};
}

void ClipPlayer::trigger(const float* l, const float* r, int frames, double rate, float vel, int tag, bool oneShot) noexcept {
    if (l == nullptr || r == nullptr || frames < 2) return;
    // a free voice, else the oldest
    int pick = 0;
    for (int k = 0; k < kVoices; ++k) {
        if (!v_[k].on) {
            pick = k;
            break;
        }
        if (v_[k].age < v_[pick].age) pick = k;
    }
    Voice& v = v_[pick];
    v.l = l;
    v.r = r;
    v.n = frames;
    v.pos = 0.0;
    v.rate = rate > 0.0 ? rate : 1.0;
    v.gain = vel;
    v.env = 0.f;
    v.on = true;
    v.rel = false;
    v.oneShot = oneShot;
    v.tag = tag;
    v.age = ++age_;
}

void ClipPlayer::release(int tag) noexcept {
    for (auto& v : v_) {
        if (v.on && v.tag == tag && !v.oneShot) v.rel = true;
    }
}

void ClipPlayer::releaseAll() noexcept {
    for (auto& v : v_) {
        if (v.on) v.rel = true;
    }
}

bool ClipPlayer::active() const noexcept {
    for (const auto& v : v_) {
        if (v.on) return true;
    }
    return false;
}

void ClipPlayer::render(float* L, float* R, int n) noexcept {
    for (auto& v : v_) {
        if (!v.on) continue;
        for (int i = 0; i < n; ++i) {
            const int k = static_cast<int>(v.pos);
            if (k + 1 >= v.n) {
                v.on = false;
                break;
            }
            const float f = static_cast<float>(v.pos - k);
            const float xl = v.l[k] + f * (v.l[k + 1] - v.l[k]);
            const float xr = v.r[k] + f * (v.r[k + 1] - v.r[k]);
            if (v.rel) {
                v.env -= relA_;
                if (v.env <= 0.f) {
                    v.on = false;
                    break;
                }
            } else if (v.env < 1.f) {
                v.env = std::fmin(1.f, v.env + att_);
            }
            const float g = v.gain * v.env;
            L[i] += g * xl;
            R[i] += g * xr;
            v.pos += v.rate;
        }
    }
}

}  // namespace lift::eng
