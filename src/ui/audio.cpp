// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "ui/audio.h"

#include "audio/process_block.h"
#include "tape/edit.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kCap = 64;

PanelCmd g_cmds[kCap];
uint32_t g_write = 0;
uint32_t g_read = 0;
TapeRuntime* g_rt = nullptr;

int g_arm = 0;
int g_playing = 0;
int g_recording = 0;
int g_reverse = 0;
int g_character = 0;
int g_frames = 0;
uint64_t g_posBits = 0;
uint32_t g_peakBits[4] = {};

void store_pos(double pos) {
    uint64_t bits = 0;
    memcpy(&bits, &pos, sizeof bits);
    __atomic_store_n(&g_posBits, bits, __ATOMIC_RELEASE);
}

double load_pos() {
    const uint64_t bits = __atomic_load_n(&g_posBits, __ATOMIC_ACQUIRE);
    double pos = 0.0;
    memcpy(&pos, &bits, sizeof pos);
    return pos;
}

void store_peak(int track, float peak) {
    uint32_t bits = 0;
    memcpy(&bits, &peak, sizeof bits);
    __atomic_store_n(&g_peakBits[track], bits, __ATOMIC_RELEASE);
}

float load_peak(int track) {
    const uint32_t bits = __atomic_load_n(&g_peakBits[track], __ATOMIC_ACQUIRE);
    float peak = 0.f;
    memcpy(&peak, &bits, sizeof peak);
    return peak;
}

void publish() {
    if (g_rt == nullptr) {
        return;
    }
    __atomic_store_n(&g_arm, g_rt->arm, __ATOMIC_RELEASE);
    __atomic_store_n(&g_playing, g_rt->playing ? 1 : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_recording, g_rt->recording ? 1 : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_reverse, g_rt->reverse ? 1 : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_character, g_rt->character, __ATOMIC_RELEASE);
    __atomic_store_n(&g_frames, g_rt->frames, __ATOMIC_RELEASE);
    store_pos(g_rt->pos);
}

void apply(const PanelCmd& cmd) {
    if (g_rt == nullptr) {
        return;
    }
    TapeRuntime& rt = *g_rt;
    switch (cmd.act) {
    case PanelAct::Arm:
        if (cmd.track >= 0 && cmd.track < kTrackCount) {
            rt.arm = cmd.track;
        }
        break;
    case PanelAct::Print:
        rt.recording = cmd.flag != 0;
        rt.overdub = cmd.flag == 2;
        if (cmd.flag != 0) {
            rt.playing = true;
        }
        break;
    case PanelAct::Play:
        rt.playing = true;
        rt.recording = false;
        break;
    case PanelAct::Stop:
        transport_stop(rt);
        break;
    case PanelAct::Lift:
        tape_lift(rt);
        break;
    case PanelAct::Drop:
        tape_drop(rt, cmd.flag != 0);
        break;
    case PanelAct::Seek:
        if (cmd.pos < 0.0) {
            rt.pos = 0.0;
        } else if (rt.frames > 0 && cmd.pos > static_cast<double>(rt.frames - 1)) {
            rt.pos = static_cast<double>(rt.frames - 1);
        } else {
            rt.pos = cmd.pos;
        }
        break;
    case PanelAct::Rev:
        transport_rev(rt);
        break;
    }
}

void drain() {
    const uint32_t r0 = __atomic_load_n(&g_read, __ATOMIC_RELAXED);
    const uint32_t w = __atomic_load_n(&g_write, __ATOMIC_ACQUIRE);
    uint32_t r = r0;
    while (r != w) {
        apply(g_cmds[r & (kCap - 1u)]);
        ++r;
    }
    if (r != r0) {
        __atomic_store_n(&g_read, r, __ATOMIC_RELEASE);
    }
}

float range_peak(const float* buf, int frames, int begin, int count) {
    if (buf == nullptr || frames <= 0 || count <= 0) {
        return 0.f;
    }
    if (begin < 0) {
        begin = 0;
    }
    if (begin >= frames) {
        return 0.f;
    }
    if (count > frames - begin) {
        count = frames - begin;
    }
    float peak = 0.f;
    for (int i = 0; i < count; ++i) {
        const float a = buf[begin + i] < 0.f ? -buf[begin + i] : buf[begin + i];
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

}  // namespace

void panel_audio_bind(TapeRuntime* rt) noexcept {
    g_rt = rt;
    __atomic_store_n(&g_write, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_read, 0u, __ATOMIC_RELAXED);
    for (int t = 0; t < 4; ++t) {
        store_peak(t, 0.f);
    }
    publish();
}

void panel_audio_push(const PanelCmd& cmd) noexcept {
    const uint32_t w = __atomic_load_n(&g_write, __ATOMIC_RELAXED);
    const uint32_t r = __atomic_load_n(&g_read, __ATOMIC_ACQUIRE);
    if (w - r >= kCap) {
        return;
    }
    g_cmds[w & (kCap - 1u)] = cmd;
    __atomic_store_n(&g_write, w + 1u, __ATOMIC_RELEASE);
}

void panel_audio_block(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept {
    drain();
    if (g_rt == nullptr) {
        return;
    }
    const double before = g_rt->pos;
    if (n > 0 && outL != nullptr && outR != nullptr) {
        process_block(*g_rt, inL, inR, outL, outR, n);
        const int begin = static_cast<int>(before);
        const int count = n > 4096 ? 4096 : n;
        for (int t = 0; t < kTrackCount; ++t) {
            store_peak(t, range_peak(g_rt->ch[t][0], g_rt->frames, begin, count));
        }
    }
    publish();
}

void panel_audio_meters(PanelMeters& out) noexcept {
    out.arm = __atomic_load_n(&g_arm, __ATOMIC_ACQUIRE);
    out.playing = __atomic_load_n(&g_playing, __ATOMIC_ACQUIRE);
    out.recording = __atomic_load_n(&g_recording, __ATOMIC_ACQUIRE);
    out.reverse = __atomic_load_n(&g_reverse, __ATOMIC_ACQUIRE);
    out.character = __atomic_load_n(&g_character, __ATOMIC_ACQUIRE);
    out.frames = __atomic_load_n(&g_frames, __ATOMIC_ACQUIRE);
    out.pos = load_pos();
    for (int t = 0; t < 4; ++t) {
        out.peak[t] = load_peak(t);
    }
}
