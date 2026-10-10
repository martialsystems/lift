// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT's patch bay (v3.1) as a signal graph, after RONIN's PatchGraph
// (Source/Modular) and the Jidai Cable Standard rules in the vendored
// jidai-common (jidai/jcs/Graph.h).
//
// Sources: the 16 OUT jacks (0..15) and the 16 matrix rows (16..31).
// Destinations: the 16 IN jacks (0..15) and the 16 matrix columns (16..31).
// A link is a cable (OUT jack -> IN jack, amount 1) or a pin (row -> column,
// amount +1, +0.5 or -1). Every destination sums its links; a column and a
// jack with the same name (PITCH, SPEED, BIAS, SCRUB, REC, GATE) are one input.
//
// Feedback: a link is a feedback link only when it closes a true cycle in the
// node graph (oldest cable first, then pins in grid order; JCS R9). Nothing
// is judged by jack name. HEAD n and the armed loop's SPEED / SCRUB / REVERSE
// are per-loop nodes, so HEAD 2 -> SPEED with T1 armed is a plain cable (loop
// 2 plays loop 1) while HEAD 1 -> SPEED with T1 armed is feedback. A feedback
// link reads the source's previous block (one block = 32 samples) through a
// soft clip (+-5 V) and a 10 Hz DC blocker; every other link is instant.
// Signals are volts (audio +-5 V = full scale, pitch 1 V/oct, gates 0 / 5 V).
//
// planPatch() runs on the message thread (it allocates); the Router copies a
// plan on the audio thread and never allocates.

#include <cstdint>

namespace lift::eng {

constexpr int kJacks = 16;
constexpr int kSrc = 32;  // OUT jacks + matrix rows
constexpr int kDst = 32;  // IN jacks + matrix columns
constexpr int kBlock = 32;
constexpr int kMaxCables = 64;
constexpr int kMaxLinks = kMaxCables + 256;

// Nodes the graph schedules, in their default (rack) order.
enum Node : int {
    N_CLOCK, N_KEYS, N_DRUMS, N_RADIO, N_MOD, N_ENV, N_INPUT, N_SH, N_SLEW, N_QUANT, N_VCA,
    N_TRANSPORT, N_H1, N_H2, N_H3, N_H4, N_SYNTH, N_FX, N_MIX, kNodes
};

// OUT jacks, panel order (rows mirror about the gutter, so A PITCH is 3).
enum OutJack : int { O_AGATE, O_BGATE, O_BPITCH, O_APITCH, O_RADIO, O_RESET, O_CLOCK, O_DRUM,
                     O_HEAD4, O_HEAD3, O_HEAD2, O_HEAD1, O_SEND, O_FXOUT, O_MIXR, O_MIXL };
enum InJack : int { I_PITCH, I_QUANT, I_GATE, I_REC, I_REVERSE, I_CLK, I_RST, I_VCAIN,
                    I_SPEED, I_BIAS, I_SCRUB, I_SLICE, I_AUDIOL, I_AUDIOR, I_SH, I_SLEW };
// Matrix rows (sources 16..31) and columns (destinations 16..31), panel order.
enum Row : int { R_LFO = 16, R_LFO2, R_ENV, R_ENV2, R_SH, R_RANDOM, R_SLEW, R_VEL,
                 R_FOLLOW, R_QUANT, R_STEP, R_PULSE, R_ACCENT, R_EOC, R_NOISE, R_VCA };
enum Col : int { C_PITCH = 16, C_SPEED, C_BIAS, C_SCRUB, C_CUTOFF, C_RESO, C_FM, C_WAVE,
                 C_DECAY, C_GATE, C_LEVEL, C_REC, C_GPOS, C_GSIZE, C_FXMAC, C_VCA };

// The node a source / destination belongs to (arm = the armed loop 0..3).
int srcNode(int s) noexcept;
int dstNode(int d, int arm) noexcept;
// Is this source a pitch (1 V/oct) signal? (A / B PITCH, QUANT, STEP.)
bool srcIsPitch(int s) noexcept;

struct PlanLink {
    std::int8_t s = 0, d = 0;  // source 0..31, destination 0..31
    bool fb = false;           // closes a cycle: one block late, soft clipped, DC blocked
    bool cable = true;         // false: a matrix pin
    float amt = 1.f;           // pins: +1, +0.5, -1 (PITCH column: x 1/12 from non-pitch rows)
};
struct PatchPlan {
    int count = 0;
    int arm = 0;
    PlanLink c[kMaxLinks];
    std::int8_t order[kNodes] = {N_CLOCK, N_KEYS, N_DRUMS, N_RADIO, N_MOD, N_ENV, N_INPUT, N_SH, N_SLEW, N_QUANT,
                                 N_VCA, N_TRANSPORT, N_H1, N_H2, N_H3, N_H4, N_SYNTH, N_FX, N_MIX};
    bool pitchRow[16] = {};  // matrix row r carries pitch (QUANT, STEP; S&H / SLEW / VCA fed by a pitch cable)
};

// Cables oldest first (o = OUT jack, i = IN jack); pins[r * 16 + c] = 0 none,
// 1 +100 %, 2 +50 %, 3 -100 % (null: none). Message thread.
PatchPlan planPatch(const int* outs, const int* ins, int n, const std::uint8_t* pins = nullptr, int arm = 0);
// Is the cable o -> i a feedback cable in this plan? (The BAY screen.)
bool cableIsFeedback(const PatchPlan& p, int o, int i) noexcept;
// Is the pin row r -> column c a feedback pin?
bool pinIsFeedback(const PatchPlan& p, int r, int c) noexcept;

class Router {
public:
    Router() noexcept;
    void setPlan(const PatchPlan& p) noexcept;
    const PatchPlan& plan() const noexcept { return plan_; }
    float* out(int src) noexcept { return out_[src]; }
    // Sum of every link into destination d for this block; null when nothing is patched.
    const float* in(int d) noexcept;
    // A jack and its same-name column as one input (null when neither is patched).
    const float* in2(int jack, int col) noexcept;
    bool patched(int d) const noexcept { return first_[d + 1] > first_[d]; }
    bool cabled(int d) const noexcept { return cables_[d] > 0; }
    bool used(int s) const noexcept { return srcUse_[s] > 0; }
    void beginBlock() noexcept;
    void endBlock() noexcept;  // keeps this block's outputs for the feedback links
    void reset() noexcept;

private:
    PatchPlan plan_;
    std::int16_t first_[kDst + 1] = {};
    std::int16_t idx_[kMaxLinks] = {};
    int cables_[kDst] = {};
    int srcUse_[kSrc] = {};
    bool inDone_[kDst] = {};
    float out_[kSrc][kBlock] = {};
    float prev_[kSrc][kBlock] = {};
    float in_[kDst][kBlock] = {};
    float sum_[kDst][kBlock] = {};
    float dc_[kMaxLinks] = {};
};

}  // namespace lift::eng
