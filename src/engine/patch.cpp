// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/patch.h"

#include <jidai/jcs/Graph.h>

#include <cmath>
#include <vector>

namespace lift::eng {

namespace {
constexpr int kOutNode[kJacks] = {N_KEYS, N_KEYS, N_KEYS, N_KEYS, N_RADIO, N_CLOCK, N_CLOCK, N_DRUMS,
                                  N_H4,   N_H3,   N_H2,   N_H1,   N_MIX,   N_FX,    N_MIX,   N_MIX};
constexpr int kRowNode[16] = {N_MOD,  N_MOD,   N_ENV, N_ENV,   N_SH,        N_MOD, N_SLEW, N_KEYS,
                              N_INPUT, N_QUANT, N_KEYS, N_CLOCK, N_KEYS, N_TRANSPORT, N_MOD, N_VCA};
constexpr int kInNode[kJacks] = {N_SYNTH, N_QUANT, N_SYNTH, N_MIX,   -1 /* REVERSE: armed loop */, N_CLOCK, N_CLOCK, N_VCA,
                                 -1,      N_MIX,   -1,      N_DRUMS, N_INPUT, N_INPUT, N_SH, N_SLEW};
constexpr int kColNode[16] = {N_SYNTH, -1, N_MIX, -1, N_SYNTH, N_SYNTH, N_SYNTH, N_SYNTH,
                              N_SYNTH, N_SYNTH, N_SYNTH, N_MIX, N_SYNTH, N_SYNTH, N_FX, N_VCA};

// In-device order: what each node reads without a cable (normals and the
// fixed signal path). These are never feedback.
std::vector<jidai::jcs::GraphEdge> fixedEdges() {
    std::vector<jidai::jcs::GraphEdge> e = {
        {N_CLOCK, N_KEYS}, {N_CLOCK, N_DRUMS}, {N_CLOCK, N_SH},  {N_CLOCK, N_MOD},   {N_KEYS, N_ENV},
        {N_KEYS, N_SYNTH}, {N_ENV, N_VCA},     {N_MOD, N_SH},    {N_SH, N_QUANT},    {N_SYNTH, N_FX},
        {N_DRUMS, N_FX},   {N_FX, N_MIX},      {N_INPUT, N_MIX}, {N_RADIO, N_INPUT}, {N_KEYS, N_DRUMS}};
    for (int t = 0; t < 4; ++t) {
        e.push_back({N_TRANSPORT, N_H1 + t});
        e.push_back({N_H1 + t, N_MIX});
    }
    return e;
}
}  // namespace

int srcNode(int s) noexcept {
    if (s < 0 || s >= kSrc) return -1;
    return s < kJacks ? kOutNode[s] : kRowNode[s - kJacks];
}

int dstNode(int d, int arm) noexcept {
    if (d < 0 || d >= kDst) return -1;
    const int n = d < kJacks ? kInNode[d] : kColNode[d - kJacks];
    // SPEED, SCRUB and REVERSE move the armed loop: that loop's own node
    return n >= 0 ? n : N_H1 + (arm >= 0 && arm < 4 ? arm : 0);
}

bool srcIsPitch(int s) noexcept { return s == O_APITCH || s == O_BPITCH || s == R_QUANT || s == R_STEP; }

PatchPlan planPatch(const int* outs, const int* ins, int n, const std::uint8_t* pins, int arm) {
    PatchPlan p;
    p.arm = arm >= 0 && arm < 4 ? arm : 0;
    std::vector<jidai::jcs::GraphEdge> links;
    for (int k = 0; k < n && p.count < kMaxCables; ++k) {
        if (outs[k] < 0 || outs[k] >= kJacks || ins[k] < 0 || ins[k] >= kJacks) {
            continue;
        }
        PlanLink& l = p.c[p.count++];
        l.s = static_cast<std::int8_t>(outs[k]);
        l.d = static_cast<std::int8_t>(ins[k]);
        l.cable = true;
        l.amt = 1.f;
        links.push_back({srcNode(l.s), dstNode(l.d, p.arm)});
        // S&H, SLEW and VCA pass their input's type on: fed pitch, their row is pitch
        if (srcIsPitch(l.s)) {
            if (l.d == I_SH) p.pitchRow[R_SH - kJacks] = true;
            if (l.d == I_SLEW) p.pitchRow[R_SLEW - kJacks] = true;
            if (l.d == I_VCAIN) p.pitchRow[R_VCA - kJacks] = true;
        }
    }
    p.pitchRow[R_QUANT - kJacks] = p.pitchRow[R_STEP - kJacks] = true;
    if (pins != nullptr) {
        static constexpr float kAmt[4] = {0.f, 1.f, 0.5f, -1.f};
        for (int r = 0; r < 16; ++r) {
            for (int c = 0; c < 16; ++c) {
                const int k = pins[r * 16 + c];
                if (k <= 0 || k > 3 || p.count >= kMaxLinks) continue;
                PlanLink& l = p.c[p.count++];
                l.s = static_cast<std::int8_t>(kJacks + r);
                l.d = static_cast<std::int8_t>(kJacks + c);
                l.cable = false;
                l.amt = kAmt[k];
                // PITCH column: pitch rows track exactly (1 V/oct); any other row
                // moves it 1 V = 1 semitone, so vibrato stays usable
                if (l.d == C_PITCH && !p.pitchRow[r]) l.amt /= 12.f;
                links.push_back({srcNode(l.s), dstNode(l.d, p.arm)});
            }
        }
    }
    const auto fixed = fixedEdges();
    const std::vector<char> fb = jidai::jcs::classifyFeedback(kNodes, fixed, links);
    for (int k = 0; k < p.count; ++k) {
        p.c[k].fb = fb[static_cast<size_t>(k)] != 0;
    }
    const std::vector<int> order = jidai::jcs::runOrder(kNodes, fixed, links, fb);
    for (int k = 0; k < kNodes; ++k) {
        p.order[k] = static_cast<std::int8_t>(k < static_cast<int>(order.size()) ? order[static_cast<size_t>(k)] : k);
    }
    return p;
}

bool cableIsFeedback(const PatchPlan& p, int o, int i) noexcept {
    for (int k = 0; k < p.count; ++k) {
        if (p.c[k].cable && p.c[k].s == o && p.c[k].d == i) return p.c[k].fb;
    }
    return false;
}

bool pinIsFeedback(const PatchPlan& p, int r, int c) noexcept {
    for (int k = 0; k < p.count; ++k) {
        if (!p.c[k].cable && p.c[k].s == kJacks + r && p.c[k].d == kJacks + c) return p.c[k].fb;
    }
    return false;
}

Router::Router() noexcept { setPlan(PatchPlan{}); }

void Router::setPlan(const PatchPlan& p) noexcept {
    plan_ = p;
    // counting sort of the links by destination
    int cnt[kDst + 1] = {};
    for (int d = 0; d < kDst; ++d) cables_[d] = 0;
    for (int s = 0; s < kSrc; ++s) srcUse_[s] = 0;
    for (int k = 0; k < plan_.count; ++k) {
        ++cnt[plan_.c[k].d + 1];
        if (plan_.c[k].cable) ++cables_[plan_.c[k].d];
        ++srcUse_[plan_.c[k].s];
    }
    first_[0] = 0;
    for (int d = 0; d < kDst; ++d) first_[d + 1] = static_cast<std::int16_t>(first_[d] + cnt[d + 1]);
    int fill[kDst];
    for (int d = 0; d < kDst; ++d) fill[d] = first_[d];
    for (int k = 0; k < plan_.count; ++k) idx_[fill[plan_.c[k].d]++] = static_cast<std::int16_t>(k);
    for (int k = 0; k < kMaxLinks; ++k) dc_[k] = 0.f;
}

void Router::reset() noexcept {
    for (int j = 0; j < kSrc; ++j) {
        for (int s = 0; s < kBlock; ++s) out_[j][s] = prev_[j][s] = 0.f;
    }
    for (int j = 0; j < kDst; ++j) {
        for (int s = 0; s < kBlock; ++s) in_[j][s] = sum_[j][s] = 0.f;
    }
    for (int k = 0; k < kMaxLinks; ++k) dc_[k] = 0.f;
}

void Router::beginBlock() noexcept {
    for (int j = 0; j < kDst; ++j) inDone_[j] = false;
}

const float* Router::in(int d) noexcept {
    if (first_[d + 1] == first_[d]) return nullptr;
    float* dst = in_[d];
    if (inDone_[d]) return dst;
    for (int s = 0; s < kBlock; ++s) dst[s] = 0.f;
    const float dcA = 1.f - std::exp(-2.f * 3.14159265f * 10.f / 48000.f);
    for (int q = first_[d]; q < first_[d + 1]; ++q) {
        const int k = idx_[q];
        const PlanLink& c = plan_.c[k];
        const float g = c.amt;
        if (!c.fb) {
            const float* src = out_[c.s];
            for (int s = 0; s < kBlock; ++s) dst[s] += g * src[s];
        } else {
            // z^-1 (one block), soft clip to +-5 V, DC block (10 Hz)
            const float* src = prev_[c.s];
            float& st = dc_[k];
            for (int s = 0; s < kBlock; ++s) {
                const float x = 5.f * std::tanh(0.2f * g * src[s]);
                st += dcA * (x - st);
                dst[s] += x - st;
            }
            if (std::fabs(st) < 1e-20f) st = 0.f;
        }
    }
    inDone_[d] = true;
    return dst;
}

const float* Router::in2(int jack, int col) noexcept {
    const float* a = in(jack);
    const float* b = in(col);
    if (a == nullptr) return b;
    if (b == nullptr) return a;
    float* s = sum_[jack];
    for (int i = 0; i < kBlock; ++i) s[i] = a[i] + b[i];
    return s;
}

void Router::endBlock() noexcept {
    for (int j = 0; j < kSrc; ++j) {
        if (srcUse_[j] == 0) continue;
        for (int s = 0; s < kBlock; ++s) {
            const float x = out_[j][s];
            prev_[j][s] = std::isfinite(x) ? x : 0.f;
        }
    }
}

}  // namespace lift::eng
