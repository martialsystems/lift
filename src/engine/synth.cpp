// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/synth.h"

#include "dsp.h"  // third_party/jidai/shogun: Jidai DSP blocks

#include <cmath>
#include <cstring>

namespace lift::eng {

namespace {

using shogun::dsp::flushDenormal;
constexpr double kTwoPi = 6.283185307179586;
constexpr int kStringLen = 8192;  // WIRE delay line per voice: 96 kHz / 8192 = 11.7 Hz lowest

inline float clampf(float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
inline double mtof(double n) noexcept { return 440.0 * std::exp2((n - 69.0) / 12.0); }

// 2^x for |x| < ~8, ~1e-5 relative (audio-rate FM / pitch CV per sample).
inline float fastExp2(float x) noexcept {
    x = clampf(x, -24.f, 24.f);
    const float fl = std::floor(x);
    const float f = x - fl;
    const float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    return std::ldexp(p, static_cast<int>(fl));
}

// sin(2 pi x) for any x: wrap, then a 7th-order odd polynomial (~1e-4).
inline float sin2pi(double x) noexcept {
    double t = x - std::floor(x);           // 0..1
    float u = static_cast<float>(t < 0.5 ? t : t - 1.0);  // -0.5..0.5
    // fold to -0.25..0.25
    if (u > 0.25f) u = 0.5f - u;
    else if (u < -0.25f) u = -0.5f - u;
    const float z = u * 6.2831853f;
    const float z2 = z * z;
    return z * (1.f - z2 * (0.16666667f - z2 * (0.0083333f - z2 * 0.000198413f)));
}

// Exponential ADSR. tick() returns the level, 0..1.
struct Adsr {
    double e = 0.0, aA = 0.0, aD = 0.0, aR = 0.0, sus = 0.5;
    int stage = 0;  // 0 idle, 1 attack, 2 decay/sustain, 3 release
    void set(double atk, double dec, double s, double rel, double fs) noexcept {
        aA = atk <= 0.0005 ? 0.0 : std::exp(-1.0 / (atk * fs));
        aD = std::exp(-1.0 / (dec * fs));
        aR = std::exp(-1.0 / (rel * fs));
        sus = s;
    }
    void on() noexcept { stage = aA <= 0.0 ? 2 : 1; if (stage == 2) e = 1.0; }
    void off() noexcept { if (stage != 0) stage = 3; }
    double tick() noexcept {
        switch (stage) {
        case 1:
            e = 1.25 + aA * (e - 1.25);
            if (e >= 1.0) { e = 1.0; stage = 2; }
            break;
        case 2: e = sus + aD * (e - sus); break;
        case 3:
            e *= aR;
            if (e < 1e-5) { e = 0.0; stage = 0; }
            break;
        default: break;
        }
        return e;
    }
};

// LIFT's own wavefolder: a sine folding law (smooth, so it aliases far less
// than a hard triangle fold) after a symmetry skew and an offset.
inline double foldLaw(double x) noexcept { return std::sin(1.5707963267948966 * x); }

// RATIO: nine 4-operator routings. mods[i] = bitmask of operators that
// modulate operator i; carriers = bitmask summed to the output. Operator 3
// carries the feedback.
struct Algo {
    std::uint8_t mods[4];
    std::uint8_t carriers;
};
constexpr Algo kAlgos[9] = {
    {{0x2, 0x4, 0x8, 0}, 0x1},          // 4>3>2>1
    {{0x2, 0xC, 0, 0}, 0x1},            // (3+4)>2>1
    {{0x6, 0, 0x8, 0}, 0x1},            // 2>1, 4>3>1
    {{0x2, 0, 0x8, 0}, 0x5},            // 2>1 + 4>3
    {{0x8, 0x8, 0x8, 0}, 0x7},          // 4 > 1,2,3
    {{0, 0x4, 0x8, 0}, 0x3},            // 1 + 4>3>2
    {{0, 0, 0x8, 0}, 0x7},              // 1 + 2 + 4>3
    {{0xE, 0, 0, 0}, 0x1},              // (2+3+4)>1
    {{0, 0, 0, 0}, 0xF},                // additive
};
constexpr float kRatioSets[8][4] = {
    {1.f, 1.f, 1.f, 1.f},     {1.f, 2.f, 3.f, 4.f},   {1.f, 2.f, 1.f, 0.5f},  {1.f, 3.5f, 7.f, 1.f},
    {1.f, 1.414f, 2.f, 2.828f}, {1.f, 0.5f, 2.f, 4.f}, {1.f, 14.f, 1.f, 3.f}, {0.5f, 7.f, 3.f, 1.5f},
};

}  // namespace

struct PolySynth::Voice {
    bool active = false;
    bool gate = false;
    bool held = false;   // released by the key but kept by the sustain pedal
    int note = -1;
    float vel = 0.8f;
    std::uint32_t age = 0;
    double ph[8] = {};   // oscillator / operator phases
    double fbPrev[2] = {};
    Adsr amp, fil;
    shogun::dsp::ZdfLadder ladder;
    shogun::dsp::OtaSvf ota;
    double dc = 0.0, dcIn = 0.0;  // DC blocker (FOLD offset)
    // WIRE
    float* str = nullptr;
    int w = 0;
    double apState = 0.0, lpState = 0.0;
    // SWARM: quadrature oscillators
    double qs[10] = {}, qc[10] = {};
    // SPOOL
    double pos = 0.0;
    double lastOut = 0.0;
};

struct PolySynth::Dec {
    shogun::dsp::Decimator d;
};

PolySynth::PolySynth() : v_(new Voice[kSynthVoices]), dec_(std::make_unique<Dec>()) {}
PolySynth::~PolySynth() = default;

void PolySynth::prepare(double fs) {
    fs_ = fs;
    fsE_ = 2.0 * fs;
    strings_.reset(new float[static_cast<size_t>(kStringLen) * kSynthVoices]);
    std::memset(strings_.get(), 0, sizeof(float) * static_cast<size_t>(kStringLen) * kSynthVoices);
    for (int i = 0; i < kSynthVoices; ++i) {
        v_[i] = Voice{};
        v_[i].str = strings_.get() + static_cast<size_t>(i) * kStringLen;
    }
    dec_->d.prepare(2);
    reset();
}

void PolySynth::reset() noexcept {
    for (int i = 0; i < kSynthVoices; ++i) {
        float* s = v_[i].str;
        v_[i] = Voice{};
        v_[i].str = s;
    }
    dec_->d.reset();
}

void PolySynth::setEngine(int e) noexcept {
    if (e < 0 || e >= kSynthEngines || e == engine_) {
        return;
    }
    engine_ = e;
    allOff();
}

void PolySynth::setMacros(const float m[4]) noexcept {
    for (int k = 0; k < 4; ++k) {
        m_[k] = clampf(m[k], 0.f, 1.f);
    }
}

void PolySynth::setSpoolSource(const float* l, const float* r, int frames, int loopStart, int loopEnd) noexcept {
    spoolL_ = l;
    spoolR_ = r;
    spoolFrames_ = frames;
    spoolLoopStart_ = loopStart;
    spoolLoopEnd_ = loopEnd;
}

void PolySynth::startVoice(Voice& v, int note, float vel) noexcept {
    const bool retrig = v.active;
    v.active = true;
    v.gate = true;
    v.held = false;
    v.note = note;
    v.vel = vel;
    v.age = ++age_;
    if (!retrig) {
        for (double& p : v.ph) p = 0.0;
        v.fbPrev[0] = v.fbPrev[1] = 0.0;
        v.ladder.reset();
        v.ota.reset();
        v.amp.e = 0.0;
        v.fil.e = 0.0;
        v.dc = v.dcIn = 0.0;
    }
    if (engine_ == SWARM) {
        for (int k = 0; k < 10; ++k) {
            const double a = kTwoPi * (0.137 * k * k + 0.31 * k);  // spread start phases (no unison click)
            v.qs[k] = std::sin(a);
            v.qc[k] = std::cos(a);
        }
    }
    if (engine_ == WIRE) {
        // excitation: a noise burst, low-passed by TONE, combed by PLUCK POS
        const double f = mtof(note);
        const int L = static_cast<int>(clampf(static_cast<float>(fsE_ / f), 8.f, kStringLen - 4.f));
        const float a = 0.05f + 0.9f * m_[2] * m_[2];
        float lp = 0.f;
        for (int i = 0; i < kStringLen; ++i) {
            v.str[i] = 0.f;
        }
        for (int i = 0; i < L; ++i) {
            rng_ = rng_ * 1664525u + 1013904223u;
            const float nz = static_cast<float>(static_cast<int32_t>(rng_)) * (1.f / 2147483648.f);
            lp += a * (nz - lp);
            v.str[i] = lp;
        }
        const int pp = 1 + static_cast<int>(static_cast<float>(L) * (0.05f + 0.45f * m_[1]));
        for (int i = L - 1; i >= pp; --i) {
            v.str[i] -= v.str[i - pp];
        }
        // normalise the burst
        float pk = 1e-6f;
        for (int i = 0; i < L; ++i) {
            pk = std::fmax(pk, std::fabs(v.str[i]));
        }
        for (int i = 0; i < L; ++i) {
            v.str[i] *= 0.9f * vel / pk;
        }
        v.w = L;  // the read tap (w - L) starts on the burst
        v.apState = v.lpState = 0.0;
    }
    if (engine_ == SPOOL) {
        const int frames = spoolLoopEnd_ > spoolLoopStart_ ? spoolLoopEnd_ - spoolLoopStart_ : spoolFrames_;
        const int base = spoolLoopEnd_ > spoolLoopStart_ ? spoolLoopStart_ : 0;
        v.pos = base + static_cast<double>(m_[0]) * frames;
    }
    v.amp.on();
    v.fil.on();
}

void PolySynth::noteOn(int note, float vel) noexcept {
    lastNote_ = note;
    // same note again: retrigger that voice
    for (int i = 0; i < kSynthVoices; ++i) {
        if (v_[i].active && v_[i].note == note) {
            startVoice(v_[i], note, vel);
            return;
        }
    }
    Voice* pick = nullptr;
    for (int i = 0; i < kSynthVoices; ++i) {
        if (!v_[i].active) {
            pick = &v_[i];
            break;
        }
    }
    if (pick == nullptr) {  // steal: the oldest released voice, else the oldest
        for (int pass = 0; pass < 2 && pick == nullptr; ++pass) {
            for (int i = 0; i < kSynthVoices; ++i) {
                if ((pass == 1 || !v_[i].gate) && (pick == nullptr || v_[i].age < pick->age)) {
                    pick = &v_[i];
                }
            }
        }
    }
    startVoice(*pick, note, vel);
}

void PolySynth::noteOff(int note) noexcept {
    for (int i = 0; i < kSynthVoices; ++i) {
        Voice& v = v_[i];
        if (v.active && v.gate && (note < 0 || v.note == note)) {
            if (sustain_) {
                v.held = true;
            } else {
                v.gate = false;
                v.amp.off();
                v.fil.off();
            }
        }
    }
}

void PolySynth::allOff() noexcept {
    for (int i = 0; i < kSynthVoices; ++i) {
        v_[i].active = false;
        v_[i].gate = false;
        v_[i].amp.stage = 0;
        v_[i].amp.e = 0.0;
    }
}

void PolySynth::setSustain(bool on) noexcept {
    sustain_ = on;
    if (!on) {
        for (int i = 0; i < kSynthVoices; ++i) {
            if (v_[i].held) {
                v_[i].held = false;
                v_[i].gate = false;
                v_[i].amp.off();
                v_[i].fil.off();
            }
        }
    }
}

bool PolySynth::voiceActive(int v) const noexcept { return v >= 0 && v < kSynthVoices && v_[v].active; }
float PolySynth::voiceEnv(int v) const noexcept {
    return v >= 0 && v < kSynthVoices && v_[v].active ? static_cast<float>(v_[v].amp.e) : 0.f;
}
int PolySynth::voiceNote(int v) const noexcept { return v >= 0 && v < kSynthVoices && v_[v].active ? v_[v].note : -1; }
float PolySynth::env() const noexcept {
    float e = 0.f;
    for (int i = 0; i < kSynthVoices; ++i) {
        e = std::fmax(e, voiceEnv(i));
    }
    return e;
}
bool PolySynth::anyGate() const noexcept {
    for (int i = 0; i < kSynthVoices; ++i) {
        if (v_[i].active && v_[i].gate) return true;
    }
    return false;
}

void PolySynth::render(float* out, int n, const SynthCv& cv) noexcept {
    if (n > 64) {
        n = 64;
    }
    const int n2 = 2 * n;
    for (int i = 0; i < n2; ++i) {
        scratch_[i] = 0.f;
    }
    // vibrato (mod wheel), at block rate
    vibPh_ += 5.5 * n / fs_;
    vibPh_ -= std::floor(vibPh_);
    bool any = false;
    if (engine_ != SPARE) {
        for (int k = 0; k < kSynthVoices; ++k) {
            if (v_[k].active) {
                renderVoice(v_[k], scratch_, n2, cv, n);
                any = true;
            }
        }
    }
    (void)any;
    int o = 0;
    for (int i = 0; i < n2; ++i) {
        double y;
        if (dec_->d.push(static_cast<double>(scratch_[i]), y)) {
            out[o++] = static_cast<float>(y);
        }
    }
    for (; o < n; ++o) {
        out[o] = 0.f;
    }
}

void PolySynth::renderVoice(Voice& v, float* dst, int n2, const SynthCv& cv, int n) noexcept {
    const double fsE = fsE_;
    const float* m = m_;
    // block-rate pitch: note + bend + vibrato + PITCH CV (block mean), per-sample FM on top
    float pcv = 0.f, ccv = 0.f;
    if (cv.pitch != nullptr) {
        for (int i = 0; i < n; ++i) pcv += cv.pitch[i];
        pcv /= static_cast<float>(n);
    }
    if (cv.cutoff != nullptr) {
        for (int i = 0; i < n; ++i) ccv += cv.cutoff[i];
        ccv /= static_cast<float>(n) * 25.f;  // +5 V = +0.2 of the knob's range
    }
    const double vib = 0.35 * mod_ * std::sin(kTwoPi * vibPh_);
    const double note = v.note + bend_ + vib + 12.0 * pcv;
    const double f0 = mtof(note);
    const float vel = 0.35f + 0.65f * v.vel;
    auto fmAt = [&](int i) -> float {  // i at the oversampled rate
        return cv.fm != nullptr ? 0.25f * cv.fm[i >> 1] : 0.f;  // 1 V = 1/4 semitone
    };
    float* d = dst;

    switch (engine_) {
    case LOOM: {
        // two PolyBLEP oscillators (saw + square, DETUNE apart) into the ZDF ladder
        const double det = std::exp2((2.0 + 38.0 * m[0] * m[0]) / 1200.0);
        const float cut = clampf(m[1] + ccv, 0.f, 1.f);
        const double envAmt = (m[2] - 0.5) * 2.0 * 6.0;  // +-6 octaves, centre = none
        const double dec = 0.04 * std::pow(80.0, m[3]);
        v.amp.set(0.003, dec, 0.45, 0.05 + 0.5 * dec, fsE);
        v.fil.set(0.002, 0.6 * dec, 0.25, 0.05 + 0.5 * dec, fsE);
        const double dt1 = f0 / fsE, dt2 = f0 * det / fsE;
        for (int i = 0; i < n2; ++i) {
            const float fm = fastExp2(fmAt(i) / 12.f);
            const double a = v.amp.tick();
            const double fe = v.fil.tick();
            const double t1 = v.ph[0], t2 = v.ph[1];
            const double d1 = dt1 * fm, d2 = dt2 * fm;
            const double saw = 2.0 * t1 - 1.0 - shogun::dsp::polyBlep(t1, d1);
            const double t2b = t2 + 0.5 - std::floor(t2 + 0.5);
            const double sq = (2.0 * t2 - 1.0 - shogun::dsp::polyBlep(t2, d2)) -
                              (2.0 * t2b - 1.0 - shogun::dsp::polyBlep(t2b, d2));
            v.ph[0] += d1;
            if (v.ph[0] >= 1.0) v.ph[0] -= 1.0;
            v.ph[1] += d2;
            if (v.ph[1] >= 1.0) v.ph[1] -= 1.0;
            if ((i & 3) == 0) {
                const double oct = 9.6 * cut + envAmt * fe;
                v.ladder.set(26.0 * std::exp2(oct), fsE);
            }
            const double y = v.ladder.tick(0.5 * (saw + 0.8 * sq), 1.6, 1.4);
            d[i] += static_cast<float>(0.6 * vel * a * y);
        }
        break;
    }
    case BEND: {
        // phase distortion on one cosine carrier: the knee follows AMOUNT and the envelope
        const float amt = clampf(m[0], 0.f, 1.f);
        const float cut = clampf(m[1] + ccv, 0.f, 1.f);
        const double atk = 0.001 * std::pow(1500.0, m[2]);
        const double dec = 0.05 * std::pow(60.0, m[3]);
        v.amp.set(atk, dec, 0.5, 0.05 + 0.4 * dec, fsE);
        v.fil.set(atk * 0.5, dec * 0.5, 0.3, 0.05 + 0.4 * dec, fsE);
        v.ota.svf.set(60.0 * std::pow(330.0, cut), 0.65, fsE);
        const double dt = f0 / fsE;
        for (int i = 0; i < n2; ++i) {
            const double a = v.amp.tick();
            const double fe = v.fil.tick();
            const double pd = amt * (0.25 + 0.75 * fe);
            const double knee = 0.5 - 0.485 * pd;
            const double t = v.ph[0];
            const double tw = t < knee ? 0.5 * t / knee : 0.5 + 0.5 * (t - knee) / (1.0 - knee);
            const double y = -std::cos(kTwoPi * tw);
            v.ph[0] += dt * fastExp2(fmAt(i) / 12.f);
            if (v.ph[0] >= 1.0) v.ph[0] -= 1.0;
            const double yf = v.ota.tick(y);
            d[i] += static_cast<float>(0.24 * vel * a * yf);
        }
        break;
    }
    case FOLD: {
        // sine -> symmetry skew -> offset -> LIFT's sine folder; the envelope opens the fold
        const float fold = clampf(m[0] + ccv, 0.f, 1.f);
        const double sym = 0.5 + 0.42 * (m[1] - 0.5) * 2.0;  // skew point of the input sine
        const double off = (m[2] - 0.5) * 1.6;
        const double dec = 0.06 * std::pow(50.0, m[3]);
        v.amp.set(0.002, dec, 0.35, 0.06 + 0.4 * dec, fsE);
        v.fil.set(0.001, 0.5 * dec, 0.2, 0.06 + 0.4 * dec, fsE);
        const double dt = f0 / fsE;
        const double hpA = 1.0 - std::exp(-kTwoPi * 18.0 / fsE);
        for (int i = 0; i < n2; ++i) {
            const double a = v.amp.tick();
            const double fe = v.fil.tick();
            const double t = v.ph[0];
            const double ts = t < sym ? 0.5 * t / sym : 0.5 + 0.5 * (t - sym) / (1.0 - sym);
            const double x = sin2pi(ts);
            const double g = 1.0 + 9.0 * fold * (0.3 + 0.7 * fe);
            const double y0 = foldLaw(g * (x + off * fe));
            // DC blocker (the offset makes even harmonics and DC)
            v.dc += hpA * (y0 - v.dc);
            const double y = y0 - v.dc;
            v.ph[0] += dt * fastExp2(fmAt(i) / 12.f);
            if (v.ph[0] >= 1.0) v.ph[0] -= 1.0;
            d[i] += static_cast<float>(0.26 * vel * a * y);
        }
        break;
    }
    case RATIO: {
        const int alg = static_cast<int>(clampf(m[0], 0.f, 0.999f) * 9.f);
        const int set = static_cast<int>(clampf(m[1], 0.f, 0.999f) * 8.f);
        // keyboard scaling (as FM hardware does): less index up high, where
        // the sidebands would fold over Nyquist
        const double ks = f0 > 500.0 ? std::sqrt(500.0 / f0) : 1.0;
        const double idx0 = 6.0 * static_cast<double>(m[2]) * m[2] * ks;
        const double fb = 1.1 * m[3] * ks;
        const Algo& A = kAlgos[alg];
        const float* R = kRatioSets[set];
        v.amp.set(0.002, 1.4, 0.55, 0.35, fsE);
        v.fil.set(0.001, 0.35, 0.25, 0.3, fsE);  // modulation-index envelope
        int nc = 0;
        for (int k = 0; k < 4; ++k) nc += (A.carriers >> k) & 1;
        const double cg = 1.0 / nc;
        double dt[4];
        for (int k = 0; k < 4; ++k) dt[k] = f0 * R[k] / fsE;
        for (int i = 0; i < n2; ++i) {
            const double a = v.amp.tick();
            const double fe = v.fil.tick();
            const float fmv = fmAt(i);
            const double I = (idx0 + (cv.fm != nullptr ? fmv : 0.0)) * (0.35 + 0.65 * fe);
            double y[4];
            // operator 3 first (feedback, averaged over two samples for stability)
            y[3] = sin2pi(v.ph[3] + fb * 0.5 * (v.fbPrev[0] + v.fbPrev[1]) * 0.25);
            v.fbPrev[1] = v.fbPrev[0];
            v.fbPrev[0] = y[3];
            for (int k = 2; k >= 0; --k) {
                double pm = 0.0;
                for (int j = k + 1; j < 4; ++j) {
                    if ((A.mods[k] >> j) & 1) pm += y[j];
                }
                y[k] = sin2pi(v.ph[k] + I * pm * 0.25);
            }
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                if ((A.carriers >> k) & 1) s += y[k];
                v.ph[k] += dt[k];
                if (v.ph[k] >= 1.0) v.ph[k] -= std::floor(v.ph[k]);
            }
            d[i] += static_cast<float>(0.3 * vel * a * s * cg);
        }
        break;
    }
    case WIRE: {
        // Karplus-Strong string: delay line, first-order allpass for the fraction, damping LP
        const double f = f0;
        double Lf = fsE / f - 0.5;  // the averaging filter adds half a sample
        if (Lf < 4.0) Lf = 4.0;
        if (Lf > kStringLen - 4) Lf = kStringLen - 4;
        const int L = static_cast<int>(Lf);
        const double frac = Lf - L;
        const double apC = (1.0 - frac) / (1.0 + frac);
        const float tone = clampf(m[2] + ccv, 0.f, 1.f);
        const double damp = 0.05 + 0.9 * (1.0 - m[0]) * (0.6 + 0.4 * (1.0 - tone));
        const double t60 = v.gate ? 0.3 * std::pow(40.0, m[3]) : 0.12;
        const double g = std::pow(10.0, -3.0 / (t60 * f));
        v.amp.set(0.0005, 10.0, 1.0, 0.25, fsE);
        for (int i = 0; i < n2; ++i) {
            const double a = v.amp.tick();
            int r = v.w - L;
            if (r < 0) r += kStringLen;
            const double x = v.str[r];
            // averaging lowpass (damping), then the allpass fraction
            const double lp = (1.0 - damp) * x + damp * v.lpState;
            v.lpState = x;
            const double ap = apC * lp + v.apState;
            v.apState = lp - apC * ap;
            const double y = g * ap;
            v.str[v.w] = static_cast<float>(flushDenormal(y));
            v.w = (v.w + 1) & (kStringLen - 1);
            v.lastOut = x;
            d[i] += static_cast<float>(0.6 * a * x);
        }
        if (!v.gate && v.amp.stage == 0) {
            v.active = false;
        }
        break;
    }
    case SWARM: {
        // detuned sines, one amplitude envelope; DENSITY adds partners and octaves
        const float spread = clampf(m[0] + ccv, 0.f, 1.f);
        const int count = 2 + static_cast<int>(m[1] * 8.f);  // 2..10
        const double atk = 0.002 * std::pow(1500.0, m[2]);
        const double rel = 0.03 * std::pow(200.0, m[3]);
        v.amp.set(atk, 2.0, 0.85, rel, fsE);
        double cs[10], sn[10];
        for (int k = 0; k < count; ++k) {
            const double cents = spread * 45.0 * ((k % 2) ? 1.0 : -1.0) * (0.3 + 0.7 * ((k + 1) / 2) / 5.0);
            double oct = 0.0;
            if (m[1] > 0.55 && k == count - 1) oct = 12.0;
            if (m[1] > 0.8 && k == count - 2) oct = -12.0;
            const double fk = f0 * std::exp2((cents / 100.0 + oct) / 12.0);
            const double w = kTwoPi * fk / fsE;
            cs[k] = std::cos(w);
            sn[k] = std::sin(w);
        }
        const double gk = 0.2 / std::sqrt(static_cast<double>(count));
        for (int i = 0; i < n2; ++i) {
            const double a = v.amp.tick();
            double s = 0.0;
            for (int k = 0; k < count; ++k) {
                const double x = v.qc[k] * cs[k] - v.qs[k] * sn[k];
                const double y = v.qs[k] * cs[k] + v.qc[k] * sn[k];
                v.qc[k] = x;
                v.qs[k] = y;
                s += y;
            }
            d[i] += static_cast<float>(gk * vel * a * s);
        }
        // keep the oscillators on the unit circle
        for (int k = 0; k < count; ++k) {
            const double r = 1.0 / std::sqrt(v.qs[k] * v.qs[k] + v.qc[k] * v.qc[k]);
            v.qs[k] *= r;
            v.qc[k] *= r;
        }
        break;
    }
    case SPOOL: {
        // one tape track as a sample: START, LENGTH, PITCH (+-12), DECAY; root C4 plays at the recorded pitch
        if (spoolL_ == nullptr || spoolFrames_ <= 0) {
            v.active = false;
            break;
        }
        const bool loop = spoolLoopEnd_ > spoolLoopStart_;
        const int base = loop ? spoolLoopStart_ : 0;
        const int span = loop ? spoolLoopEnd_ - spoolLoopStart_ : spoolFrames_;
        const double start = base + static_cast<double>(m[0]) * span;
        const double len = 0.01 * fs_ + static_cast<double>(m[1]) * m[1] * span;
        const double rate = std::exp2((v.note - 60 + (m[2] - 0.5) * 24.0 + bend_ + 12.0 * pcv) / 12.0) * fs_ / fsE;
        const double dec = 0.05 * std::pow(80.0, m[3]);
        v.amp.set(0.002, dec, 0.0, 0.08, fsE);
        for (int i = 0; i < n2; ++i) {
            const double a = v.amp.tick();
            double p = v.pos;
            if (p >= start + len || p >= spoolFrames_ - 2) {
                p = start;  // loop the slice
            }
            const int i0 = static_cast<int>(p);
            const float fr = static_cast<float>(p - i0);
            const float l = spoolL_[i0] + fr * (spoolL_[i0 + 1] - spoolL_[i0]);
            const float r = spoolR_ != nullptr ? spoolR_[i0] + fr * (spoolR_[i0 + 1] - spoolR_[i0]) : l;
            v.pos = p + rate * fastExp2(fmAt(i) / 12.f);
            d[i] += static_cast<float>(0.9 * vel * a * 0.5 * (l + r));
        }
        if (v.amp.e < 1e-4 && v.amp.stage == 2) {
            v.active = false;
        }
        break;
    }
    default:
        v.active = false;
        break;
    }
    if (v.amp.stage == 0 && !v.gate) {
        v.active = false;
    }
}

}  // namespace lift::eng
