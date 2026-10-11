// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/grain.h"

#include <algorithm>
#include <cmath>

namespace lift::eng {

void GrainCloud::prepare(double fs) {
    fs_ = fs > 0.0 ? fs : 48000.0;
    cap_ = static_cast<int>(kBufferSeconds * fs_);
    l_.assign(static_cast<size_t>(cap_), 0.f);
    r_.assign(static_cast<size_t>(cap_), 0.f);
    reset();
}

void GrainCloud::reset() noexcept {
    std::fill(l_.begin(), l_.end(), 0.f);
    std::fill(r_.begin(), r_.end(), 0.f);
    w_ = 0;
    written_ = 0;
    for (auto& g : g_) g.on = false;
    untilNext_ = 0.0;
}

void GrainCloud::write(const float* l, const float* r, int n) noexcept {
    if (cap_ <= 0) return;
    for (int i = 0; i < n; ++i) {
        l_[static_cast<size_t>(w_)] = l[i];
        r_[static_cast<size_t>(w_)] = r != nullptr ? r[i] : l[i];
        if (++w_ >= cap_) w_ = 0;
    }
    written_ += n;
}

void GrainCloud::setSource(const float* l, const float* r, int frames, int lo, int hi) noexcept {
    extL_ = l;
    extR_ = r;
    extN_ = frames;
    extLo_ = lo;
    extHi_ = hi;
}

float GrainCloud::rnd() noexcept {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) / 16777216.f;
}

int GrainCloud::activeGrains() const noexcept {
    int k = 0;
    for (const auto& g : g_) k += g.on ? 1 : 0;
    return k;
}

void GrainCloud::spawn(const Params& p) noexcept {
    Grain* slot = nullptr;
    for (auto& g : g_) {
        if (!g.on) {
            slot = &g;
            break;
        }
    }
    if (slot == nullptr) return;
    const float size = std::clamp(p.size, 0.f, 1.f);
    const int len = std::max(64, static_cast<int>((0.01 + 0.49 * size * size) * fs_));
    const double rate = p.pitch * std::exp2((rnd() - 0.5f) * 2.f * p.pitchSpread / 12.f);
    const float jitter = (rnd() - 0.5f) * p.spray;
    const float pos = std::clamp(p.pos + 0.25f * jitter, 0.f, 1.f);
    double start;
    if (extL_ != nullptr) {
        const bool region = extHi_ > extLo_;
        const int lo = region ? extLo_ : 0, hi = region ? std::min(extHi_, extN_) : extN_;
        const int span = hi - lo;
        if (span < 2 * len) return;
        start = lo + pos * (span - len * rate - 2);
        if (start < lo) start = lo;
    } else {
        if (written_ < len + 2) return;
        // how far back: POS 0 = just written .. 1 = the oldest of the 8 s; the
        // grain never overtakes the write head
        const double room = std::min<double>(static_cast<double>(written_), cap_) - len * std::max(1.0, rate) - 2.0;
        if (room <= 0.0) return;
        const double back = len * std::max(1.0, rate) + 1.0 + pos * room;
        start = static_cast<double>(w_) - back;
        while (start < 0.0) start += cap_;
    }
    const float pan = 0.5f + (rnd() - 0.5f) * p.spray;
    slot->pos = start;
    slot->rate = rate;
    slot->len = len;
    slot->age = 0;
    slot->gl = std::cos(pan * 1.5707963f);
    slot->gr = std::sin(pan * 1.5707963f);
    slot->on = true;
}

void GrainCloud::render(float* l, float* r, int n, const Params& p, float gain) noexcept {
    const bool ext = extL_ != nullptr;
    if ((!ext && cap_ <= 0) || (ext && extN_ < 4)) return;
    const double perSec = 2.0 + 78.0 * std::clamp(p.density, 0.f, 1.f);
    const double interval = fs_ / perSec;
    // normalise for overlap: about sqrt(grains in flight)
    const double size = std::clamp(p.size, 0.f, 1.f);
    const double overlap = std::max(1.0, perSec * (0.01 + 0.49 * size * size));
    const float norm = gain * static_cast<float>(1.0 / std::sqrt(overlap));
    for (int i = 0; i < n; ++i) {
        untilNext_ -= 1.0;
        if (untilNext_ <= 0.0) {
            spawn(p);
            untilNext_ += interval * (0.75 + 0.5 * rnd());
        }
        float sl = 0.f, sr = 0.f;
        for (auto& g : g_) {
            if (!g.on) continue;
            const double ph = static_cast<double>(g.age) / g.len;
            const float w = 0.5f - 0.5f * static_cast<float>(std::cos(6.283185307 * ph));
            const int i0 = static_cast<int>(g.pos);
            const float fr = static_cast<float>(g.pos - i0);
            float a, b;
            if (ext) {
                const int j0 = std::clamp(i0, 0, extN_ - 2);
                a = extL_[j0] + fr * (extL_[j0 + 1] - extL_[j0]);
                const float* R = extR_ != nullptr ? extR_ : extL_;
                b = R[j0] + fr * (R[j0 + 1] - R[j0]);
            } else {
                const int j0 = i0 % cap_, j1 = (j0 + 1) % cap_;
                a = l_[static_cast<size_t>(j0)] + fr * (l_[static_cast<size_t>(j1)] - l_[static_cast<size_t>(j0)]);
                b = r_[static_cast<size_t>(j0)] + fr * (r_[static_cast<size_t>(j1)] - r_[static_cast<size_t>(j0)]);
            }
            const float m = 0.5f * (a + b);
            sl += w * (a * 0.5f + m * 0.5f) * g.gl * 1.414f;
            sr += w * (b * 0.5f + m * 0.5f) * g.gr * 1.414f;
            g.pos += g.rate;
            if (!ext && g.pos >= cap_) g.pos -= cap_;
            if (++g.age >= g.len) g.on = false;
        }
        l[i] += sl * norm;
        r[i] += sr * norm;
    }
}

}  // namespace lift::eng
