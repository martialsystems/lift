// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <string>
#include <vector>

enum class DecodeStatus { Ok, Format, Duration, Io };

struct Decoded {
    std::vector<float> left;
    std::vector<float> right;
    int rate = 0;
};

// Disk thread. Wav, aif, flac, and mp3. A file past 30 minutes is refused.
DecodeStatus decode_file(const char* path, Decoded& out, char* reason, int reasonLen);

bool write_stereo_wav(const char* path, const float* left, const float* right, int frames);
bool read_stereo_wav(const char* path, float* left, float* right, int frames);
