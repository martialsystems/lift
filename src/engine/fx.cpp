// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/fx.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace lift::eng {

const char* const kFxNames[kFxTypes] = {"SPRING", "PLATE", "ECHO"};
const char* const kFxKnobNames[kFxTypes][4] = {
    {"TONE", "DECAY", "TENSION", "MIX"}, {"SIZE", "DECAY", "TONE", "MIX"}, {"TIME", "FEEDBACK", "TONE", "MIX"}};
const float kFxDefaults[kFxTypes][4] = {{0.55f, 0.5f, 0.5f, 0.4f}, {0.6f, 0.55f, 0.5f, 0.3f}, {0.45f, 0.45f, 0.5f, 0.3f}};

namespace {

constexpr double kPi = 3.14159265358979323846;
inline float clampf(float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
inline float flush(float x) noexcept { return std::fabs(x) < 1e-20f ? 0.f : x; }
inline float onePoleA(double hz, double fs) noexcept { return static_cast<float>(1.0 - std::exp(-2.0 * kPi * hz / fs)); }
inline float softClip(float x) noexcept { return x / (1.f + std::fabs(x)) * 1.0f; }

// Circular delay line, power-of-two size, fractional (linear) reads.
struct Delay {
    std::vector<float> b;
    int mask = 0, w = 0;
    void alloc(int minLen) {
        int n = 1;
        while (n < minLen + 4) n <<= 1;
        b.assign(static_cast<size_t>(n), 0.f);
        mask = n - 1;
        w = 0;
    }
    void clear() { std::fill(b.begin(), b.end(), 0.f); }
    void push(float x) noexcept {
        b[static_cast<size_t>(w)] = x;
        w = (w + 1) & mask;
    }
    float at(int d) const noexcept { return b[static_cast<size_t>((w - 1 - d) & mask)]; }
    float frac(float d) const noexcept {
        const int i = static_cast<int>(d);
        const float f = d - static_cast<float>(i);
        const float a = at(i), c = at(i + 1);
        return a + f * (c - a);
    }
};

// Schroeder allpass on a delay line (Dattorro's tank and diffusers).
struct Allpass {
    Delay d;
    int len = 1;
    float process(float x, float g) noexcept {
        const float v = d.at(len - 1);
        const float u = x + g * v;  // Dattorro's sign convention: in + g*z^-N, out = z^-N - g*u
        d.push(flush(u));
        return v - g * u;
    }
    float processMod(float x, float g, float lenF) noexcept {
        const float v = d.frac(lenF - 1.f);
        const float u = x + g * v;
        d.push(flush(u));
        return v - g * u;
    }
};

}  // namespace

// ---------------------------------------------------------------- SPRING

struct FxRack::Spring {
    static constexpr int kSprings = 3;
    static constexpr int kStagesLf = 64;   // stretched allpasses in each low-chirp loop
    static constexpr int kStagesHf = 24;   // plain allpasses in each high-chirp loop
    static constexpr int kHist = 16;       // max stretch K + 1, power of two
    struct One {
        float xh[kStagesLf][kHist] = {};
        float yh[kStagesLf][kHist] = {};
        float hx[kStagesHf] = {}, hy[kStagesHf] = {};
        Delay lf, hf;
        float lfFb = 0.f, hfFb = 0.f;
        float lp1 = 0.f, lp2 = 0.f, hlp = 0.f;
        float dBase = 2000.f;   // loop delay (samples)
        float a = 0.65f;        // dispersion coefficient
        float modPh = 0.f, modRate = 0.f;
    };
    One s[kSprings];
    int t = 0;
    int K = 6;
    double fs = 48000.0;
    // input conditioning and output tone
    float inHp = 0.f, inLp = 0.f, outLpL = 0.f, outLpR = 0.f, outLpL2 = 0.f, outLpR2 = 0.f;
    // boing: transient detector and tank motion
    float envF = 0.f, envS = 0.f, hold = 0.f, shake = 0.f, shakePh = 0.f, kick = 0.f, kickLp = 0.f;
    // parameters (block rate)
    float gLf[kSprings] = {}, gHf[kSprings] = {}, lpA = 0.3f, toneA = 0.3f, tension = 0.5f;

    void prepare(double f) {
        fs = f;
        const float base[kSprings] = {41.3f, 47.9f, 55.1f};  // ms
        const float aa[kSprings] = {0.62f, 0.66f, 0.70f};
        const float rate[kSprings] = {0.37f, 0.53f, 0.29f};
        for (int i = 0; i < kSprings; ++i) {
            s[i].lf.alloc(static_cast<int>(base[i] * 1.6f * 0.001f * static_cast<float>(fs)) + 64);
            s[i].hf.alloc(static_cast<int>(base[i] * 1.6f * 0.001f * static_cast<float>(fs) / 2.3f) + 64);
            s[i].dBase = base[i] * 0.001f * static_cast<float>(fs);
            s[i].a = aa[i];
            s[i].modRate = rate[i];
        }
        reset();
    }
    void reset() {
        for (auto& o : s) {
            std::memset(o.xh, 0, sizeof o.xh);
            std::memset(o.yh, 0, sizeof o.yh);
            std::memset(o.hx, 0, sizeof o.hx);
            std::memset(o.hy, 0, sizeof o.hy);
            o.lf.clear();
            o.hf.clear();
            o.lfFb = o.hfFb = o.lp1 = o.lp2 = o.hlp = 0.f;
        }
        inHp = inLp = outLpL = outLpR = outLpL2 = outLpR2 = 0.f;
        envF = envS = hold = shake = kick = kickLp = 0.f;
    }
    void setParams(const float* p) {
        // TONE, DECAY, TENSION, MIX
        tension = p[2];
        const double fc = 2500.0 + 3500.0 * tension;          // chirp transition frequency
        K = static_cast<int>(std::lround(fs / (2.0 * fc)));
        if (K < 2) K = 2;
        if (K > kHist - 1) K = kHist - 1;
        const double t60 = 1.6 * std::pow(8.0, static_cast<double>(p[1]));  // loop t60 1.6 .. 12.8 s (tail ~0.7 .. 5 s)
        const float scale = 1.45f - 0.75f * tension;             // slack springs are longer
        for (int i = 0; i < kSprings; ++i) {
            const double d = s[i].dBase * scale;
            gLf[i] = static_cast<float>(std::pow(10.0, -3.0 * d / (t60 * fs)));
            gHf[i] = static_cast<float>(std::pow(10.0, -3.0 * (d / 2.3) / (0.6 * t60 * fs)));
        }
        lpA = onePoleA(fc, fs);
        toneA = onePoleA(1200.0 * std::pow(8.0, p[0]), fs);  // 1.2 .. 9.6 kHz
    }
    void kickNow(float amt) { kick += amt; shake = std::fmin(1.f, shake + amt); }

    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        const float hpA = onePoleA(120.0, fs);
        const float lpIn = onePoleA(5000.0, fs);
        const float aF = onePoleA(300.0, fs), aS = onePoleA(4.0, fs);
        const float shakeDecay = static_cast<float>(std::exp(-1.0 / (0.35 * fs)));
        const float kickA = onePoleA(180.0, fs);
        const float scale = 1.45f - 0.75f * tension;
        const float drip = 0.18f + 0.3f * (1.f - tension);
        for (int i = 0; i < n; ++i) {
            float x = 0.5f * (inL[i] + inR[i]);
            // input: band-limit like a tank's drive coil
            inHp += hpA * (x - inHp);
            x -= inHp;
            inLp += lpIn * (x - inLp);
            x = inLp;
            // boing: a fast envelope jumping well above the slow one shakes the tank
            const float ax = std::fabs(x);
            envF += aF * (ax - envF);
            envS += aS * (ax - envS);
            if (hold > 0.f) {
                hold -= 1.f;
            } else if (envF > 0.02f && envF > 3.f * envS + 0.01f) {
                kickNow(clampf(envF * 2.f, 0.f, 1.f));
                hold = static_cast<float>(0.12 * fs);
            }
            // the kick is a low thump into the springs (they turn it into the chirp)
            kickLp += kickA * (kick - kickLp);
            const float thump = kickLp * 0.3f;
            kick *= 0.995f;
            if (kick < 1e-6f) kick = 0.f;
            shake *= shakeDecay;
            shakePh += static_cast<float>(6.5 / fs);
            if (shakePh >= 1.f) shakePh -= 1.f;
            const float wob = shake * std::sin(static_cast<float>(2.0 * kPi) * shakePh);

            float outS[kSprings];
            for (int k = 0; k < kSprings; ++k) {
                One& o = s[k];
                // low chirp loop: input + feedback through 64 stretched allpasses
                float u = x + thump + o.lfFb;
                const float a = o.a + 0.06f * (1.f - tension);
                const int r = (t - K) & (kHist - 1);
                const int wi = t & (kHist - 1);
                for (int st = 0; st < kStagesLf; ++st) {
                    const float y = a * u + o.xh[st][r] - a * o.yh[st][r];
                    o.xh[st][wi] = u;
                    o.yh[st][wi] = flush(y);
                    u = y;
                }
                // keep it below the chirp transition (the stretched filter repeats above it)
                o.lp1 += lpA * (u - o.lp1);
                const float chirp = o.lp1;
                o.lf.push(chirp);
                o.modPh += static_cast<float>(o.modRate / fs);
                if (o.modPh >= 1.f) o.modPh -= 1.f;
                const float dl = o.dBase * scale *
                                 (1.f + 0.0015f * std::sin(static_cast<float>(2.0 * kPi) * o.modPh) + 0.02f * wob);
                o.lfFb = gLf[k] * o.lf.frac(dl);
                const float pre = o.lf.frac(dl * 0.2f);  // the drip: an early copy of the chirp
                // high chirp loop: shorter, plain allpasses, quieter
                float h = x + o.hfFb;
                for (int st = 0; st < kStagesHf; ++st) {
                    const float y = -0.6f * h + o.hx[st] + 0.6f * o.hy[st];
                    o.hx[st] = h;
                    o.hy[st] = flush(y);
                    h = y;
                }
                o.hlp += 0.5f * (h - o.hlp);
                o.hf.push(o.hlp);
                o.hfFb = gHf[k] * o.hf.frac(dl / 2.3f);
                outS[k] = chirp + drip * pre + 0.12f * o.hlp;
            }
            ++t;
            float l = outS[0] + 0.5f * outS[2];
            float rr = outS[1] + 0.5f * outS[2];
            outLpL += toneA * (l - outLpL);
            outLpR += toneA * (rr - outLpR);
            outLpL2 += toneA * (outLpL - outLpL2);
            outLpR2 += toneA * (outLpR - outLpR2);
            wl[i] = 0.9f * outLpL2;
            wr[i] = 0.9f * outLpR2;
        }
    }
};

// ---------------------------------------------------------------- PLATE (Dattorro 1997)

struct FxRack::Plate {
    Delay pre;
    Allpass in[4];
    Allpass apL1, apL2, apR1, apR2;
    Delay dL1, dL2, dR1, dR2;
    float bw = 0.f, dampL = 0.f, dampR = 0.f, fbL = 0.f, fbR = 0.f;
    float lfo = 0.f;
    double fs = 48000.0, k = 1.0;  // delay scale (fs / 29761)
    float size = 1.f, decay = 0.5f, damping = 0.3f, bwA = 0.7f;
    int L(int n) const { return static_cast<int>(n * k * size); }
    void prepare(double f) {
        fs = f;
        k = fs / 29761.0;
        pre.alloc(static_cast<int>(0.05 * fs));
        const int ins[4] = {142, 107, 379, 277};
        for (int i = 0; i < 4; ++i) in[i].d.alloc(static_cast<int>(ins[i] * k) + 8);
        apL1.d.alloc(static_cast<int>((672 + 32) * k) + 8);
        apR1.d.alloc(static_cast<int>((908 + 32) * k) + 8);
        apL2.d.alloc(static_cast<int>(1800 * k) + 8);
        apR2.d.alloc(static_cast<int>(2656 * k) + 8);
        dL1.alloc(static_cast<int>(4453 * k) + 8);
        dL2.alloc(static_cast<int>(3720 * k) + 8);
        dR1.alloc(static_cast<int>(4217 * k) + 8);
        dR2.alloc(static_cast<int>(3163 * k) + 8);
        reset();
    }
    void reset() {
        pre.clear();
        for (auto& a : in) a.d.clear();
        apL1.d.clear(); apL2.d.clear(); apR1.d.clear(); apR2.d.clear();
        dL1.clear(); dL2.clear(); dR1.clear(); dR2.clear();
        bw = dampL = dampR = fbL = fbR = 0.f;
    }
    void setParams(const float* p) {
        size = 0.45f + 0.55f * p[0];
        decay = 0.2f + 0.77f * p[1];
        damping = 0.65f - 0.6f * p[2];
        bwA = 0.35f + 0.6f * p[2];
        const int ins[4] = {142, 107, 379, 277};
        for (int i = 0; i < 4; ++i) in[i].len = static_cast<int>(ins[i] * k);
        apL2.len = L(1800);
        apR2.len = L(2656);
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        const float dd1 = 0.7f, dd2 = 0.5f;
        const float lfoInc = static_cast<float>(2.0 * kPi * 1.0 / fs);
        const int pd = static_cast<int>(0.012 * fs);
        for (int i = 0; i < n; ++i) {
            float x = 0.5f * (inL[i] + inR[i]);
            pre.push(x);
            x = pre.at(pd);
            bw += bwA * (x - bw);
            x = bw;
            x = in[0].process(x, 0.75f);
            x = in[1].process(x, 0.75f);
            x = in[2].process(x, 0.625f);
            x = in[3].process(x, 0.625f);
            lfo += lfoInc;
            if (lfo > static_cast<float>(2.0 * kPi)) lfo -= static_cast<float>(2.0 * kPi);
            const float ex = static_cast<float>(8.0 * k) * std::sin(lfo);
            // left tank
            float l = x + fbR;
            l = apL1.processMod(l, -dd1, static_cast<float>(672 * k * size) + ex);
            dL1.push(l);
            l = dL1.at(L(4453) - 1);
            dampL += (1.f - damping) * (l - dampL);
            l = apL2.process(dampL * decay, dd2);
            dL2.push(l);
            const float outLtank = dL2.at(L(3720) - 1);
            // right tank
            float r = x + fbL;
            r = apR1.processMod(r, -dd1, static_cast<float>(908 * k * size) - ex);
            dR1.push(r);
            r = dR1.at(L(4217) - 1);
            dampR += (1.f - damping) * (r - dampR);
            r = apR2.process(dampR * decay, dd2);
            dR2.push(r);
            const float outRtank = dR2.at(L(3163) - 1);
            fbL = flush(outLtank * decay);
            fbR = flush(outRtank * decay);
            // output taps (Dattorro's table, scaled)
            float yl = dR1.at(L(266)) + dR1.at(L(2974)) - apR2.d.at(L(1913)) + dR2.at(L(1996)) - dL1.at(L(1990)) -
                       apL2.d.at(L(187)) - dL2.at(L(1066));
            float yr = dL1.at(L(353)) + dL1.at(L(3627)) - apL2.d.at(L(1228)) + dL2.at(L(2673)) - dR1.at(L(2111)) -
                       apR2.d.at(L(335)) - dR2.at(L(121));
            wl[i] = 0.6f * yl;
            wr[i] = 0.6f * yr;
        }
    }
};

// ---------------------------------------------------------------- ECHO

struct FxRack::Echo {
    Delay dl, dr;
    double fs = 48000.0;
    float time = 0.3f, timeS = 0.3f, fb = 0.4f, lpA = 0.3f, hpA = 0.01f;
    float lpL = 0.f, lpR = 0.f, hpL = 0.f, hpR = 0.f, wow = 0.f;
    void prepare(double f) {
        fs = f;
        dl.alloc(static_cast<int>(1.4 * fs));
        dr.alloc(static_cast<int>(1.4 * fs));
        reset();
    }
    void reset() {
        dl.clear();
        dr.clear();
        lpL = lpR = hpL = hpR = 0.f;
    }
    void setParams(const float* p) {
        time = static_cast<float>(0.04 * std::pow(30.0, p[0]) * fs);  // 40 ms .. 1.2 s
        fb = 1.02f * p[1];
        lpA = onePoleA(900.0 * std::pow(12.0, p[2]), fs);
        hpA = onePoleA(90.0, fs);
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        const float glide = static_cast<float>(1.0 - std::exp(-1.0 / (0.12 * fs)));  // tape-like time changes
        const float wowInc = static_cast<float>(0.7 / fs);
        for (int i = 0; i < n; ++i) {
            timeS += glide * (time - timeS);
            wow += wowInc;
            if (wow >= 1.f) wow -= 1.f;
            const float d = timeS * (1.f + 0.002f * std::sin(static_cast<float>(2.0 * kPi) * wow));
            float l = dl.frac(d);
            float r = dr.frac(d * 1.01f);
            lpL += lpA * (l - lpL);
            lpR += lpA * (r - lpR);
            hpL += hpA * (lpL - hpL);
            hpR += hpA * (lpR - hpR);
            l = lpL - hpL;
            r = lpR - hpR;
            dl.push(flush(inL[i] + softClip(fb * l)));
            dr.push(flush(inR[i] + softClip(fb * r)));
            wl[i] = l;
            wr[i] = r;
        }
    }
};

// ---------------------------------------------------------------- rack

FxRack::FxRack()
    : spring_(std::make_unique<Spring>()), plate_(std::make_unique<Plate>()), echo_(std::make_unique<Echo>()) {}
FxRack::~FxRack() = default;

void FxRack::prepare(double fs) {
    fs_ = fs;
    spring_->prepare(fs);
    plate_->prepare(fs);
    echo_->prepare(fs);
    setParams(p_);
}

void FxRack::reset() noexcept {
    spring_->reset();
    plate_->reset();
    echo_->reset();
}

void FxRack::setType(int t) noexcept {
    if (t < 0 || t >= kFxTypes || t == type_) {
        return;
    }
    type_ = t;
    reset();
    setParams(p_);
}

void FxRack::setParams(const float p[4]) noexcept {
    for (int k = 0; k < 4; ++k) {
        p_[k] = clampf(p[k], 0.f, 1.f);
    }
    switch (type_) {
    case FX_SPRING: spring_->setParams(p_); break;
    case FX_PLATE: plate_->setParams(p_); break;
    default: echo_->setParams(p_); break;
    }
}

float FxRack::shake() const noexcept { return spring_->shake; }

void FxRack::process(float* L, float* R, int n, const float* mac) noexcept {
    if (n > 64) {
        n = 64;
    }
    const float target = bypass_ ? 0.f : 1.f;
    if (bypass_ && wet_ == 0.f) {
        return;
    }
    float inL[64], inR[64], wl[64], wr[64];
    const float ramp = static_cast<float>(1.0 / (0.01 * fs_));
    float wetIn = wet_;
    for (int i = 0; i < n; ++i) {
        wetIn += wetIn < target ? ramp : -ramp;
        wetIn = clampf(wetIn, 0.f, 1.f);
        inL[i] = L[i] * wetIn;
        inR[i] = R[i] * wetIn;
    }
    float macMean = 0.f;
    if (mac != nullptr) {
        for (int i = 0; i < n; ++i) {
            macMean += mac[i];
            if (type_ == FX_SPRING && mac[i] > 2.5f && macPrev_ <= 2.5f) {
                spring_->kickNow(0.8f);  // FX MAC rising edge: shake the tank
            }
            macPrev_ = mac[i];
        }
        macMean /= static_cast<float>(n);
    }
    switch (type_) {
    case FX_SPRING: spring_->process(inL, inR, wl, wr, n); break;
    case FX_PLATE: plate_->process(inL, inR, wl, wr, n); break;
    default: echo_->process(inL, inR, wl, wr, n); break;
    }
    const float mix = clampf(p_[3] + macMean / 5.f, 0.f, 1.f);
    float w = wet_;
    for (int i = 0; i < n; ++i) {
        w += w < target ? ramp : -ramp;
        w = clampf(w, 0.f, 1.f);
        const float dryG = 1.f - 0.5f * mix * w;
        L[i] = L[i] * dryG + wl[i] * mix * w * 1.4f;
        R[i] = R[i] * dryG + wr[i] * mix * w * 1.4f;
    }
    wet_ = w;
}

}  // namespace lift::eng
