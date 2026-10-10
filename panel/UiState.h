// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "PatchBay.h"
#include "engine/drums.h"
#include "engine/fx.h"

#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <vector>

namespace lift {

// Everything the panel shows that a save slot keeps, apart from the tape audio:
// screen, knobs on every screen, keypad/pad states, track arm and mutes, the
// shift-layer settings, the patch bay (cables, colours, stackables), synth
// engine and drum kit (sel[0] and sel[1]) and the MIDI learn map.
//
// The processor holds the authoritative copy (so a plug-in without its editor
// open still saves and restores everything); the panel mirrors it.
struct UiState {
    static constexpr int kKnobTargets = 20;  // 5 screens x 4 knobs; target = screen * 4 + knob

    int mode = 2;  // 0 SYNTH, 1 DRUM, 2 TAPE, 3 MIX, 4 IN
    bool bay = false;
    bool fx = false;  // init is dry: the FX pad starts bypassed
    std::array<int, 5> sel{{0, 0, 0, 0, 2}};
    std::array<std::array<float, 4>, 5> enc{};
    bool loop = true;
    int loopIn = 0;
    int loopOut = 8 * 48000;
    std::array<int, 4> marks{{0, 0, 0, 0}};
    int nextMark = 0;
    int arm = 0;
    std::array<bool, 4> mutes{{false, false, true, false}};
    int oct = 0;
    int transpose = 0;
    int seqDiv = 16;
    int drumDiv = 16;
    int drumLen = 16;
    int swing = 0;
    int recSource = 0;
    int character = 0;
    bool cassette = false;  // the cassette stage (off: the looper plays clean)
    int color = 1;
    bool stack = false;
    std::vector<Cord> cords;
    // the v3.1 pin matrix: [row * 16 + column], 0 none, 1 +100 %, 2 +50 %, 3 -100 %
    std::array<std::uint8_t, 256> pins{};
    // REC jack option (P4): the REC jack presses LIFT instead of REC
    bool recJackLifts = false;
    std::array<int, 128> learn;  // CC number -> knob target, -1 = not learned
    // DRUM: step patterns per kit and voice (bit s = step s), the voice the
    // DRUM knobs play, and each voice's PITCH / CHOKE / DECAY knob.
    std::array<std::array<uint32_t, eng::kDrumVoices>, eng::kKits> drumPat{};
    int drumVoice = 0;
    std::array<std::array<float, 3>, eng::kDrumVoices> drumKnobs{};
    // FX: the insert's type and its four knobs per type
    int fxType = 0;
    std::array<std::array<float, 4>, eng::kFxTypes> fxKnobs{};

    UiState();
    bool operator==(const UiState&) const = default;

    juce::ValueTree toTree() const;
    // Missing or out-of-range values keep their defaults.
    static UiState fromTree(const juce::ValueTree& t);
    static bool remapV2(Cord& c, std::array<std::uint8_t, 256>& pins);
};

}  // namespace lift
