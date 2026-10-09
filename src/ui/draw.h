// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "ui/face.h"

constexpr int kPanelW = 960;
constexpr int kPanelH = 540;
constexpr int kPanelKeys = 24;

struct PanelRect {
    int x;
    int y;
    int w;
    int h;
};

enum class PanelHitKind { None, Track, Lift, Drop, Rec, Play, Stop, Rev, Over, Key };

struct PanelHit {
    PanelHitKind kind;
    int index;
};

struct PanelLayout {
    PanelRect screen;
    PanelRect mascot;
    PanelRect tracks[4];
    PanelRect enc[4];
    PanelRect modes[5];
    PanelRect keys[kPanelKeys];
    PanelRect lift;
    PanelRect drop;
    PanelRect rec;
    PanelRect play;
    PanelRect stop;
    PanelRect rev;
    PanelRect over;
};

struct PanelDrawIn {
    int baseColor;
    int playing;
    int recording;
    int reverse;
    int overdub;
    int arm;
    double pos;
    double seconds;
    int frames;
    float peak[4];
    int keyUsed[kPanelKeys];
    char keyName[kPanelKeys][32];
    const char* character;
};

void panel_layout(PanelLayout& layout);
PanelHit panel_hit(int x, int y);
int panel_playhead_x(const PanelRect& track, double pos, int frames);
void panel_key_mark(const PanelRect& key, int& x, int& y);
void panel_draw(const PanelDrawIn& in, uint8_t* rgba, int w, int h);
int panel_write_ppm(const char* path, const uint8_t* rgba, int w, int h);
