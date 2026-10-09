// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <cstdint>

enum class PoolStatus { Ok, Warn, Duplicate, OverCap, Duration, Format, Io };

constexpr uint64_t kPoolWarnBytes = 1610612736ull;
constexpr uint64_t kPoolCapBytes = 2147483648ull;
constexpr int kPoolMaxFrames = 48000 * 60 * 30;
constexpr int kPoolSlices = 24;
constexpr int kCaptureFrames = 48000 * 20;

struct PoolItem {
    char hash[65];
    char name[128];
    char map[16];
    int root;
    int slices;
    int frames;
    int rate;
    uint64_t bytes;
};

PoolStatus pool_admit(uint64_t existing, uint64_t add);
void pool_slice(int frames, int slices, int key, int& start, int& length);
float pool_pitch_ratio(int root, int note);
void pool_hash_bytes(const void* data, int n, char hex[65]);

PoolStatus pool_import(const char* project, const char* path, PoolItem* out, char* reason, int reasonLen);
PoolStatus pool_add_pcm(
    const char* project,
    const char* name,
    const float* left,
    const float* right,
    int frames,
    PoolItem* out,
    char* reason,
    int reasonLen);
int pool_load(const char* project, PoolItem* items, int cap);
PoolStatus pool_set_map(const char* project, const char* hash, const char* map, int root);
PoolStatus pool_read(const char* project, const char* hash, float* left, float* right, int frames);
uint64_t pool_bytes(const char* project);
