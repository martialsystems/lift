// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <juce_graphics/juce_graphics.h>

// Constants and geometry copied from the prototype (LIFT-v3.html, v3.1).
// Geometry is in "dev" pixels: the instrument face's own CSS box (1328 x 994),
// whose top-left sits at (16, 76) on the panel canvas.

namespace lift::ui {

inline juce::Colour hex(juce::uint32 rgb, float a = 1.f) {
    return juce::Colour(static_cast<juce::uint8>((rgb >> 16) & 0xff), static_cast<juce::uint8>((rgb >> 8) & 0xff),
                        static_cast<juce::uint8>(rgb & 0xff), a);
}

constexpr float kDevX = 16.f;
constexpr float kDevY = 76.f;
constexpr float kDevW = 1328.f;
constexpr float kDevH = 994.f;
// The screen and right-hand controls were drawn for the v2 face; v3.1 moved
// that whole block by (-16, +124) (keyboard: +120 and taller keys).
constexpr float kMainDX = -16.f, kMainDY = 124.f;

constexpr juce::uint32 RED = 0xd8382b, YEL = 0xf2b705, BLU = 0x1f4e9c, BLK = 0x1b1b1b, CRM = 0xf6f2e9;
constexpr juce::uint32 SCR[4] = {0xe8473a, 0xf4be2a, 0x4c82e6, 0xede6d6};
constexpr juce::uint32 HW[4] = {RED, YEL, BLU, BLK};
// The four encoders by column on every screen: 1 frequency (yellow triangle),
// 2 tone / shape (blue circle), 3 level / amount (black diamond), 4 time /
// position (red square).
constexpr juce::uint32 KNOB[4] = {0xf0b000, 0x1f4fa3, 0x171717, 0xd0261b};
constexpr juce::uint32 KSCR[4] = {0xf4be2a, 0x5a8ff0, 0xede6d6, 0xec4b3c};  // the same, on the screen
constexpr char KSYM[4] = {'p', 'm', 'a', 'g'};
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

// Signal types: shape = type, colour = the type's family. p pitch (yellow
// triangle), g gate (red square), c clock (red hollow diamond), m mod (blue
// circle), a audio (black / white diamond).
struct TypeDef {
    const char* n;
    juce::uint32 c, d;  // printed on the case, printed on black
    int cloth;          // the cable colour this type's cables take
};
inline const TypeDef& typeOf(char t) {
    static const TypeDef P{"PITCH", 0xf2b705, 0xf4be2a, 2}, G{"GATE", 0xd8382b, 0xec4b3c, 1},
        C{"CLOCK", 0xd8382b, 0xec4b3c, 1}, M{"MOD", 0x1f4e9c, 0x5a8ff0, 3}, A{"AUDIO", 0x1b1b1b, 0xede6d6, 4};
    switch (t) {
    case 'p': return P;
    case 'g': return G;
    case 'c': return C;
    case 'm': return M;
    default: return A;
    }
}

struct JackDef {
    const char* n;
    char sym;
};
// JACKS (16 + 16). Rows read OUT -> IN across a printed arrow; rows 1-2
// mirror about the gutter so like faces like (pitch innermost).
constexpr JackDef OUTS[16] = {{"A GATE", 'g'}, {"B GATE", 'g'}, {"B PITCH", 'p'}, {"A PITCH", 'p'},
                              {"RADIO", 'a'},  {"RESET", 'c'},  {"CLOCK", 'c'},   {"DRUM", 'g'},
                              {"HEAD 4", 'a'}, {"HEAD 3", 'a'}, {"HEAD 2", 'a'},  {"HEAD 1", 'a'},
                              {"SEND", 'a'},   {"FX OUT", 'a'}, {"MIX R", 'a'},   {"MIX L", 'a'}};
constexpr JackDef INS[16] = {{"PITCH", 'p'},   {"QUANT", 'p'},  {"GATE", 'g'},   {"REC", 'g'},
                             {"REVERSE", 'g'}, {"CLK IN", 'c'}, {"RST IN", 'c'}, {"VCA IN", 'a'},
                             {"SPEED", 'm'},   {"BIAS", 'm'},   {"SCRUB", 'm'},  {"SLICE", 'p'},
                             {"AUDIO L", 'a'}, {"AUDIO R", 'a'}, {"S&H", 'm'},   {"SLEW", 'm'}};
// MATRIX (pins, 16 x 16): internal modulation sources (rows) into timbre /
// motion destinations (columns). A column and a jack of the same name sum.
// v3.1 + P4: row MOD WH became FOLLOW (envelope follower on the input),
// column DELAY became REC and column PAN became GATE (Patching Guide section
// 10, item 2), so PULSE / ACCENT / EOC (and any row) can press REC or play the
// synth GATE. They sum with the REC and GATE jacks.
constexpr JackDef MXR[16] = {{"LFO", 'm'},   {"LFO 2", 'm'}, {"ENV", 'm'},    {"ENV 2", 'm'},  {"S&H", 'm'},  {"RANDOM", 'm'},
                             {"SLEW", 'm'},  {"VEL", 'm'},   {"FOLLOW", 'm'}, {"QUANT", 'p'},  {"STEP", 'p'}, {"PULSE", 'g'},
                             {"ACCENT", 'g'}, {"EOC", 'g'},  {"NOISE", 'a'},  {"VCA", 'a'}};
constexpr JackDef MXC[16] = {{"PITCH", 'p'}, {"SPEED", 'm'}, {"BIAS", 'm'},  {"SCRUB", 'm'}, {"CUTOFF", 'm'}, {"RESO", 'm'},
                             {"FM IDX", 'm'}, {"WAVE", 'm'}, {"DECAY", 'm'}, {"GATE", 'g'},  {"LEVEL", 'm'},  {"REC", 'g'},
                             {"G POS", 'm'}, {"G SIZE", 'm'}, {"FX MAC", 'm'}, {"VCA", 'm'}};
inline int cableCloth(int out) { return typeOf(OUTS[out].sym).cloth; }  // cables take their source's colour

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
    {"GRAIN", "GRAINS OVER THE ARMED LOOP", {"PITCH", "SPRAY", "SIZE", "POS"}}};
// kit names = the drum engine's (src/engine/drums.cpp): 808 / 909 tunings and pattern variants
inline const char* const KITS[8] = {"808", "909", "808 B", "909 B", "808 C", "909 C", "808 D", "909 D"};
inline const char* const INPUTS[8] = {"LINE", "MIC", "RADIO", "RESAMPLE", "", "", "", ""};
// Engine order of each screen's four macros (what UiState and the processor
// store, unchanged since v2).
inline const char* const MODE_MACROS[5][4] = {{"", "", "", ""},
                                              {"SLICE", "PITCH", "CHOKE", "DECAY"},
                                              {"SPEED", "BIAS", "REC LVL", "SCRUB"},
                                              {"LEVEL", "PAN", "LOW", "HIGH"},
                                              {"STATION", "URL", "GAIN", "THRESH"}};
// Physical knob k (by colour: yellow, blue, black, red) -> engine macro slot.
// DRUM: PITCH SLICE CHOKE DECAY; MIX: HIGH LOW LEVEL PAN; SPOOL: PITCH DECAY
// LENGTH START (yellow pitch, blue tone/shape, black amount, red time).
inline int knobSlot(int mode, int synthEngine, int k) {
    static constexpr int DRUM[4] = {1, 0, 2, 3}, MIX[4] = {3, 2, 0, 1}, SPOOL[4] = {2, 3, 1, 0};
    if (mode == 1) return DRUM[k];
    if (mode == 3) return MIX[k];
    if (mode == 0 && synthEngine == 6) return SPOOL[k];
    return k;
}
inline const char* const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr float ENC_DEFAULT[5][4] = {
    {0.35f, 0.62f, 0.48f, 0.55f}, {0.2f, 0.5f, 0.3f, 0.6f}, {0.5f, 0.45f, 0.7f, 0.0f}, {0.72f, 0.5f, 0.55f, 0.45f},
    {0.3f, 0.0f, 0.6f, 0.55f}};

// ---- geometry (dev pixels), v3.1
// patch bay: matrix left (cols 1-8 of the case grid), jacks right
constexpr float kBayX = 28.f, kBayY = 20.f, kBayW = 1272.f, kBayH = 286.f;
constexpr float kMxX = 126.f, kMxY = 55.f, kMxW = 30.f, kMxH = 15.f;  // pin grid origin and cell
inline juce::Rectangle<float> pinCell(int r, int c) {
    return {kMxX + static_cast<float>(c) * kMxW, kMxY + static_cast<float>(r) * kMxH, kMxW, kMxH};
}
inline juce::Point<float> jackCentre(char r, int i) {
    return {(r == 'o' ? 703.f : 1055.f) + static_cast<float>(i % 4) * 70.f, 69.f + static_cast<float>(i / 4) * 60.f};
}
constexpr float kJackR = 13.f;
inline juce::Point<float> knobCentre(int i) { return {674.f + static_cast<float>(i) * 160.f + 70.f, 379.75f + 70.f}; }
inline juce::Rectangle<float> padRect(int k) {
    return {668.f + static_cast<float>(k % 8) * 80.f, 570.f + static_cast<float>(k / 8) * 80.f, 72.f, 72.f};
}
// Push-encoders (bottom left, on the case). Slot -> function: 0 LIFT, 1 LOOP,
// 2 SHIFT, 3 REV, 4 DROP, 5 REC, 6 OCT-, 7 PLAY, 8 OCT+, 9 STOP. Top row
// LIFT LOOP DROP REV SHIFT.
constexpr int MEM_FN[10] = {0, 1, 4, 3, 2, 5, 6, 7, 8, 9};
constexpr int kShiftSlot = 4;
inline juce::Point<float> encCentre(int k) {
    return {110.39f + static_cast<float>(k % 5) * 116.8f, 852.f + static_cast<float>(k / 5) * 80.f};
}
inline juce::Rectangle<float> memRect(int k) {  // the encoder's 48 x 48 box
    const auto c = encCentre(k);
    return {c.x - 24.f, c.y - 24.f, 48.f, 48.f};
}
constexpr float kNatW = (632.f - 13.f * 6.f) / 14.f;
inline juce::Rectangle<float> natRect(int j) { return {668.f + static_cast<float>(j) * (kNatW + 6.f), 874.f, kNatW, 100.f}; }
// keyboard index -> note (0..23) for naturals and sharps
constexpr int NAT_SEMI[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int SHARP_DEF[5][2] = {{1, 0}, {3, 1}, {6, 3}, {8, 4}, {10, 5}};
inline juce::Point<float> sharpCentre(int o, int b) {
    const float cx = static_cast<float>(o * 7 + SHARP_DEF[b][1] + 1) * (kNatW + 6.f) - 3.f;
    return {668.f + cx, 820.f + 21.f};
}

}  // namespace lift::ui
