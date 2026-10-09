// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/process_block.h"

#include <cmath>

namespace {

constexpr int kChunk = kTapeMaxBlock;
constexpr int kRecMask = kRecIdxRing - 1;

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

// One chunk. The record chain runs as a block first: it needs the input and,
// for sound on sound, what is already on the armed track at each capture
// position. Its output is written back where it was captured, latency later.
void process_chunk(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept {
    float tick[kChunk];
    float srcL[kChunk];
    float srcR[kChunk];
    float recL[kChunk];
    float recR[kChunk];
    int capture[kChunk];
    for (int i = 0; i < n; ++i) {
        tick[i] = metronome_tick(rt);
    }
    float* destL = rt.ch[rt.arm][0];
    float* destR = rt.ch[rt.arm][1];
    const bool canWrite = destL != nullptr && destR != nullptr;
    if (rt.recording) {
        if (!rt.recWas) {
            rt.recCount = 0;
        }
        double p = rt.pos;
        for (int i = 0; i < n; ++i) {
            const float t = rt.resampleInput ? tick[i] : 0.f;
            srcL[i] = (inL != nullptr ? inL[i] : 0.f) + t;
            srcR[i] = (inR != nullptr ? inR[i] : 0.f) + t;
            const int idx = static_cast<int>(p);
            capture[i] = idx;
            if (rt.overdub && canWrite && idx >= 0 && idx < rt.frames) {
                srcL[i] += destL[idx];
                srcR[i] += destR[idx];
            }
            p = transport_next_pos(rt, p, static_cast<double>(kRecordRate));
        }
        tape_engine_record(rt.engine, srcL, srcR, recL, recR, n);
    }
    rt.recWas = rt.recording;
    const int lat = tape_engine_latency(rt.engine);
    for (int i = 0; i < n; ++i) {
        if (rt.recording) {
            rt.recIdx[rt.recCount & kRecMask] = capture[i];
            const int from = rt.recCount - lat;
            rt.recCount += 1;
            if (from >= 0) {
                const int idx = rt.recIdx[from & kRecMask];
                if (canWrite && idx >= 0 && idx < rt.frames) {
                    destL[idx] = recL[i];
                    destR[idx] = recR[i];
                } else {
                    rt.clipped = true;
                }
            }
        }
        const float speed = tape_engine_next_speed(rt.engine, rt.varispeed);
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
            sumL += read_looped(bufL, rt.frames, rt.pos, rt.loopStart, rt.loopEnd, rt.reverse) * rt.fader[t];
            sumR += read_looped(bufR, rt.frames, rt.pos, rt.loopStart, rt.loopEnd, rt.reverse) * rt.fader[t];
        }
        // Every track shares the heads and the tape path, so the playback
        // chain runs once on the mix.
        tape_engine_play(rt.engine, sumL, sumR);
        if (!rt.resampleInput) {
            sumL += tick[i];
            sumR += tick[i];
        }
        outL[i] = sumL;
        outR[i] = sumR;
        double rate = static_cast<double>(kRecordRate);
        if (!rt.recording) {
            rate = rt.reverse ? -static_cast<double>(speed) : static_cast<double>(speed);
        }
        transport_advance(rt, rate);
    }
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
        rt.recWas = false;
        return;
    }
    TapeNoDenormals guard;
    for (int start = 0; start < n; start += kChunk) {
        const int m = n - start < kChunk ? n - start : kChunk;
        process_chunk(rt, inL != nullptr ? inL + start : nullptr, inR != nullptr ? inR + start : nullptr,
                      outL + start, outR + start, m);
        if (tape_engine_halted(rt.engine)) {
            transport_stop(rt);
            for (int i = start + m; i < n; ++i) {
                outL[i] = 0.f;
                outR[i] = 0.f;
            }
            break;
        }
    }
}
