// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "tape/edit.h"

namespace {

int edge_len(int n) noexcept {
    int edge = kEdgeFrames;
    if (edge > n / 2) {
        edge = n / 2;
    }
    if (edge < 0) {
        edge = 0;
    }
    return edge;
}

float edge_gain(int i, int n, int edge) noexcept {
    if (edge <= 0) {
        return 1.f;
    }
    if (i < edge) {
        return static_cast<float>(i) / static_cast<float>(edge);
    }
    if (i >= n - edge) {
        return static_cast<float>(n - 1 - i) / static_cast<float>(edge);
    }
    return 1.f;
}

}  // namespace

void tape_lift(TapeRuntime& rt) {
    if (rt.clip[0] == nullptr || rt.ch[rt.arm][0] == nullptr || rt.frames <= 0) {
        return;
    }
    int start = 0;
    int end = rt.frames;
    if (rt.loopEnd > rt.loopStart) {
        start = rt.loopStart;
        end = rt.loopEnd;
        if (end > rt.frames) {
            end = rt.frames;
        }
    }
    const int n = end - start;
    if (n <= 0 || n > rt.frames) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        rt.clip[0][i] = rt.ch[rt.arm][0][start + i];
        rt.clip[1][i] = rt.ch[rt.arm][1][start + i];
    }
    rt.clipFrames = n;
}

void tape_drop(TapeRuntime& rt, bool overdub) {
    if (rt.clipFrames <= 0 || rt.clip[0] == nullptr || rt.ch[rt.arm][0] == nullptr) {
        return;
    }
    const int start = static_cast<int>(rt.pos);
    if (start < 0) {
        return;
    }
    const int edge = edge_len(rt.clipFrames);
    for (int i = 0; i < rt.clipFrames; ++i) {
        const int dest = start + i;
        if (dest < 0 || dest >= rt.frames) {
            rt.clipped = true;
            break;
        }
        const float g = edge_gain(i, rt.clipFrames, edge);
        for (int c = 0; c < 2; ++c) {
            float* track = rt.ch[rt.arm][c];
            const float sample = rt.clip[c][i];
            if (overdub) {
                track[dest] += sample * g;
            } else if (g >= 1.f) {
                track[dest] = sample;
            } else {
                track[dest] = track[dest] * (1.f - g) + sample * g;
            }
        }
    }
}
