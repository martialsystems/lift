// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
// LIFT Mini knob maps (LIFT-Mini-Sheet section 4): per mode and page, the
// plain turn (A) and the SHIFT + turn (S) parameter of each knob, in colour
// order yellow, blue, black, red. "" = no parameter on that side.
#pragma once

namespace mini {

struct PageDef {
    const char* name;
    const char* a[4];
    const char* s[4];
};

inline constexpr int kMaxPages = 4;
inline constexpr int kPageCount[4] = {4, 4, 1, 3};   // SYNTH, DRUM, TAPE, MIX

inline constexpr PageDef kSynthPages[4] = {
    {"SOUND", {"PITCH", "TONE", "AMOUNT", "TIME"}, {"GLIDE", "ATTACK", "SUSTAIN", "RELEASE"}},
    {"FX", {"FX 1", "FX 2", "MIX", "FX 4"}, {"", "", "", ""}},
    {"LFO", {"RATE", "SHAPE", "DEPTH", "TARGET"}, {"SYNC", "", "TWO-WAY", "FADE"}},
    {"SEQ", {"RATE", "MODE", "CHANCE", "LENGTH"}, {"TRANSPOSE", "GATE", "VELOCITY", "SWING"}}};
inline constexpr PageDef kDrumPages[4] = {
    {"KIT", {"PITCH", "CHOKE", "DECAY", "SLICE"}, {"PLAY", "START", "GROUP", "END"}},
    {"FX", {"FX 1", "FX 2", "MIX", "FX 4"}, {"", "", "", ""}},
    {"LFO", {"RATE", "SHAPE", "DEPTH", "TARGET"}, {"SYNC", "", "TWO-WAY", "FADE"}},
    {"SEQ", {"RATE", "MODE", "CHANCE", "LENGTH"}, {"TRANSPOSE", "GATE", "VELOCITY", "SWING"}}};
inline constexpr PageDef kTapePages[1] = {
    {"TAPE", {"SPEED", "BIAS", "REC LVL", "POSITION"}, {"WOBBLE", "CHARACTER", "DRIVE", "LOOP"}}};
inline constexpr PageDef kMixPages[3] = {
    {"MIX", {"T1", "T2", "T3", "T4"}, {"PAN 1", "PAN 2", "PAN 3", "PAN 4"}},
    {"MASTER", {"HIGH", "LOW", "COMP", "FX MIX"}, {"MID", "FX TYPE", "WIDTH", "FX TIME"}},
    {"PROJECT", {"TEMPO", "PROJECT", "BOUNCES", "BOUNCE"}, {"KEY", "METRO", "IN GAIN", "COUNT-IN"}}};

inline const PageDef& pageDef(int mode, int page) {
    const int p = page < 0 ? 0 : page >= kPageCount[mode] ? kPageCount[mode] - 1 : page;
    switch (mode) {
        case 0: return kSynthPages[p];
        case 1: return kDrumPages[p];
        case 2: return kTapePages[0];
        default: return kMixPages[p];
    }
}

// Discrete parameters step one item per detent (no acceleration).
inline constexpr const char* kCharacterNames[4] = {"DECK", "POCKET", "SHED", "CAP"};
inline constexpr int kLoopBars[6] = {0, 1, 2, 4, 8, 16};

// Tempo knob: 40..260 BPM; 100 BPM is the new-project default.
inline constexpr double kTempoMin = 40.0, kTempoMax = 260.0;
inline double knobToTempo(float v) { return kTempoMin + double(v) * (kTempoMax - kTempoMin); }
inline float tempoToKnob(double bpm) { return float((bpm - kTempoMin) / (kTempoMax - kTempoMin)); }

// Encoder acceleration: a detent moves 1 % when turned slowly, up to 4 % when
// detents come quickly (a fast hand) or several arrive at once (a fast Pico).
inline float encoderAccel(double msSinceLast, int detents) {
    float a = msSinceLast < 25.0 ? 4.f : msSinceLast < 60.0 ? 2.5f : msSinceLast < 120.0 ? 1.5f : 1.f;
    const int n = detents < 0 ? -detents : detents;
    if (n >= 3 && a < 2.f) a = 2.f;
    return a;
}
inline constexpr float kSlowStep = 0.01f;

}  // namespace mini
