// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "tape/model.h"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDc = 0.995f;

float onepole_a(float hz) noexcept {
    if (hz <= 0.f) {
        return 1.f;
    }
    float a = 1.f - expf(-2.f * kPi * hz / static_cast<float>(kSampleRate));
    if (a < 0.f) {
        return 0.f;
    }
    if (a > 1.f) {
        return 1.f;
    }
    return a;
}

float onepole(float& z, float x, float a) noexcept {
    z += a * (x - z);
    return z;
}

void design_low_shelf(float f0, float q, float gainDb, BiquadCoeff& c) noexcept {
    if (gainDb == 0.f || f0 <= 0.f) {
        c.b0 = 1.f;
        c.b1 = 0.f;
        c.b2 = 0.f;
        c.a1 = 0.f;
        c.a2 = 0.f;
        return;
    }
    const float A = powf(10.f, gainDb / 40.f);
    const float w0 = 2.f * kPi * f0 / static_cast<float>(kSampleRate);
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.f * q);
    const float two = 2.f * sqrtf(A) * alpha;
    const float b0 = A * ((A + 1.f) - (A - 1.f) * cw + two);
    const float b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cw);
    const float b2 = A * ((A + 1.f) - (A - 1.f) * cw - two);
    const float a0 = (A + 1.f) + (A - 1.f) * cw + two;
    const float a1 = -2.f * ((A - 1.f) + (A + 1.f) * cw);
    const float a2 = (A + 1.f) + (A - 1.f) * cw - two;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
}

float biquad(BiquadState& s, float x, const BiquadCoeff& c) noexcept {
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

float dc_block(ChannelFilter& st, float x) noexcept {
    const float y = x - st.dcX + kDc * st.dcY;
    st.dcX = x;
    st.dcY = y;
    return y;
}

}  // namespace

void reset_filter(ChannelFilter& st) noexcept {
    st.lp = 0.f;
    st.emph = 0.f;
    st.hissLp = 0.f;
    st.env = 0.f;
    st.dcX = 0.f;
    st.dcY = 0.f;
    st.bump.z1 = 0.f;
    st.bump.z2 = 0.f;
}

void fill_coeffs(const Character& row, CharacterCoeffs& dst) noexcept {
    dst.lpA = onepole_a(row.lowpassHz);
    dst.emphA = onepole_a(1800.f);
    dst.envA = onepole_a(20.f);
    dst.wowInc = 2.f * kPi * row.wowHz / static_cast<float>(kSampleRate);
    dst.wowDepth = row.wowDepth;
    dst.flutterA = onepole_a(row.flutterHz);
    dst.flutterDepth = row.flutterDepth;
    dst.hissA = onepole_a(6000.f);
    dst.hissAmp = powf(10.f, row.hissDb / 20.f);
    dst.preEmph = row.preEmph;
    dst.drive = row.drive;
    dst.asym = row.asym;
    dst.bias = 0.f;
    design_low_shelf(row.bumpHz, row.bumpQ, row.bumpDb, dst.bump);
}

float next_white(uint32_t& rng) noexcept {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(rng)) / 2147483648.f;
}

float eco_record(ChannelFilter& st, float x, const CharacterCoeffs& c) noexcept {
    const float low = onepole(st.emph, x, c.emphA);
    const float pre = x + c.preEmph * (x - low);
    const float y = pre * c.drive + c.bias;
    const float shaped = tanhf(y) + c.asym * y * y;
    const float centered = dc_block(st, shaped);
    const float bumped = biquad(st.bump, centered, c.bump);
    return onepole(st.lp, bumped, c.lpA);
}

float eco_play(ChannelFilter& st, float x, const CharacterCoeffs& c, uint32_t& rng) noexcept {
    const float y = onepole(st.lp, x, c.lpA);
    const float white = next_white(rng);
    const float hiss = onepole(st.hissLp, white, c.hissA);
    const float envIn = y < 0.f ? -y : y;
    st.env += c.envA * (envIn - st.env);
    constexpr float kFloor = 0.35f;
    const float gain = c.hissAmp * (kFloor + (1.f - kFloor) * st.env);
    return y + hiss * gain;
}
