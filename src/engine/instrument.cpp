// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/instrument.h"

#include <cmath>

namespace lift::eng {

namespace {
inline float clampf(float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
// Schmitt trigger at 1.0 / 2.0 V (JCS gate detection thresholds)
inline bool schmitt(bool prev, float v) noexcept { return prev ? v > 1.f : v > 2.f; }
constexpr int kScale[8] = {0, 2, 4, 5, 7, 9, 11, 12};  // QUANT: major scale
}  // namespace

Instrument::Instrument() { router.setPlan(planPatch(nullptr, nullptr, 0)); }

void Instrument::prepare(double fs) {
    fs_ = fs;
    synth.prepare(fs);
    drums.prepare(fs);
    fx.prepare(fs);
    router.reset();
    reset();
}

void Instrument::reset() noexcept {
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
    synth.noteOn(note, vel);
    lastNote_ = note;
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
    synth.noteOff(note);
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
        const float* cin = router.in(I_CLK);
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
                clk[s] = rst[s] = 0.f;
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
            if (slice != nullptr) {
                const bool h = schmitt(sliceIn_, slice[s]);
                if (h && !sliceIn_) {
                    m |= 1u << (ctl.selVoice >= 0 && ctl.selVoice < kDrumVoices ? ctl.selVoice : 0);
                }
                sliceIn_ = h;
            }
            hitMask_[s] = m;
            if (m != 0) {
                drumGate_ = static_cast<float>(0.01 * fs_);
                lastHits_ |= m;
            }
            dg[s] = drumGate_ > 0.f ? 5.f : 0.f;
            drumGate_ -= 1.f;
        }
        drums.render(drumL_, drumR_, kBlock, hitMask_, vel_);
        break;
    }
    case N_RADIO: {
        float* r = router.out(O_RADIO);  // PLACEHOLDER: the radio is not on the patch bay yet
        for (int s = 0; s < kBlock; ++s) r[s] = 0.f;
        break;
    }
    case N_LFO: {
        // RONIN MG style triangle, tempo locked: one cycle every two beats, +-2.5 V
        float* o = router.out(O_LFO);
        const double inc = (ctl.bpm > 1.0 ? ctl.bpm : 1.0) / 60.0 / 2.0 / fs_;
        for (int s = 0; s < kBlock; ++s) {
            const double t = lfoPh_;
            const float tri = static_cast<float>(t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t);
            o[s] = 2.5f * tri;
            lfoPh_ += inc;
            if (lfoPh_ >= 1.0) lfoPh_ -= 1.0;
        }
        lfoOut_ = o[kBlock - 1];
        break;
    }
    case N_ENV: {
        // RONIN EG style AR on the keyboard gate: 5 ms attack, 300 ms release, 0..5 V
        float* o = router.out(O_ENV);
        const float aA = 1.f - std::exp(-1.f / (0.005f * static_cast<float>(fs_)));
        const float aR = 1.f - std::exp(-1.f / (0.3f * static_cast<float>(fs_)));
        const float target = aGate_ > 0.f ? 5.f : 0.f;
        for (int s = 0; s < kBlock; ++s) {
            env_ += (target > env_ ? aA : aR) * (target - env_);
            o[s] = env_;
        }
        if (env_ < 1e-6f) env_ = 0.f;
        break;
    }
    case N_SH: {
        // sample and hold on every clock step; unpatched it samples its own noise (+-5 V)
        float* o = router.out(O_SH);
        const float* in = router.in(I_SH);
        for (int s = 0; s < kBlock; ++s) {
            if (tickAt_[s] >= 0) {
                if (in != nullptr) {
                    sh_ = in[s];
                } else {
                    rng_ ^= rng_ << 13;
                    rng_ ^= rng_ >> 17;
                    rng_ ^= rng_ << 5;
                    sh_ = 5.f * (static_cast<float>(rng_ >> 8) / 8388608.f - 1.f);
                }
            }
            o[s] = sh_;
        }
        break;
    }
    case N_VCA: {
        // VCA IN x VCA CV / 5 V (linear, clamped 0..1); VCA CV unpatched follows ENV
        float* o = router.out(O_VCA);
        const float* in = router.in(I_VCAIN);
        const float* cv = router.in(I_VCACV);
        const float* env = router.out(O_ENV);
        for (int s = 0; s < kBlock; ++s) {
            const float g = clampf((cv != nullptr ? cv[s] : env[s]) * 0.2f, 0.f, 1.f);
            o[s] = in != nullptr ? in[s] * g : 0.f;
        }
        break;
    }
    case N_SLEW: {
        // lag processor (RONIN integrator style), 120 ms
        float* o = router.out(O_SLEW);
        const float* in = router.in(I_SLEW);
        const float a = 1.f - std::exp(-1.f / (0.12f * static_cast<float>(fs_)));
        for (int s = 0; s < kBlock; ++s) {
            slew_ += a * ((in != nullptr ? in[s] : 0.f) - slew_);
            o[s] = slew_;
        }
        break;
    }
    case N_QUANT: {
        // 1 V/oct to the nearest note of the major scale (key = TRANSPOSE)
        float* o = router.out(O_QUANT);
        const float* in = router.in(I_QUANT);
        for (int s = 0; s < kBlock; ++s) {
            const float x = in != nullptr ? in[s] : 0.f;
            const float n = x * 12.f - static_cast<float>(ctl.transpose);
            const float oct = std::floor(n / 12.f);
            const float r = n - 12.f * oct;
            int best = 0;
            for (int k = 1; k < 8; ++k) {
                if (std::fabs(r - static_cast<float>(kScale[k])) < std::fabs(r - static_cast<float>(kScale[best]))) {
                    best = k;
                }
            }
            o[s] = (oct * 12.f + static_cast<float>(kScale[best] + ctl.transpose)) / 12.f;
        }
        break;
    }
    case N_SYNTH: {
        // PITCH moves every voice by (PITCH - A PITCH) at 1 V/oct, so the
        // keyboard's own cable (A PITCH -> PITCH) is a straight wire and any
        // other source plays the held chord from C4. GATE adds triggers: a
        // rising edge plays the keyboard's last note, a falling edge releases it.
        SynthCv cv;
        const float* pin = router.in(I_PITCH);
        if (pin != nullptr) {
            const float a = static_cast<float>(lastNote_ - 60) / 12.f;
            for (int s = 0; s < kBlock; ++s) pitchRel_[s] = pin[s] - a;
            cv.pitch = pitchRel_;
        }
        cv.fm = router.in(I_FM);
        cv.cutoff = router.in(I_CUTOFF);
        const float* gate = router.in(I_GATE);
        if (gate != nullptr) {
            float mx = -10.f;
            for (int s = 0; s < kBlock; ++s) mx = std::fmax(mx, gate[s]);
            const bool h = schmitt(cvGate_, mx);
            if (h && !cvGate_) {
                cvNote_ = lastNote_;
                synth.noteOn(cvNote_, 0.85f);
            }
            if (!h && cvGate_ && !isHeld(cvNote_)) synth.noteOff(cvNote_);
            cvGate_ = h;
        } else if (cvGate_) {
            if (!isHeld(cvNote_)) synth.noteOff(cvNote_);
            cvGate_ = false;
        }
        synth.render(synthBuf_, kBlock, cv);
        break;
    }
    case N_FX: {
        for (int s = 0; s < kBlock; ++s) {
            srcL_[s] = synthBuf_[s] + drumL_[s];
            srcR_[s] = synthBuf_[s] + drumR_[s];
        }
        fx.setBypass(!ctl.fxOn);
        fx.process(srcL_, srcR_, kBlock, router.in(I_FXMAC));
        break;
    }
    case N_TAPE: {
        const float* sp = router.in(I_SPEED);
        const float* rv = router.in(I_REVERSE);
        const float* bi = router.in(I_BIAS);
        bool rev = false;
        if (rv != nullptr) {
            float mx = -10.f;
            for (int s = 0; s < kBlock; ++s) mx = std::fmax(mx, rv[s]);
            rev = mx > 1.5f;
        }
        tape.tapeBlock(srcL_, srcR_, sp, rv != nullptr, rev, bi, outL_, outR_, head1_, head2_, kBlock);
        float* h1 = router.out(O_HEAD1);
        float* h2 = router.out(O_HEAD2);
        for (int s = 0; s < kBlock; ++s) {
            h1[s] = 5.f * head1_[s];
            h2[s] = 5.f * head2_[s];
        }
        break;
    }
    default: break;
    }
}

}  // namespace lift::eng
