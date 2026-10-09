// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "tape/transport.h"

#include <cmath>

namespace {

constexpr int kFade = 256;

double wrap_loop(double pos, int loopStart, int loopEnd) noexcept {
    const double len = static_cast<double>(loopEnd - loopStart);
    double rel = pos - static_cast<double>(loopStart);
    rel = rel - floor(rel / len) * len;
    if (rel < 0.0) {
        rel += len;
    }
    return static_cast<double>(loopStart) + rel;
}

float lerp_clamp(const float* buf, int frames, double pos) noexcept {
    if (frames <= 0) {
        return 0.f;
    }
    if (pos < 0.0) {
        pos = 0.0;
    }
    const double last = static_cast<double>(frames - 1);
    if (pos > last) {
        pos = last;
    }
    const int i = static_cast<int>(pos);
    const float f = static_cast<float>(pos - static_cast<double>(i));
    const float a = buf[i];
    const float b = buf[i + 1 < frames ? i + 1 : i];
    return a * (1.f - f) + b * f;
}

}  // namespace

void transport_init(TapeRuntime& rt) noexcept {
    for (int t = 0; t < kTrackCount; ++t) {
        rt.ch[t][0] = nullptr;
        rt.ch[t][1] = nullptr;
        rt.mute[t] = false;
        rt.fader[t] = 1.f;
    }
    rt.frames = 0;
    rt.pos = 0.0;
    rt.varispeed = 1.f;
    rt.playing = false;
    rt.recording = false;
    rt.reverse = false;
    rt.arm = 0;
    rt.loopStart = 0;
    rt.loopEnd = 0;
    rt.character = 0;
    rt.metronome = false;
    rt.resampleInput = false;
    rt.metroCounter = 0;
    rt.metroPeriod = kSampleRate / 2;
    rt.rng = 1u;
    rt.wowPhase = 0.f;
    rt.flutter = 0.f;
    rt.clipped = false;
    rt.clip[0] = nullptr;
    rt.clip[1] = nullptr;
    rt.clipFrames = 0;
    transport_bind_character(rt, 0);
    transport_reset_filters(rt);
}

void transport_bind_character(TapeRuntime& rt, int index) noexcept {
    if (index < 0 || index >= kCharacterCount) {
        index = 0;
    }
    rt.character = index;
    fill_coeffs(character_row(index), rt.coeff);
}

void transport_set_varispeed(TapeRuntime& rt, float speed) noexcept {
    if (speed < kMinVarispeed) {
        speed = kMinVarispeed;
    }
    if (speed > kMaxVarispeed) {
        speed = kMaxVarispeed;
    }
    rt.varispeed = speed;
}

void transport_rev(TapeRuntime& rt) noexcept {
    rt.reverse = !rt.reverse;
}

void transport_stop(TapeRuntime& rt) noexcept {
    rt.playing = false;
    rt.recording = false;
    rt.reverse = false;
}

void transport_scrub(TapeRuntime& rt, double delta) noexcept {
    if (rt.playing || rt.recording) {
        return;
    }
    rt.pos += delta;
    if (rt.loopEnd > rt.loopStart) {
        rt.pos = wrap_loop(rt.pos, rt.loopStart, rt.loopEnd);
        return;
    }
    if (rt.pos < 0.0) {
        rt.pos = 0.0;
    }
    if (rt.frames > 0 && rt.pos > static_cast<double>(rt.frames - 1)) {
        rt.pos = static_cast<double>(rt.frames - 1);
    }
}

void transport_reset_filters(TapeRuntime& rt) noexcept {
    reset_filter(rt.rec[0]);
    reset_filter(rt.rec[1]);
    for (int t = 0; t < kTrackCount; ++t) {
        reset_filter(rt.play[t][0]);
        reset_filter(rt.play[t][1]);
    }
    rt.flutter = 0.f;
}

void transport_advance(TapeRuntime& rt, double rate) noexcept {
    rt.pos += rate;
    if (rt.loopEnd > rt.loopStart) {
        rt.pos = wrap_loop(rt.pos, rt.loopStart, rt.loopEnd);
        return;
    }
    if (rt.pos < 0.0) {
        rt.pos = 0.0;
    }
    if (rt.frames > 0 && rt.pos >= static_cast<double>(rt.frames)) {
        rt.pos = static_cast<double>(rt.frames);
        rt.clipped = true;
    }
}

float read_looped(const float* buf, int frames, double pos, int loopStart, int loopEnd, bool reverse) noexcept {
    if (buf == nullptr || frames <= 0) {
        return 0.f;
    }
    if (loopEnd <= loopStart) {
        return lerp_clamp(buf, frames, pos);
    }
    const double len = static_cast<double>(loopEnd - loopStart);
    const int fadeN = kFade < (loopEnd - loopStart) / 2 ? kFade : (loopEnd - loopStart) / 2;
    const double fade = static_cast<double>(fadeN);
    const double rel = wrap_loop(pos, loopStart, loopEnd) - static_cast<double>(loopStart);
    const double dryPos = static_cast<double>(loopStart) + rel;
    if (fade < 2.0) {
        return lerp_clamp(buf, frames, dryPos);
    }
    if (!reverse && rel > len - fade) {
        const double into = rel - (len - fade);
        const float t = static_cast<float>(into / fade);
        double wetPos = static_cast<double>(loopStart) + into - fade;
        if (wetPos < static_cast<double>(loopStart)) {
            wetPos = static_cast<double>(loopStart);
        }
        const float dry = lerp_clamp(buf, frames, dryPos);
        const float wet = lerp_clamp(buf, frames, wetPos);
        return dry * (1.f - t) + wet * t;
    }
    if (reverse && rel < fade) {
        const float t = static_cast<float>((fade - rel) / fade);
        double wetPos = static_cast<double>(loopEnd) - rel;
        if (wetPos > static_cast<double>(loopEnd - 1)) {
            wetPos = static_cast<double>(loopEnd - 1);
        }
        if (wetPos < static_cast<double>(loopStart)) {
            wetPos = static_cast<double>(loopStart);
        }
        const float dry = lerp_clamp(buf, frames, dryPos);
        const float wet = lerp_clamp(buf, frames, wetPos);
        return dry * (1.f - t) + wet * t;
    }
    return lerp_clamp(buf, frames, dryPos);
}
