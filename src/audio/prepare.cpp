// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/prepare.h"

#include <cstddef>

void prepare_tracks(TapeRuntime& rt, int frames) {
    release_tracks(rt);
    if (frames <= 0) {
        return;
    }
    rt.frames = frames;
    for (int t = 0; t < kTrackCount; ++t) {
        rt.ch[t][0] = new float[static_cast<size_t>(frames)]();
        rt.ch[t][1] = new float[static_cast<size_t>(frames)]();
    }
    rt.clip[0] = new float[static_cast<size_t>(frames)]();
    rt.clip[1] = new float[static_cast<size_t>(frames)]();
    rt.clipFrames = 0;
    tape_engine_prepare(rt.engine, static_cast<double>(kSampleRate));
}

void release_tracks(TapeRuntime& rt) {
    for (int t = 0; t < kTrackCount; ++t) {
        delete[] rt.ch[t][0];
        delete[] rt.ch[t][1];
        rt.ch[t][0] = nullptr;
        rt.ch[t][1] = nullptr;
    }
    delete[] rt.clip[0];
    delete[] rt.clip[1];
    rt.clip[0] = nullptr;
    rt.clip[1] = nullptr;
    rt.clipFrames = 0;
    rt.frames = 0;
    tape_engine_release(rt.engine);
}
