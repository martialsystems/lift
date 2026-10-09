// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/engine.h"
#include "tape/model.h"

constexpr int kRecIdxRing = 1024;

struct TapeRuntime {
    float* ch[kTrackCount][2];
    int frames;
    double pos;
    float varispeed;
    bool playing;
    bool recording;
    bool reverse;
    bool mute[kTrackCount];
    float fader[kTrackCount];
    int arm;
    int loopStart;
    int loopEnd;
    int character;
    bool metronome;
    bool resampleInput;
    int metroCounter;
    int metroPeriod;
    uint32_t rng;
    float wowPhase;
    float flutter;
    bool clipped;
    CharacterCoeffs coeff;
    ChannelFilter rec[2];
    ChannelFilter play[kTrackCount][2];
    float* clip[2];
    int clipFrames;
    TapeEngine engine;
    bool overdub;
    bool recWas;
    int recCount;
    int recIdx[kRecIdxRing];
};

void transport_init(TapeRuntime& rt) noexcept;
void transport_bind_character(TapeRuntime& rt, int index) noexcept;
void transport_set_varispeed(TapeRuntime& rt, float speed) noexcept;
void transport_rev(TapeRuntime& rt) noexcept;
void transport_stop(TapeRuntime& rt) noexcept;
void transport_scrub(TapeRuntime& rt, double delta) noexcept;
void transport_reset_filters(TapeRuntime& rt) noexcept;
// Tape stop and start ramps. Pitch and level follow the reels; a finished stop
// leaves the transport stopped. A stop while recording stops at once.
void transport_tape_stop(TapeRuntime& rt, float seconds) noexcept;
void transport_tape_start(TapeRuntime& rt, float seconds) noexcept;

float read_looped(const float* buf, int frames, double pos, int loopStart, int loopEnd, bool reverse) noexcept;
void transport_advance(TapeRuntime& rt, double rate) noexcept;
double transport_next_pos(const TapeRuntime& rt, double pos, double rate) noexcept;
