// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/patch.h"

#include <jidai/jcs/Graph.h>

#include <cmath>
#include <vector>

namespace lift::eng {

const int kOutNode[kJacks] = {N_KEYS, N_KEYS, N_KEYS,  N_KEYS, N_DRUMS, N_CLOCK, N_CLOCK, N_RADIO,
                              N_LFO,  N_ENV,  N_SH,    N_VCA,  N_SLEW,  N_QUANT, N_TAPE,  N_TAPE};
const int kInNode[kJacks] = {N_SYNTH, N_SYNTH, N_SYNTH, N_SYNTH, N_DRUMS, N_FX,  N_CLOCK, N_CLOCK,
                             N_VCA,   N_VCA,   N_SLEW,  N_QUANT, N_SH,    N_TAPE, N_TAPE, N_TAPE};

namespace {
// In-device order: what each node reads without a cable.
std::vector<jidai::jcs::GraphEdge> fixedEdges() {
    return {{N_CLOCK, N_KEYS}, {N_CLOCK, N_DRUMS}, {N_CLOCK, N_SH},  {N_CLOCK, N_LFO}, {N_KEYS, N_ENV},
            {N_KEYS, N_SYNTH}, {N_ENV, N_VCA},     {N_SYNTH, N_FX},  {N_DRUMS, N_FX},  {N_FX, N_TAPE}};
}
}  // namespace

PatchPlan planPatch(const int* outs, const int* ins, int n) {
    PatchPlan p;
    std::vector<jidai::jcs::GraphEdge> cables;
    std::vector<int> idx;
    for (int k = 0; k < n && p.count < kMaxCables; ++k) {
        if (outs[k] < 0 || outs[k] >= kJacks || ins[k] < 0 || ins[k] >= kJacks) {
            continue;
        }
        p.c[p.count].o = static_cast<std::int8_t>(outs[k]);
        p.c[p.count].i = static_cast<std::int8_t>(ins[k]);
        p.amount[p.count] = 1.f;
        cables.push_back({kOutNode[outs[k]], kInNode[ins[k]]});
        ++p.count;
    }
    const auto fixed = fixedEdges();
    const std::vector<char> fb = jidai::jcs::classifyFeedback(kNodes, fixed, cables);
    for (int k = 0; k < p.count; ++k) {
        p.c[k].fb = fb[static_cast<size_t>(k)] != 0;
    }
    const std::vector<int> order = jidai::jcs::runOrder(kNodes, fixed, cables, fb);
    for (int k = 0; k < kNodes; ++k) {
        p.order[k] = static_cast<std::int8_t>(k < static_cast<int>(order.size()) ? order[static_cast<size_t>(k)] : k);
    }
    return p;
}

bool cableIsFeedback(const PatchPlan& p, int o, int i) noexcept {
    for (int k = 0; k < p.count; ++k) {
        if (p.c[k].o == o && p.c[k].i == i) {
            return p.c[k].fb;
        }
    }
    return false;
}

void Router::setPlan(const PatchPlan& p) noexcept {
    plan_ = p;
    for (int j = 0; j < kJacks; ++j) {
        inCount_[j] = outCount_[j] = 0;
    }
    for (int k = 0; k < plan_.count; ++k) {
        ++inCount_[plan_.c[k].i];
        ++outCount_[plan_.c[k].o];
    }
}

void Router::reset() noexcept {
    for (int j = 0; j < kJacks; ++j) {
        for (int s = 0; s < kBlock; ++s) {
            out_[j][s] = prev_[j][s] = in_[j][s] = 0.f;
        }
        for (int k = 0; k < kJacks; ++k) {
            dc_[j][k] = 0.f;
        }
    }
}

void Router::beginBlock() noexcept {
    for (int j = 0; j < kJacks; ++j) {
        inDone_[j] = false;
    }
}

const float* Router::in(int jack) noexcept {
    if (inCount_[jack] == 0) {
        return nullptr;
    }
    float* dst = in_[jack];
    if (inDone_[jack]) {
        return dst;
    }
    for (int s = 0; s < kBlock; ++s) {
        dst[s] = 0.f;
    }
    const float dcA = 1.f - std::exp(-2.f * 3.14159265f * 10.f / 48000.f);
    for (int k = 0; k < plan_.count; ++k) {
        const PlanCable& c = plan_.c[k];
        if (c.i != jack) {
            continue;
        }
        const float g = plan_.amount[k];
        if (!c.fb) {
            const float* src = out_[c.o];
            for (int s = 0; s < kBlock; ++s) {
                dst[s] += g * src[s];
            }
        } else {
            // z^-1 (one block), soft clip to +-5 V, DC block (10 Hz)
            const float* src = prev_[c.o];
            float& st = dc_[c.o][c.i];
            for (int s = 0; s < kBlock; ++s) {
                const float x = 5.f * std::tanh(0.2f * g * src[s]);
                st += dcA * (x - st);
                dst[s] += x - st;
            }
            if (std::fabs(st) < 1e-20f) {
                st = 0.f;
            }
        }
    }
    inDone_[jack] = true;
    return dst;
}

void Router::endBlock() noexcept {
    for (int j = 0; j < kJacks; ++j) {
        if (outCount_[j] == 0) {
            continue;
        }
        for (int s = 0; s < kBlock; ++s) {
            const float x = out_[j][s];
            prev_[j][s] = std::isfinite(x) ? x : 0.f;
        }
    }
}

}  // namespace lift::eng
