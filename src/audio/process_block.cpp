// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/process_block.h"

#include <cmath>

namespace {

float play_rate(TapeRuntime& rt) noexcept {
    const CharacterCoeffs& c = rt.coeff;
    const float white = next_white(rt.rng);
    rt.flutter += c.flutterA * (white - rt.flutter);
    const float wow = sinf(rt.wowPhase);
    rt.wowPhase += c.wowInc;
    if (rt.wowPhase > 6.28318530718f) {
        rt.wowPhase -= 6.28318530718f;
    }
    float scale = 1.f + c.wowDepth * wow + c.flutterDepth * rt.flutter;
    if (scale < 0.25f) {
        scale = 0.25f;
    }
    float rate = rt.varispeed * scale;
    if (rt.reverse) {
        rate = -rate;
    }
    return rate;
}

float metronome_tick(TapeRuntime& rt) noexcept {
    if (!rt.metronome || rt.metroPeriod <= 0) {
        return 0.f;
    }
    rt.metroCounter -= 1;
    if (rt.metroCounter > 0) {
        return 0.f;
    }
    rt.metroCounter = rt.metroPeriod;
    return 0.6f;
}

}  // namespace

void process_block(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept {
    if (n <= 0 || outL == nullptr || outR == nullptr) {
        return;
    }
    if (!rt.playing && !rt.recording) {
        for (int i = 0; i < n; ++i) {
            outL[i] = 0.f;
            outR[i] = 0.f;
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float tick = metronome_tick(rt);
        const float srcL = (inL != nullptr ? inL[i] : 0.f) + (rt.resampleInput ? tick : 0.f);
        const float srcR = (inR != nullptr ? inR[i] : 0.f) + (rt.resampleInput ? tick : 0.f);
        if (rt.recording) {
            const int idx = static_cast<int>(rt.pos);
            float* destL = rt.ch[rt.arm][0];
            float* destR = rt.ch[rt.arm][1];
            if (destL != nullptr && destR != nullptr && idx >= 0 && idx < rt.frames) {
                destL[idx] = eco_record(rt.rec[0], srcL, rt.coeff);
                destR[idx] = eco_record(rt.rec[1], srcR, rt.coeff);
            } else {
                rt.clipped = true;
            }
        }
        float sumL = 0.f;
        float sumR = 0.f;
        for (int t = 0; t < kTrackCount; ++t) {
            if (rt.mute[t] || rt.fader[t] == 0.f) {
                continue;
            }
            const float* bufL = rt.ch[t][0];
            const float* bufR = rt.ch[t][1];
            if (bufL == nullptr || bufR == nullptr) {
                continue;
            }
            const float sL = read_looped(bufL, rt.frames, rt.pos, rt.loopStart, rt.loopEnd, rt.reverse);
            const float sR = read_looped(bufR, rt.frames, rt.pos, rt.loopStart, rt.loopEnd, rt.reverse);
            const float heardL = eco_play(rt.play[t][0], sL, rt.coeff, rt.rng);
            const float heardR = eco_play(rt.play[t][1], sR, rt.coeff, rt.rng);
            sumL += heardL * rt.fader[t];
            sumR += heardR * rt.fader[t];
        }
        if (!rt.resampleInput) {
            sumL += tick;
            sumR += tick;
        }
        outL[i] = sumL;
        outR[i] = sumR;
        const double rate = rt.recording ? static_cast<double>(kRecordRate) : static_cast<double>(play_rate(rt));
        transport_advance(rt, rate);
    }
}
