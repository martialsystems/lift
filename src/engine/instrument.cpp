// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/instrument.h"

#include <cmath>

namespace lift::eng {

namespace {
inline float clampf(float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
// Schmitt trigger at 1.0 / 2.0 V (JCS gate detection thresholds)
inline bool schmitt(bool prev, float v) noexcept { return prev ? v > 1.f : v > 2.f; }
// QUANT scales (semitones in an octave, the octave closes each list)
constexpr int kScales[5][13] = {{0, 2, 4, 5, 7, 9, 11, 12},
                                {0, 2, 3, 5, 7, 8, 10, 12},
                                {0, 2, 4, 7, 9, 12},
                                {0, 3, 5, 7, 10, 12},
                                {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}};
constexpr int kScaleLen[5] = {8, 8, 6, 6, 13};
inline float blockMean(const float* x) noexcept {
    if (x == nullptr) return 0.f;
    float m = 0.f;
    for (int s = 0; s < kBlock; ++s) m += x[s];
    return m / static_cast<float>(kBlock);
}
inline float blockMax(const float* x) noexcept {
    float m = -10.f;
    for (int s = 0; s < kBlock; ++s) m = std::fmax(m, x[s]);
    return m;
}
}  // namespace

float quantize(float volts, int scale, int root) noexcept {
    const int sc = scale >= 0 && scale < 5 ? scale : 0;
    const int* sd = kScales[sc];
    const int len = kScaleLen[sc];
    const float n = volts * 12.f - static_cast<float>(root);
    const float oct = std::floor(n / 12.f);
    const float r = n - 12.f * oct;
    int best = 0;
    for (int k = 1; k < len; ++k) {
        if (std::fabs(r - static_cast<float>(sd[k])) < std::fabs(r - static_cast<float>(sd[best]))) best = k;
    }
    return (oct * 12.f + static_cast<float>(sd[best] + root)) / 12.f;
}

void Instrument::setPatch(const PatchPlan& p) noexcept {
    router.setPlan(p);
    // GATE takes over the synth's triggers, except the keyboard's own cable
    // alone (A GATE -> GATE is the normal made visible).
    bool other = false, any = false, pitchPin = false, vcaPin = false, clkOther = false;
    for (int k = 0; k < p.count; ++k) {
        const PlanLink& l = p.c[k];
        if (l.d == I_GATE || l.d == C_GATE) {
            any = true;
            other = other || !(l.cable && l.s == O_AGATE);
        }
        if (l.d == C_PITCH && !l.cable && p.pitchRow[l.s - kJacks]) pitchPin = true;
        if (l.d == C_VCA) vcaPin = true;
        if (l.d == I_CLK && !(l.cable && l.s == O_CLOCK)) clkOther = true;
    }
    // CLOCK -> CLK IN is the internal clock made visible, not an external one
    clkExternal_ = clkOther;
    const bool taken = any && other;
    if (taken && !gateTaken_) synth.allOff();
    gateTaken_ = taken;
    pitchTaken_ = router.cabled(I_PITCH) || pitchPin;
    vcaPinned_ = vcaPin;
}

Instrument::Instrument() { setPatch(planPatch(nullptr, nullptr, 0)); }

void Instrument::prepare(double fs) {
    fs_ = fs;
    synth.prepare(fs);
    drums.prepare(fs);
    fx.prepare(fs);
    clips.prepare(fs);
    router.reset();
    reset();
}

void Instrument::reset() noexcept {
    clips.reset();
    synth.reset();
    drums.reset();
    fx.reset();
    router.reset();
    pos_ = 0.0;
    nextStep_ = 0;
    wasRunning_ = false;
    lastStep_ = -1;
    nHeld_ = 0;
    pendingHits_ = 0;
}

void Instrument::noteOn(int note, float vel) noexcept {
    if (!gateTaken_) synth.noteOn(note, vel);
    lastNote_ = note;
    lastVel_ = vel;
    accentPending_ = accentPending_ || vel > 0.8f;
    for (int k = 0; k < nHeld_; ++k) {
        if (held_[k] == note) {
            return;
        }
    }
    if (nHeld_ < 16) {
        // keep the held list sorted (the arpeggiator plays it upwards)
        int k = nHeld_++;
        while (k > 0 && held_[k - 1] > note) {
            held_[k] = held_[k - 1];
            --k;
        }
        held_[k] = note;
    }
}

void Instrument::noteOff(int note) noexcept {
    if (!gateTaken_ || note < 0) synth.noteOff(note);
    if (note < 0) {
        nHeld_ = 0;
        return;
    }
    int w = 0;
    for (int k = 0; k < nHeld_; ++k) {
        if (held_[k] != note) {
            held_[w++] = held_[k];
        }
    }
    nHeld_ = w;
}

bool Instrument::isHeld(int note) const noexcept {
    for (int k = 0; k < nHeld_; ++k) {
        if (held_[k] == note) return true;
    }
    return false;
}

void Instrument::allOff() noexcept {
    synth.allOff();
    nHeld_ = 0;
}

void Instrument::drumHit(int voice, float vel) noexcept {
    if (voice < 0 || voice >= kDrumVoices) {
        return;
    }
    pendingHits_ |= 1u << voice;
    pendingVel_[voice] = vel;
    lastVel_ = vel;
    accentPending_ = accentPending_ || vel > 0.8f;
}

void Instrument::block(TapeHost& tape, const InstrumentCtl& ctl, float* outL, float* outR) noexcept {
    outL_ = outL;
    outR_ = outR;
    router.beginBlock();
    const PatchPlan& p = router.plan();
    for (int k = 0; k < kNodes; ++k) {
        runNode(p.order[k], tape, ctl);
    }
    router.endBlock();
    sampleCount_ += kBlock;
}

void Instrument::runNode(int node, TapeHost& tape, const InstrumentCtl& ctl) noexcept {
    switch (node) {
    case N_CLOCK: {
        float* clk = router.out(O_CLOCK);
        float* rst = router.out(O_RESET);
        float* pu = router.out(R_PULSE);  // PULSE FX not built yet: high on every step (half a step)
        const float* cin = clkExternal_ ? router.in(I_CLK) : nullptr;
        const float* rin = router.in(I_RST);
        const double perBeat = ctl.stepDiv == 12 ? 3.0 : ctl.stepDiv / 4.0;
        const double inc = (ctl.bpm > 1.0 ? ctl.bpm : 1.0) / 60.0 * perBeat / fs_;  // steps per sample
        const double sw = ctl.swing / 100.0 * 0.5;
        for (int s = 0; s < kBlock; ++s) {
            tickAt_[s] = -1;
        }
        if (!ctl.running) {
            wasRunning_ = false;
            pos_ = 0.0;
            nextStep_ = 0;
            extStep_ = 0;
            lastStep_ = -1;
            for (int s = 0; s < kBlock; ++s) {
                clk[s] = rst[s] = pu[s] = 0.f;
            }
            clkHigh_ = rstHigh_ = 0.f;
            break;
        }
        if (!wasRunning_) {
            wasRunning_ = true;
            pos_ = 0.0;
            nextStep_ = 0;
            extStep_ = 0;
            lfoPh_ = 0.0;
        }
        const int len = ctl.length > 0 ? ctl.length : 16;
        for (int s = 0; s < kBlock; ++s) {
            if (rin != nullptr) {
                const bool h = schmitt(rstInPrev_, rin[s]);
                if (h && !rstInPrev_) {
                    pos_ = 0.0;
                    nextStep_ = 0;
                    extStep_ = 0;
                }
                rstInPrev_ = h;
            }
            int tick = -1;
            if (cin != nullptr) {
                const bool h = schmitt(clkInPrev_, cin[s]);
                if (h && !clkInPrev_) {
                    tick = extStep_++;
                }
                clkInPrev_ = h;
            } else {
                const double due = nextStep_ + ((nextStep_ & 1) ? sw : 0.0);
                if (pos_ >= due) {
                    tick = nextStep_++;
                }
                pos_ += inc;
            }
            if (tick >= 0) {
                tickAt_[s] = tick;
                lastStep_ = tick % len;
                lastTick_ = sampleCount_ + s;
                ++tickCount_;
                clkHigh_ = cin != nullptr ? static_cast<float>(0.005 * fs_) : static_cast<float>(0.5 / inc);
                if (lastStep_ == 0) {
                    rstHigh_ = static_cast<float>(0.002 * fs_);
                }
            }
            clk[s] = clkHigh_ > 0.f ? 5.f : 0.f;
            pu[s] = clk[s];
            rst[s] = rstHigh_ > 0.f ? 5.f : 0.f;
            clkHigh_ -= 1.f;
            rstHigh_ -= 1.f;
        }
        break;
    }
    case N_KEYS: {
        float* ap = router.out(O_APITCH);
        float* ag = router.out(O_AGATE);
        float* bp = router.out(O_BPITCH);
        float* bg = router.out(O_BGATE);
        float* st = router.out(R_STEP);
        float* vl = router.out(R_VEL);
        float* ac = router.out(R_ACCENT);
        // ACCENT: a hard key or pad hit (velocity above 0.8), 10 ms
        if (accentPending_) {
            accentLeft_ = static_cast<float>(0.01 * fs_);
            accentPending_ = false;
        }
        const float a = static_cast<float>(lastNote_ - 60) / 12.f;
        const float g = nHeld_ > 0 ? 5.f : 0.f;
        const double perBeat = ctl.arpDiv == 12 ? 3.0 : ctl.arpDiv / 4.0;
        const float halfStep = static_cast<float>(0.5 * fs_ * 60.0 / ((ctl.bpm > 1.0 ? ctl.bpm : 1.0) * perBeat));
        // the arpeggiator steps on the clock's ticks scaled to its own division
        const double ratio = (ctl.arpDiv == 12 ? 3.0 : ctl.arpDiv / 4.0) / (ctl.stepDiv == 12 ? 3.0 : ctl.stepDiv / 4.0);
        for (int s = 0; s < kBlock; ++s) {
            ap[s] = a;
            ag[s] = g;
            if (tickAt_[s] >= 0 && nHeld_ > 0) {
                // ratio 1: every tick; 0.5: every other tick; 2: tick + midpoint (approximated per tick)
                const bool fire = ratio >= 1.0 || (tickAt_[s] % static_cast<int>(std::lround(1.0 / ratio))) == 0;
                if (fire) {
                    arpNote_ = held_[arpIdx_ % nHeld_];
                    ++arpIdx_;
                    bPitch_ = static_cast<float>(arpNote_ - 60) / 12.f;
                    bGateLeft_ = halfStep;
                }
            }
            if (nHeld_ == 0) {
                arpIdx_ = 0;
            }
            bp[s] = bPitch_;
            bg[s] = bGateLeft_ > 0.f ? 5.f : 0.f;
            bGateLeft_ -= 1.f;
            st[s] = bPitch_;
            vl[s] = 5.f * lastVel_;
            ac[s] = accentLeft_ > 0.f ? 5.f : 0.f;
            accentLeft_ -= 1.f;
        }
        aGate_ = g;
        break;
    }
    case N_DRUMS: {
        if (drums.kit() != ctl.kit) {
            drums.setKit(ctl.kit);
        }
        const float* slice = router.in(I_SLICE);
        float* dg = router.out(O_DRUM);
        const int len = ctl.length > 0 ? ctl.length : 16;
        const int kit = ctl.kit >= 0 && ctl.kit < kKits ? ctl.kit : 0;
        for (int v = 0; v < kDrumVoices; ++v) {
            vel_[v] = 0.9f;
        }
        for (int s = 0; s < kBlock; ++s) {
            std::uint32_t m = 0;
            if (s == 0 && pendingHits_ != 0) {
                m |= pendingHits_;
                for (int v = 0; v < kDrumVoices; ++v) {
                    if ((pendingHits_ >> v) & 1u) vel_[v] = pendingVel_[v];
                }
                pendingHits_ = 0;
            }
            if (tickAt_[s] >= 0 && patterns_ != nullptr) {
                const int st = tickAt_[s] % len;
                for (int v = 0; v < kDrumVoices; ++v) {
                    const std::uint32_t bits = patterns_[kit * kDrumVoices + v].load(std::memory_order_relaxed);
                    if ((bits >> st) & 1u) {
                        m |= 1u << v;
                    }
                }
            }
            hitMask_[s] = m;
            if (m != 0) {
                drumGate_ = static_cast<float>(clampf(ctl.drumGateMs, 1.f, 250.f) * 0.001f * static_cast<float>(fs_));
                lastHits_ |= m;
            }
            dg[s] = drumGate_ > 0.f ? 5.f : 0.f;
            drumGate_ -= 1.f;
        }
        // SLICE (pitch): transposes the selected drum sound at 1 V/oct
        const int sv = ctl.selVoice >= 0 && ctl.selVoice < kDrumVoices ? ctl.selVoice : 0;
        if (sv != sliceVoice_) {
            drums.setTranspose(sliceVoice_, 0.f);
            sliceVoice_ = sv;
        }
        drums.setTranspose(sv, slice != nullptr ? blockMean(slice) : 0.f);
        drums.render(drumL_, drumR_, kBlock, hitMask_, vel_);
        break;
    }
    case N_RADIO: {
        float* r = router.out(O_RADIO);  // PLACEHOLDER: the radio is not on the patch bay yet
        for (int s = 0; s < kBlock; ++s) r[s] = 0.f;
        break;
    }
    case N_MOD: {
        // LFO: RONIN MG style triangle, tempo locked, one cycle every two beats,
        // +-2.5 V. LFO 2: free sine, 8 s, +-2.5 V. RANDOM: a new value each step,
        // glided (+-5 V). NOISE: white, +-5 V.
        float* o = router.out(R_LFO);
        float* o2 = router.out(R_LFO2);
        float* rn = router.out(R_RANDOM);
        float* nz = router.out(R_NOISE);
        const double inc = (ctl.bpm > 1.0 ? ctl.bpm : 1.0) / 60.0 / 2.0 / fs_;
        const double inc2 = 1.0 / (8.0 * fs_);
        const float glide = 1.f / static_cast<float>(0.5 * fs_ * 60.0 / (ctl.bpm > 1.0 ? ctl.bpm : 1.0));
        for (int s = 0; s < kBlock; ++s) {
            const double t = lfoPh_;
            o[s] = 2.5f * static_cast<float>(t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t);
            lfoPh_ += inc;
            if (lfoPh_ >= 1.0) lfoPh_ -= 1.0;
            o2[s] = 2.5f * static_cast<float>(std::sin(6.283185307179586 * lfo2Ph_));
            lfo2Ph_ += inc2;
            if (lfo2Ph_ >= 1.0) lfo2Ph_ -= 1.0;
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            nz[s] = 5.f * (static_cast<float>(rng_ >> 8) / 8388608.f - 1.f);
            if (tickAt_[s] >= 0) {
                rndFrom_ = rnd_;
                rndTo_ = nz[s];
                rndT_ = 0.f;
            }
            rndT_ = std::fmin(1.f, rndT_ + glide);
            const float e = rndT_ * rndT_ * (3.f - 2.f * rndT_);
            rnd_ = rndFrom_ + (rndTo_ - rndFrom_) * e;
            rn[s] = rnd_;
        }
        lfoOut_ = o[kBlock - 1];
        break;
    }
    case N_ENV: {
        // ENV: RONIN EG style AR on the keyboard gate, 5 ms / 300 ms, 0..5 V.
        // ENV 2: a 150 ms decay on every note (keys and SEQ steps).
        float* o = router.out(R_ENV);
        float* o2 = router.out(R_ENV2);
        const float aA = 1.f - std::exp(-1.f / (0.005f * static_cast<float>(fs_)));
        const float aR = 1.f - std::exp(-1.f / (0.3f * static_cast<float>(fs_)));
        const float aD = std::exp(-1.f / (0.15f * static_cast<float>(fs_)));
        const float target = aGate_ > 0.f ? 5.f : 0.f;
        const float* ag = router.out(O_AGATE);
        const float* bg = router.out(O_BGATE);
        for (int s = 0; s < kBlock; ++s) {
            env_ += (target > env_ ? aA : aR) * (target - env_);
            o[s] = env_;
            const bool g = ag[s] > 2.f || bg[s] > 2.f;
            if (g && !env2Gate_) env2_ = 5.f;
            env2Gate_ = g;
            env2_ *= aD;
            o2[s] = env2_;
        }
        if (env_ < 1e-6f) env_ = 0.f;
        if (env2_ < 1e-6f) env2_ = 0.f;
        break;
    }
    case N_INPUT: {
        // The IN path (AUDIO L / R or the interface) and FOLLOW, its level
        // (10 ms up, 150 ms down; -6 dBFS peaks = 5 V).
        tape.inputBlock(router.in(I_AUDIOL), router.in(I_AUDIOR), inL_, inR_, kBlock);
        float* f = router.out(R_FOLLOW);
        const float up = 1.f - std::exp(-1.f / (0.01f * static_cast<float>(fs_)));
        const float dn = 1.f - std::exp(-1.f / (0.15f * static_cast<float>(fs_)));
        for (int s = 0; s < kBlock; ++s) {
            const float a = std::fmax(std::fabs(inL_[s]), std::fabs(inR_[s]));
            follow_ += (a > follow_ ? up : dn) * (a - follow_);
            f[s] = clampf(10.f * follow_, 0.f, 5.f);
        }
        if (follow_ < 1e-9f) follow_ = 0.f;
        break;
    }
    case N_SH: {
        // sample and hold on every clock step; unpatched it samples NOISE (row O)
        float* o = router.out(R_SH);
        const float* in = router.in(I_SH);
        const float* nz = router.out(R_NOISE);
        for (int s = 0; s < kBlock; ++s) {
            if (tickAt_[s] >= 0) sh_ = in != nullptr ? in[s] : nz[s];
            o[s] = sh_;
        }
        break;
    }
    case N_VCA: {
        // VCA IN x gain; the gain is ENV / 5 V unless a pin sits in the VCA column
        float* o = router.out(R_VCA);
        const float* in = router.in(I_VCAIN);
        const float* cv = vcaPinned_ ? router.in(C_VCA) : nullptr;
        const float* env = router.out(R_ENV);
        for (int s = 0; s < kBlock; ++s) {
            const float g = clampf((cv != nullptr ? cv[s] : env[s]) * 0.2f, 0.f, 1.f);
            o[s] = in != nullptr ? in[s] * g : 0.f;
        }
        break;
    }
    case N_SLEW: {
        // lag processor (RONIN integrator style), 120 ms
        float* o = router.out(R_SLEW);
        const float* in = router.in(I_SLEW);
        const float a = 1.f - std::exp(-1.f / (0.12f * static_cast<float>(fs_)));
        for (int s = 0; s < kBlock; ++s) {
            slew_ += a * ((in != nullptr ? in[s] : 0.f) - slew_);
            o[s] = slew_;
        }
        break;
    }
    case N_QUANT: {
        // 1 V/oct to the nearest note of the scale (root = TRANSPOSE); unpatched it quantizes S&H
        float* o = router.out(R_QUANT);
        const float* in = router.in(I_QUANT);
        if (in == nullptr) in = router.out(R_SH);
        for (int s = 0; s < kBlock; ++s) o[s] = quantize(in[s], ctl.quantScale, ctl.transpose);
        break;
    }
    case N_TRANSPORT: {
        tape.transportBlock(router.out(R_EOC), kBlock);
        break;
    }
    case N_H1:
    case N_H2:
    case N_H3:
    case N_H4: {
        const int t = node - N_H1;
        const bool armed = t == ctl.arm;
        const float* sp = armed ? router.in2(I_SPEED, C_SPEED) : nullptr;
        const float* sc = armed ? router.in2(I_SCRUB, C_SCRUB) : nullptr;
        bool rev = false;
        if (armed) {
            const float* rv = router.in(I_REVERSE);
            rev = rv != nullptr && blockMax(rv) > 1.5f;
        }
        tape.headBlock(t, sp, rev, sc, head_, kBlock);
        float* h = router.out(O_HEAD1 - t);
        for (int s = 0; s < kBlock; ++s) h[s] = 5.f * head_[s];
        break;
    }
    case N_SYNTH: {
        // PITCH (jack + column): a cable or a pitch-row pin replaces the A PITCH
        // normal; other rows add 1 V = 1 semitone on top of the keyboard.
        // Voices move by (PITCH - A PITCH), so the keyboard's own cable is a
        // straight wire. GATE (jack + column) takes over: rising edges play,
        // falling edges release, and keys / SEQ no longer trigger the synth
        // (the A GATE cable alone is the normal).
        SynthCv cv;
        const float* pin = router.in2(I_PITCH, C_PITCH);
        if (pin != nullptr) {
            const float a = static_cast<float>(lastNote_ - 60) / 12.f;
            for (int s = 0; s < kBlock; ++s) pitchRel_[s] = pitchTaken_ ? pin[s] - a : pin[s];
            cv.pitch = pitchRel_;
        }
        cv.fm = router.in(C_FM);
        cv.cutoff = router.in(C_CUTOFF);
        cv.reso = 0.1f * blockMean(router.in(C_RESO));
        cv.wave = 0.1f * blockMean(router.in(C_WAVE));
        cv.decay = 0.1f * blockMean(router.in(C_DECAY));
        cv.level = 0.1f * blockMean(router.in(C_LEVEL));
        gPos_ = blockMean(router.in(C_GPOS));
        gSize_ = blockMean(router.in(C_GSIZE));
        const float* gate = router.in2(I_GATE, C_GATE);
        if (gate != nullptr && gateTaken_) {
            const bool h = schmitt(cvGate_, blockMax(gate));
            if (h && !cvGate_) {
                cvNote_ = lastNote_;
                synth.noteOn(cvNote_, 0.85f);
            }
            if (!h && cvGate_) synth.noteOff(cvNote_);
            cvGate_ = h;
        } else if (cvGate_) {
            synth.noteOff(cvNote_);
            cvGate_ = false;
        }
        synth.render(synthBuf_, kBlock, cv);
        break;
    }
    case N_FX: {
        for (int s = 0; s < kBlock; ++s) clipL_[s] = clipR_[s] = 0.f;
        clips.render(clipL_, clipR_, kBlock);
        for (int s = 0; s < kBlock; ++s) {
            srcL_[s] = synthBuf_[s] + drumL_[s] + clipL_[s];
            srcR_[s] = synthBuf_[s] + drumR_[s] + clipR_[s];
        }
        fx.setBypass(!ctl.fxOn);
        fx.process(srcL_, srcR_, kBlock, router.in(C_FXMAC));
        float* o = router.out(O_FXOUT);
        for (int s = 0; s < kBlock; ++s) o[s] = 2.5f * (srcL_[s] + srcR_[s]);
        break;
    }
    case N_MIX: {
        TapeMixIo io;
        io.srcL = srcL_;
        io.srcR = srcR_;
        io.inL = inL_;
        io.inR = inR_;
        io.biasCv = router.in2(I_BIAS, C_BIAS);
        const float* rec = router.in2(I_REC, C_REC);
        if (rec != nullptr) {
            const bool h = schmitt(recGate_, blockMax(rec));
            io.recPress = h && !recGate_;
            recGate_ = h;
        } else {
            recGate_ = false;
        }
        io.outL = outL_;
        io.outR = outR_;
        io.send = send_;
        tape.mixBlock(io, kBlock);
        float* ml = router.out(O_MIXL);
        float* mr = router.out(O_MIXR);
        float* se = router.out(O_SEND);
        for (int s = 0; s < kBlock; ++s) {
            ml[s] = 5.f * outL_[s];
            mr[s] = 5.f * outR_[s];
            se[s] = 5.f * send_[s];
        }
        break;
    }
    default: break;
    }
}

}  // namespace lift::eng
