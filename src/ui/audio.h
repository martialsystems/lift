// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/transport.h"

// Commands are applied on the audio thread. The caller does not touch the tape.

enum class PanelAct : uint8_t { Arm = 1, Print = 2, Play = 3, Stop = 4, Lift = 5, Drop = 6, Seek = 7, Rev = 8 };

struct PanelCmd {
    PanelAct act;
    int track;
    int flag;
    double pos;
};

struct PanelMeters {
    int arm;
    int playing;
    int recording;
    int reverse;
    int character;
    double pos;
    int frames;
    float peak[4];
};

void panel_audio_bind(TapeRuntime* rt) noexcept;
void panel_audio_push(const PanelCmd& cmd) noexcept;
void panel_audio_block(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;
void panel_audio_meters(PanelMeters& out) noexcept;
