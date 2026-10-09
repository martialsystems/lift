// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Tune, read, and capture. A directory entry is a name, a URL, and a codec hint.

struct RadioRequest {
    const char* name;
    const char* url;
    const char* codec;
    const char* project;
};

struct RadioEntry {
    char name[128];
    char url[512];
    char codec[16];
};

int lift_radio_tune(const RadioRequest* request);

// n is frames. dst is interleaved stereo, two floats per frame.
// 0: the ring had n frames. 1: not ready. 2: underrun, last sample repeated.
int lift_radio_read(float* dst, int n) noexcept;

// Copies up to 20 seconds into the pool. 0: stored to the cap. 2: stored a shorter take. 1: nothing stored.
int lift_radio_capture(float* dst, int n);

int radio_load_directory(const char* path, RadioEntry* entries, int cap);
