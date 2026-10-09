// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <cmath>
#include <cstdint>

// One drawing. Magenta is the mark. Lime, cyan, and violet swap the fill.

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

constexpr Rgb kFaceGround{16, 16, 20};
constexpr Rgb kFaceMagenta{232, 20, 148};
constexpr Rgb kFaceLime{176, 240, 48};
constexpr Rgb kFaceCyan{32, 210, 214};
constexpr Rgb kFaceViolet{138, 72, 255};
constexpr Rgb kFaceSlot{255, 255, 255};

constexpr float kLobeD = 20.f;
constexpr float kLobeR = 26.f;
constexpr float kGhostTurn = 22.f;
constexpr int kGhostNum = 102;
constexpr int kGhostDen = 255;

inline Rgb face_color(int index) {
    if (index == 1) {
        return kFaceLime;
    }
    if (index == 2) {
        return kFaceCyan;
    }
    if (index == 3) {
        return kFaceViolet;
    }
    return kFaceMagenta;
}

inline uint8_t face_mix(uint8_t dst, uint8_t src) {
    return static_cast<uint8_t>((static_cast<int>(src) * kGhostNum + static_cast<int>(dst) * (kGhostDen - kGhostNum) + 127) /
                                kGhostDen);
}

// Out and back once a second. Stopped, the angle is zero.
inline float panel_ghost_degrees(double seconds, int playing) {
    if (!playing) {
        return 0.f;
    }
    double phase = seconds - std::floor(seconds);
    if (phase < 0.0) {
        phase += 1.0;
    }
    const double eased = 0.5 - 0.5 * std::cos(6.283185307179586 * phase);
    return static_cast<float>(eased * static_cast<double>(kGhostTurn));
}

inline bool face_in_body(float x, float y) {
    const float r2 = kLobeR * kLobeR;
    const float cx[4] = {0.f, kLobeD, 0.f, -kLobeD};
    const float cy[4] = {kLobeD, 0.f, -kLobeD, 0.f};
    for (int i = 0; i < 4; ++i) {
        const float dx = x - cx[i];
        const float dy = y - cy[i];
        if (dx * dx + dy * dy <= r2) {
            return true;
        }
    }
    return false;
}

inline bool face_in_slot(float x, float y) {
    const float hw = 8.f;
    const float hh = 5.f;
    const float sx[2] = {-15.f, 15.f};
    const float sy = 14.f;
    for (int i = 0; i < 2; ++i) {
        const float dx = std::fabs(x - sx[i]);
        const float dy = std::fabs(y - sy);
        if (dy > hh) {
            continue;
        }
        if (dx <= hw) {
            return true;
        }
        const float ex = dx - hw;
        if (ex * ex + dy * dy <= hh * hh) {
            return true;
        }
    }
    return false;
}

inline void face_rotate(float x, float y, float degrees, float& ox, float& oy) {
    const float a = degrees * 0.01745329251f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    ox = c * x - s * y;
    oy = s * x + c * y;
}
