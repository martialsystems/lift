// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <juce_graphics/juce_graphics.h>

// Constants and geometry copied from the prototype (LIFT.html). Geometry is in
// "dev" pixels: the instrument face's own CSS box (1360 x 866), whose top-left
// sits at (36, 87.5) on the page.

namespace lift::ui {

inline juce::Colour hex(juce::uint32 rgb, float a = 1.f) {
    return juce::Colour(static_cast<juce::uint8>((rgb >> 16) & 0xff), static_cast<juce::uint8>((rgb >> 8) & 0xff),
                        static_cast<juce::uint8>(rgb & 0xff), a);
}

constexpr float kDevX = 36.f;
constexpr float kDevY = 87.5f;
constexpr float kDevW = 1360.f;
constexpr float kDevH = 866.f;

constexpr juce::uint32 RED = 0xd8382b, YEL = 0xf2b705, BLU = 0x1f4e9c, BLK = 0x1b1b1b, CRM = 0xf6f2e9;
constexpr juce::uint32 SCR[4] = {0xe8473a, 0xf4be2a, 0x4c82e6, 0xede6d6};
constexpr juce::uint32 HW[4] = {RED, YEL, BLU, BLK};
constexpr juce::uint32 KNOB[4] = {0xd0261b, 0xf0b000, 0x1f4fa3, 0x171717};
inline juce::uint32 psk(juce::uint32 c) {
    switch (c) {
    case RED: return 0x8f1d14;
    case YEL: return 0xa87d00;
    case BLU: return 0x102c5c;
    default: return 0x000000;
    }
}

struct Cloth {
    const char* n;
    juce::uint32 c, dk, lt, boot;
};
constexpr Cloth CLOTH[6] = {
    {"Black", 0x2a2823, 0x0e0d0b, 0x6b665a, 0x1c1b19}, {"Red", 0xb5332a, 0x741c16, 0xe2766c, 0x8e2620},
    {"Mustard", 0xd4a224, 0x8e6a0c, 0xf5d77e, 0xa87d12}, {"Blue", 0x2c5898, 0x15366a, 0x7aa2dc, 0x1d3f73},
    {"Cream", 0xe3dac4, 0xa59a80, 0xffffff, 0xcfc4aa}, {"Green", 0x3e7d4f, 0x1f4a2b, 0x86c493, 0x2c5e39}};

struct JackDef {
    const char* n;
    char sym;
};
constexpr JackDef OUTS[16] = {{"A PITCH", 'p'}, {"A GATE", 'g'}, {"B PITCH", 'p'}, {"B GATE", 'g'}, {"DRUM", 'g'},
                              {"CLOCK", 'c'},   {"RESET", 'c'},  {"RADIO", 'a'},   {"LFO", 'm'},    {"ENV", 'm'},
                              {"S&H", 'm'},     {"VCA", 'a'},    {"SLEW", 'm'},    {"QUANT", 'p'},  {"HEAD 1", 'a'},
                              {"HEAD 2", 'a'}};
constexpr JackDef INS[16] = {{"PITCH", 'p'},  {"GATE", 'g'},   {"FM IDX", 'a'}, {"CUTOFF", 'm'}, {"SLICE", 'p'},
                             {"FX MAC", 'm'}, {"CLK IN", 'c'}, {"RST IN", 'c'}, {"VCA IN", 'a'}, {"VCA CV", 'm'},
                             {"SLEW", 'm'},   {"QUANT", 'p'},  {"S&H", 'm'},    {"SPEED", 'm'},  {"REVERSE", 'g'},
                             {"BIAS", 'm'}};

struct EngineDef {
    const char* n;
    const char* d;
    const char* m[4];
};
inline const EngineDef ENGINES[8] = {
    {"LOOM", "TWO OSCILLATORS \xc2\xb7 SAW + SQUARE \xc2\xb7 FILTER", {"DETUNE", "CUTOFF", "ENV AMT", "DECAY"}},
    {"BEND", "PHASE DISTORTION ON ONE CARRIER", {"AMOUNT", "CUTOFF", "ATTACK", "DECAY"}},
    {"FOLD", "SINE INTO A WAVEFOLDER", {"FOLD", "SYMMETRY", "OFFSET", "DECAY"}},
    {"RATIO", "FOUR SINE OPERATORS \xc2\xb7 NINE ALGORITHMS", {"ALGORITHM", "RATIO SET", "INDEX", "FEEDBACK"}},
    {"WIRE", "PLUCKED STRING", {"DAMPING", "PLUCK POS", "TONE", "DECAY"}},
    {"SWARM", "DETUNED SINES \xc2\xb7 ONE AMP ENVELOPE", {"SPREAD", "DENSITY", "ATTACK", "RELEASE"}},
    {"SPOOL", "ONE FILE \xc2\xb7 SLICED OR ROOT-MAPPED", {"START", "LENGTH", "PITCH", "DECAY"}},
    {"SPARE", "EMPTY IN THIS VERSION", {"\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94"}}};
inline const char* const KITS[8] = {"TAP", "KIT 2", "KIT 3", "KIT 4", "KIT 5", "KIT 6", "KIT 7", "KIT 8"};
inline const char* const INPUTS[8] = {"LINE", "MIC", "RADIO", "RESAMPLE", "", "", "", ""};
inline const char* const MODE_MACROS[5][4] = {{"", "", "", ""},
                                              {"SLICE", "PITCH", "CHOKE", "DECAY"},
                                              {"SPEED", "BIAS", "REC LVL", "SCRUB"},
                                              {"LEVEL", "PAN", "LOW", "HIGH"},
                                              {"STATION", "URL", "GAIN", "THRESH"}};
inline const char* const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr float ENC_DEFAULT[5][4] = {
    {0.35f, 0.62f, 0.48f, 0.55f}, {0.2f, 0.5f, 0.3f, 0.6f}, {0.5f, 0.45f, 0.7f, 0.0f}, {0.72f, 0.5f, 0.55f, 0.45f},
    {0.3f, 0.0f, 0.6f, 0.55f}};
inline const char* const AMTS[8] = {"+1.00", "+1.00", "+0.62", "\xe2\x88\x92" "0.40", "+0.80", "+0.50", "+0.25",
                                    "\xe2\x88\x92" "1.00"};
inline bool isFb(int o, int i) {
    return ((o == 14 || o == 15) && (i >= 13 || i == 2)) || (o == 11 && i == 8) || (o == 12 && i == 10) ||
           (o == 13 && i == 11) || (o == 10 && i == 12);
}

// geometry (dev pixels)
inline float jx(int i) { return 80.f + static_cast<float>(i) * 80.f; }
inline float jy(char r) { return r == 'o' ? 54.f : 122.f; }
inline juce::Point<float> knobCentre(int i) { return {684.f + static_cast<float>(i) * 160.f + 76.f, 255.75f + 70.f}; }
inline juce::Rectangle<float> padRect(int k) {
    return {684.f + static_cast<float>(k % 8) * 80.f, 446.f + static_cast<float>(k / 8) * 80.f, 72.f, 72.f};
}
// Keypad slot -> function. Functions: 0 LIFT, 1 LOOP, 2 SHIFT, 3 REV, 4 DROP,
// 5 REC, 6 OCT-, 7 PLAY, 8 OCT+, 9 STOP. SHIFT sits above STOP (top-right),
// swapped with DROP: top row LIFT LOOP DROP REV SHIFT.
constexpr int MEM_FN[10] = {0, 1, 4, 3, 2, 5, 6, 7, 8, 9};
constexpr int kShiftSlot = 4;
inline juce::Rectangle<float> memRect(int k) {
    return {72.f + static_cast<float>(k % 5) * 117.6f, 724.f + static_cast<float>(k / 5) * 54.f, 105.6f, 44.f};
}
constexpr float kNatW = (632.f - 13.f * 6.f) / 14.f;
inline juce::Rectangle<float> natRect(int j) { return {684.f + static_cast<float>(j) * (kNatW + 6.f), 754.f, kNatW, 86.f}; }
// keyboard index -> note (0..23) for naturals and sharps
constexpr int NAT_SEMI[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int SHARP_DEF[5][2] = {{1, 0}, {3, 1}, {6, 3}, {8, 4}, {10, 5}};
inline juce::Point<float> sharpCentre(int o, int b) {
    const float cx = static_cast<float>(o * 7 + SHARP_DEF[b][1] + 1) * (kNatW + 6.f) - 3.f;
    return {684.f + cx, 698.f + 2.f + 21.f};
}

}  // namespace lift::ui
