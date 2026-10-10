// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT's patch bay as a signal graph, after RONIN's PatchGraph (Source/Modular)
// and the Jidai Cable Standard rules in the vendored jidai-common
// (jidai/jcs/Graph.h): every cable is a zero-delay connection unless it closes
// a loop, judged oldest cable first (JCS R9); a loop cable reads the source's
// previous block (one block = 32 samples, z^-32 here instead of RONIN's one
// sample) through a soft clip and a DC blocker, so feedback patches stay
// bounded. Inputs sum. Signals are volts (audio +-5 V = full scale).
//
// planPatch() runs on the message thread (it allocates); the Router copies a
// plan on the audio thread and never allocates.

#include <cstdint>

namespace lift::eng {

constexpr int kJacks = 16;
constexpr int kBlock = 32;
constexpr int kMaxCables = 64;

// Nodes the graph schedules, in their default (rack) order.
enum Node : int { N_CLOCK, N_KEYS, N_DRUMS, N_RADIO, N_LFO, N_ENV, N_SH, N_VCA, N_SLEW, N_QUANT, N_SYNTH, N_FX, N_TAPE, kNodes };

enum OutJack : int { O_APITCH, O_AGATE, O_BPITCH, O_BGATE, O_DRUM, O_CLOCK, O_RESET, O_RADIO,
                     O_LFO, O_ENV, O_SH, O_VCA, O_SLEW, O_QUANT, O_HEAD1, O_HEAD2 };
enum InJack : int { I_PITCH, I_GATE, I_FM, I_CUTOFF, I_SLICE, I_FXMAC, I_CLK, I_RST,
                    I_VCAIN, I_VCACV, I_SLEW, I_QUANT, I_SH, I_SPEED, I_REVERSE, I_BIAS };

extern const int kOutNode[kJacks];
extern const int kInNode[kJacks];

struct PlanCable {
    std::int8_t o = 0, i = 0;
    bool fb = false;
};
struct PatchPlan {
    int count = 0;
    PlanCable c[kMaxCables];
    std::int8_t order[kNodes] = {};
    float amount[kMaxCables] = {};  // attenuation per cable: LIFT's cords have none, so 1.0 (100 %)
};

// Cables oldest first (o = output jack, i = input jack). Message thread.
PatchPlan planPatch(const int* outs, const int* ins, int n);
// Is cable k of this list a feedback (delayed) cable? Same rule, for the BAY screen.
bool cableIsFeedback(const PatchPlan& p, int o, int i) noexcept;

class Router {
public:
    void setPlan(const PatchPlan& p) noexcept;
    const PatchPlan& plan() const noexcept { return plan_; }
    float* out(int jack) noexcept { return out_[jack]; }
    // Sum of every cable into input jack j for this block; null when nothing is plugged in.
    const float* in(int jack) noexcept;
    bool patched(int jack) const noexcept { return inCount_[jack] > 0; }
    bool outUsed(int jack) const noexcept { return outCount_[jack] > 0; }
    void beginBlock() noexcept;
    void endBlock() noexcept;  // keeps this block's outputs for the loop cables
    void reset() noexcept;

private:
    PatchPlan plan_;
    int inCount_[kJacks] = {};
    int outCount_[kJacks] = {};
    bool inDone_[kJacks] = {};
    float out_[kJacks][kBlock] = {};
    float prev_[kJacks][kBlock] = {};
    float in_[kJacks][kBlock] = {};
    float dc_[kJacks][kJacks] = {};  // DC blocker state per (out, in) loop cable
};

}  // namespace lift::eng
