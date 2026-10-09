// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Musical rows. Runtime coefficients are filled off the audio thread.

struct Character {
    const char* name;
    float wowHz;
    float wowDepth;
    float flutterHz;
    float flutterDepth;
    float bumpHz;
    float bumpDb;
    float bumpQ;
    float hissDb;
    float lowpassHz;
    float preEmph;
    float drive;
    float asym;
};

constexpr int kCharacterCount = 4;
constexpr int kSampleRate = 48000;
constexpr int kTrackCount = 4;
constexpr int kDefaultFrames = kSampleRate * 6 * 60;
constexpr int kProjectVersion = 1;
constexpr float kRecordRate = 1.f;
constexpr float kMinVarispeed = 0.25f;
constexpr float kMaxVarispeed = 4.f;
constexpr int kEdgeFrames = 240;

const Character& character_row(int index);
int character_index(const char* name);
