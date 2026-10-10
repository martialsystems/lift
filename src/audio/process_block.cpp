// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/process_block.h"


#include <cmath>

namespace {

constexpr int kChunk = kTapeMaxBlock;
constexpr int kRecMask = kRecIdxRing - 1;
// one-pole coefficients at the tape rate (48 kHz): 1 - exp(-2 pi f / fs)
constexpr float kEqLoA = 0.0322f;  // 250 Hz
constexpr float kEqHiA = 0.4076f;  // 4 kHz

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

inline double wrapSpan(double x, double span) noexcept {
    if (span <= 0.0) return x;
    while (x >= span) x -= span;
    while (x < 0.0) x += span;
    return x;
}

}  // namespace

int tape_transport(TapeRuntime& rt, int n, float* eoc) noexcept {
    n = n > kChunk ? kChunk : n;
    if (!rt.playing && !rt.recording) {
        rt.blkN = 0;
        if (eoc != nullptr) {
            for (int i = 0; i < n; ++i) eoc[i] = 0.f;
        }
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        rt.blkPos[i] = rt.pos;
        const float speed = tape_engine_next_speed(rt.engine, rt.varispeed);
        rt.blkRamp[i] = rt.engine.ramp;
        rt.blkSpeed[i] = rt.engine.speedNow;
        double rate = static_cast<double>(kRecordRate);
        if (!rt.recording) {
            rate = rt.reverse ? -static_cast<double>(speed) : static_cast<double>(speed);
        }
        rt.blkRate[i] = rate;
        const double before = rt.pos;
        transport_advance(rt, rate);
        if (fabs(rt.pos - before - rate) > 1.0) {
            rt.eocLeft = 96.f;  // the loop wrapped: 2 ms at 48 kHz
        }
        if (eoc != nullptr) {
            eoc[i] = rt.eocLeft > 0.f ? 5.f : 0.f;
        }
        rt.eocLeft -= 1.f;
    }
    rt.blkN = n;
    return n;
}

void tape_head(TapeRuntime& rt, int t, const float* speedCv, bool rev, const float* scrubCv, float* mono) noexcept {
    const int n = rt.blkN;
    float* hl = rt.headL[t];
    float* hr = rt.headR[t];
    const float* bufL = rt.ch[t][0];
    const float* bufR = rt.ch[t][1];
    const bool loop = rt.loopEnd > rt.loopStart;
    const double span = loop ? static_cast<double>(rt.loopEnd - rt.loopStart) : static_cast<double>(rt.frames);
    const bool moved = speedCv != nullptr || rev;
    for (int i = 0; i < n; ++i) {
        float l = 0.f, r = 0.f;
        if (bufL != nullptr && bufR != nullptr) {
            double p = rt.blkPos[i] + rt.trackOff[t];
            if (scrubCv != nullptr) {
                const float v = scrubCv[i] < -5.f ? -5.f : (scrubCv[i] > 5.f ? 5.f : scrubCv[i]);
                p += rt.beatFrames * static_cast<double>(v) / 5.0;
            }
            if (!loop && rt.frames > 0) {
                p = p < 0.0 ? 0.0 : (p > rt.frames - 1 ? rt.frames - 1 : p);
            }
            const bool back = rt.reverse != rev;
            l = read_looped(bufL, rt.frames, p, rt.loopStart, rt.loopEnd, back);
            r = read_looped(bufR, rt.frames, p, rt.loopStart, rt.loopEnd, back);
        }
        if (moved) {
            // this loop runs at its own rate: the offset takes the difference
            double m = 1.0;
            if (speedCv != nullptr) {
                const float v = speedCv[i] < -36.f ? -36.f : (speedCv[i] > 36.f ? 36.f : speedCv[i]);
                m = exp2(static_cast<double>(v) / 12.0);
            }
            const double base = rt.blkRate[i];
            const double eff = (rev ? -1.0 : 1.0) * base * m;
            rt.trackOff[t] = wrapSpan(rt.trackOff[t] + eff - base, span);
        }
        hl[i] = l;
        hr[i] = r;
        if (mono != nullptr) {
            mono[i] = 0.5f * (l + r);
        }
    }
}

void tape_mix(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, float* sendBus, int n) noexcept {
    const int m = rt.blkN < n ? rt.blkN : n;
    for (int i = m; i < n; ++i) {
        outL[i] = outR[i] = 0.f;
        if (sendBus != nullptr) sendBus[i] = 0.f;
    }
    if (m <= 0) {
        rt.recWas = false;
        return;
    }
    TapeNoDenormals guard;
    const float rampEnd = rt.engine.ramp, speedEnd = rt.engine.speedNow;
    float tick[kChunk];
    float srcL[kChunk];
    float srcR[kChunk];
    float recL[kChunk];
    float recR[kChunk];
    int capture[kChunk];
    for (int i = 0; i < m; ++i) {
        tick[i] = metronome_tick(rt);
    }
    float* destL = rt.ch[rt.arm][0];
    float* destR = rt.ch[rt.arm][1];
    const bool canWrite = destL != nullptr && destR != nullptr;
    // The record chain runs as a block first: it needs the input and, for
    // sound on sound, what is already on the armed track at each capture
    // position. Its output is written back where it was captured, latency later.
    if (rt.recording) {
        if (!rt.recWas) {
            rt.recCount = 0;
            // a new overdub pass
            rt.passCount += 1;
            rt.passTrack = rt.arm;
            rt.passId = static_cast<uint8_t>((rt.passCount - 1) % 255 + 1);  // 1..255, never 0
            if (rt.passId == 1 && rt.passCount > 1 && rt.passMark != nullptr) {
                for (int i = 0; i < rt.frames; ++i) rt.passMark[i] = 0;  // ids wrapped: old marks go
            }
        }
        for (int i = 0; i < m; ++i) {
            const float t = rt.resampleInput ? tick[i] : 0.f;
            srcL[i] = (inL != nullptr ? inL[i] : 0.f) + t;
            srcR[i] = (inR != nullptr ? inR[i] : 0.f) + t;
            const int idx = static_cast<int>(rt.blkPos[i]);
            capture[i] = idx;
            if (rt.overdub && canWrite && idx >= 0 && idx < rt.frames) {
                srcL[i] += destL[idx];
                srcR[i] += destR[idx];
            }
        }
        if (rt.cassette) {
            tape_engine_record(rt.engine, srcL, srcR, recL, recR, m);
        } else {
            // no cassette stage: the source straight onto the loop at REC LVL
            for (int i = 0; i < m; ++i) {
                const int idx = capture[i];
                const float t = rt.resampleInput ? tick[i] : 0.f;
                const float il = (inL != nullptr ? inL[i] : 0.f) + t;
                const float ir = (inR != nullptr ? inR[i] : 0.f) + t;
                const bool od = rt.overdub && canWrite && idx >= 0 && idx < rt.frames;
                recL[i] = (od ? destL[idx] : 0.f) + rt.recGain * il;
                recR[i] = (od ? destR[idx] : 0.f) + rt.recGain * ir;
            }
        }
    }
    const bool keepPass = rt.passMark != nullptr && rt.passBak[0] != nullptr && rt.passTrack == rt.arm;
    rt.recWas = rt.recording;
    const int lat = rt.cassette ? tape_engine_latency(rt.engine) : 0;
    for (int i = 0; i < m; ++i) {
        if (rt.recording) {
            rt.recIdx[rt.recCount & kRecMask] = capture[i];
            const int from = rt.recCount - lat;
            rt.recCount += 1;
            if (from >= 0) {
                const int idx = rt.recIdx[from & kRecMask];
                if (canWrite && idx >= 0 && idx < rt.frames) {
                    if (keepPass && rt.passMark[idx] != rt.passId) {
                        rt.passBak[0][idx] = destL[idx];
                        rt.passBak[1][idx] = destR[idx];
                        rt.passMark[idx] = rt.passId;
                    }
                    destL[idx] = recL[i];
                    destR[idx] = recR[i];
                } else {
                    rt.clipped = true;
                }
            }
        }
        float sumL = 0.f;
        float sumR = 0.f;
        float dry = 0.f;
        for (int t = 0; t < kTrackCount; ++t) {
            if (rt.mute[t] || rt.fader[t] == 0.f || rt.ch[t][0] == nullptr || rt.ch[t][1] == nullptr) {
                continue;
            }
            float xl = rt.headL[t][i] * rt.fader[t];
            float xr = rt.headR[t][i] * rt.fader[t];
            if (rt.lowG[t] != 0.f || rt.highG[t] != 0.f) {
                // shelves: x + lowG * lowpass(x) + highG * (x - lowpass4k(x))
                float* lo = rt.eqLo[t];
                float* hi = rt.eqHi[t];
                lo[0] += kEqLoA * (xl - lo[0]);
                lo[1] += kEqLoA * (xr - lo[1]);
                hi[0] += kEqHiA * (xl - hi[0]);
                hi[1] += kEqHiA * (xr - hi[1]);
                xl += rt.lowG[t] * lo[0] + rt.highG[t] * (xl - hi[0]);
                xr += rt.lowG[t] * lo[1] + rt.highG[t] * (xr - hi[1]);
            }
            sumL += xl * rt.panL[t];
            sumR += xr * rt.panR[t];
            dry += 0.5f * (xl + xr);
        }
        if (sendBus != nullptr) {
            sendBus[i] = dry;
        }
        // Every loop shares the tape path, so the playback chain runs once on
        // the mix, at the motor's state for this sample.
        if (rt.cassette) {
            rt.engine.ramp = rt.blkRamp[i];
            rt.engine.speedNow = rt.blkSpeed[i];
            tape_engine_play(rt.engine, sumL, sumR);
        }
        if (!rt.resampleInput) {
            sumL += tick[i];
            sumR += tick[i];
        }
        outL[i] = sumL;
        outR[i] = sumR;
    }
    rt.engine.ramp = rampEnd;
    rt.engine.speedNow = speedEnd;
    if (tape_engine_halted(rt.engine)) {
        transport_stop(rt);
    }
}

void process_block(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept {
    if (n <= 0 || outL == nullptr || outR == nullptr) {
        return;
    }
    for (int start = 0; start < n; start += kChunk) {
        const int m = n - start < kChunk ? n - start : kChunk;
        tape_transport(rt, m, nullptr);
        for (int t = 0; t < kTrackCount; ++t) {
            tape_head(rt, t, nullptr, false, nullptr, nullptr);
        }
        tape_mix(rt, inL != nullptr ? inL + start : nullptr, inR != nullptr ? inR + start : nullptr, outL + start,
                 outR + start, nullptr, m);
    }
}
