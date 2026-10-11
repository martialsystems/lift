// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/fx.h"

#include <jidai/dsp/Halfband.h>  // vendored jidai-common: exact halfband 2x up / down

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace lift::eng {

const char* const kFxNames[kFxTypes] = {"SPRING", "REVERB", "DELAY", "CHORUS", "PHASER",
                                        "FILTER", "DRIVE",  "LOFI",  "COMP", "GRAIN", "PULSE"};
const char* const kFxKnobNames[kFxTypes][4] = {
    {"TONE", "DECAY", "TENSION", "MIX"},  {"SIZE", "DECAY", "TONE", "MIX"},     {"TIME", "FEEDBACK", "TONE", "MIX"},
    {"RATE", "DEPTH", "FLANGE", "MIX"},   {"RATE", "DEPTH", "FEEDBACK", "MIX"}, {"CUTOFF", "RESO", "MODE", "MIX"},
    {"DRIVE", "TONE", "BIAS", "MIX"},     {"BITS", "RATE", "TONE", "MIX"},      {"AMOUNT", "ATTACK", "RELEASE", "MIX"},
    {"POS", "SIZE", "DENSITY", "MIX"},    {"RATE", "GATE", "STUTTER", "MIX"}};
const float kFxDefaults[kFxTypes][4] = {
    {0.55f, 0.4f, 0.5f, 0.25f}, {0.6f, 0.55f, 0.5f, 0.3f},  {0.45f, 0.45f, 0.5f, 0.3f},
    {0.3f, 0.45f, 0.f, 0.5f},  {0.25f, 0.6f, 0.45f, 0.5f}, {0.55f, 0.35f, 0.f, 1.f},
    {0.45f, 0.5f, 0.3f, 1.f},  {0.5f, 0.5f, 0.6f, 1.f},    {0.5f, 0.3f, 0.4f, 1.f},
    {0.15f, 0.45f, 0.55f, 0.6f}, {0.67f, 0.5f, 0.35f, 1.f}};

// Reverbs and the delay add a wet signal to the dry; the rest replace it (MIX
// crossfades dry to processed, so 100 % is the full effect).
static bool isSend(int t) noexcept { return t == FX_SPRING || t == FX_REVERB || t == FX_DELAY; }

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
    // 4-point, 3rd-order Hermite read (modulated delays: chorus, flanger)
    float herm(float d) const noexcept {
        const int i = static_cast<int>(d);
        const float f = d - static_cast<float>(i);
        const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
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

struct FxRack::SpringTank {
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
        pre.alloc(static_cast<int>(0.09 * fs));
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
        size = 0.45f + 0.55f * p[0];  // the tank's delays: plate .. hall
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
        const int pd = static_cast<int>((0.004 + 0.07 * (size - 0.45f) / 0.55f) * fs);  // plate 4 ms .. hall 74 ms
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


// ---------------------------------------------------------------- CHORUS / FLANGER

struct FxRack::Chorus {
    Delay dl, dr;
    double fs = 48000.0;
    float rate = 0.5f, depth = 0.5f, flange = 0.f;
    double ph = 0.0;
    float fbL = 0.f, fbR = 0.f;
    void prepare(double f) {
        fs = f;
        dl.alloc(static_cast<int>(0.05 * fs));
        dr.alloc(static_cast<int>(0.05 * fs));
        reset();
    }
    void reset() {
        dl.clear();
        dr.clear();
        fbL = fbR = 0.f;
    }
    void setParams(const float* p) {
        rate = static_cast<float>(0.05 * std::pow(100.0, p[0]));  // 0.05 .. 5 Hz
        depth = p[1];
        flange = p[2];
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        const double inc = rate / fs;
        const float msF = static_cast<float>(fs / 1000.0);
        // chorus: 3 taps around 14 ms, up to +-5 ms; flanger: one tap 0.4 .. 4.4 ms, feedback
        const float cBase = 14.f * msF, cDep = 5.f * depth * msF;
        const float fBase = 0.4f * msF, fDep = 4.f * depth * msF;
        const float fb = 0.82f * flange * flange;
        const float tw = static_cast<float>(2.0 * kPi);
        for (int i = 0; i < n; ++i) {
            ph += inc;
            if (ph >= 1.0) ph -= 1.0;
            const float p0 = static_cast<float>(ph);
            dl.push(flush(inL[i] + fb * fbL));
            dr.push(flush(inR[i] + fb * fbR));
            float cl = 0.f, cr = 0.f;
            for (int k = 0; k < 3; ++k) {
                const float o = p0 + static_cast<float>(k) / 3.f;
                cl += dl.herm(cBase + cDep * std::sin(tw * o));
                cr += dr.herm(cBase + cDep * std::sin(tw * (o + 0.25f)));
            }
            cl *= 0.45f;
            cr *= 0.45f;
            const float sl = 0.5f + 0.5f * std::sin(tw * p0), sr = 0.5f + 0.5f * std::sin(tw * (p0 + 0.25f));
            const float flL = dl.herm(fBase + fDep * sl), flR = dr.herm(fBase + fDep * sr);
            fbL = flL;
            fbR = flR;
            wl[i] = (1.f - flange) * cl + flange * flL;
            wr[i] = (1.f - flange) * cr + flange * flR;
        }
    }
};

// ---------------------------------------------------------------- PHASER

struct FxRack::Phaser {
    static constexpr int kStages = 8;
    double fs = 48000.0;
    float rate = 0.3f, depth = 0.6f, fb = 0.4f;
    double ph = 0.0;
    float zl[kStages] = {}, zr[kStages] = {};
    float yl = 0.f, yr = 0.f, al = 0.f, ar = 0.f;
    void prepare(double f) { fs = f; reset(); }
    void reset() {
        std::memset(zl, 0, sizeof zl);
        std::memset(zr, 0, sizeof zr);
        yl = yr = 0.f;
    }
    void setParams(const float* p) {
        rate = static_cast<float>(0.03 * std::pow(200.0, p[0]));  // 0.03 .. 6 Hz
        depth = p[1];
        fb = 0.9f * p[2];
    }
    static float coef(double hz, double fs) noexcept {
        const double t = std::tan(kPi * std::fmin(hz, 0.45 * fs) / fs);
        return static_cast<float>((t - 1.0) / (t + 1.0));
    }
    static float run(float x, float a, float* z) noexcept {
        for (int k = 0; k < kStages; ++k) {
            // first-order allpass, transposed direct form II
            const float y = a * x + z[k];
            z[k] = flush(x - a * y);
            x = y;
        }
        return x;
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        const double inc = rate / fs;
        const double lo = 120.0, oct = 1.0 + 6.0 * depth;  // sweep 120 Hz up to 7 octaves
        for (int i = 0; i < n; ++i) {
            ph += inc;
            if (ph >= 1.0) ph -= 1.0;
            if ((i & 3) == 0) {
                const double tl = 0.5 - 0.5 * std::cos(2.0 * kPi * ph);
                const double tr = 0.5 - 0.5 * std::cos(2.0 * kPi * (ph + 0.25));
                al = coef(lo * std::exp2(oct * tl), fs);
                ar = coef(lo * std::exp2(oct * tr), fs);
            }
            yl = run(inL[i] + fb * yl, al, zl);
            yr = run(inR[i] + fb * yr, ar, zr);
            // the notches come from summing with the dry (MIX 50 % = the classic phaser)
            wl[i] = yl;
            wr[i] = yr;
        }
    }
};

// ---------------------------------------------------------------- FILTER (TPT SVF)

struct FxRack::Svf {
    double fs = 48000.0;
    float cut = 0.5f, q = 0.7f, mode = 0.f, macOct = 0.f;
    float s1[2] = {}, s2[2] = {};
    void prepare(double f) { fs = f; reset(); }
    void reset() {
        s1[0] = s1[1] = s2[0] = s2[1] = 0.f;
    }
    void setParams(const float* p) {
        cut = p[0];
        q = 0.55f + 19.f * p[1] * p[1];
        mode = p[2];
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n, const float* mac) {
        const float* in[2] = {inL, inR};
        float* out[2] = {wl, wr};
        const float k = 1.f / q;
        // LP -> BP -> HP morph weights
        const float lpW = clampf(1.f - 2.f * mode, 0.f, 1.f);
        const float hpW = clampf(2.f * mode - 1.f, 0.f, 1.f);
        const float bpW = 1.f - lpW - hpW;
        float g = 0.f, a1 = 0.f, a2 = 0.f, a3 = 0.f;
        for (int i = 0; i < n; ++i) {
            if ((i & 7) == 0) {
                double hz = 20.0 * std::pow(900.0, static_cast<double>(cut));  // 20 Hz .. 18 kHz
                if (mac != nullptr) hz *= std::exp2(clampf(mac[i], -8.f, 8.f));
                hz = std::fmin(std::fmax(hz, 16.0), 0.46 * fs);
                g = static_cast<float>(std::tan(kPi * hz / fs));
                a1 = 1.f / (1.f + g * (g + k));
                a2 = g * a1;
                a3 = g * a2;
            }
            for (int c = 0; c < 2; ++c) {
                const float x = 1.2f * std::tanh(0.8f * in[c][i]);  // a little drive into the filter
                const float v3 = x - s2[c];
                const float v1 = a1 * s1[c] + a2 * v3;
                const float v2 = s2[c] + a2 * s1[c] + a3 * v3;
                s1[c] = flush(2.f * v1 - s1[c]);
                s2[c] = flush(2.f * v2 - s2[c]);
                const float lp = v2, bp = v1, hp = x - k * v1 - v2;
                out[c][i] = lpW * lp + bpW * bp * (0.5f + 0.5f * k) * 1.6f + hpW * hp;
            }
        }
    }
};

// ---------------------------------------------------------------- DRIVE (ADAA tanh)

struct FxRack::Drive {
    // 2x oversampled (jidai-common's 93-tap halfband pair) and ADAA on top:
    // the two together keep a hard-driven high note clean
    double fs = 48000.0;
    float g = 2.f, bias = 0.f, toneA = 0.5f, comp = 1.f, dcA = 0.001f;
    double xp[2] = {}, Fp[2] = {};
    float lp[2] = {}, dc[2] = {};
    jidai::dsp::Upsampler2x up[2];
    jidai::dsp::Downsampler2x down[2];
    static constexpr int kLat = 2 * jidai::dsp::Halfband93::kLatencyPerDirection;  // base samples
    float dry[2][kLat] = {};
    int dw = 0;
    void prepare(double f) {
        fs = f;
        dcA = onePoleA(12.0, fs);
        reset();
    }
    void reset() {
        xp[0] = xp[1] = Fp[0] = Fp[1] = 0.0;
        lp[0] = lp[1] = dc[0] = dc[1] = 0.f;
        for (int c = 0; c < 2; ++c) {
            up[c].reset();
            down[c].reset();
        }
        std::memset(dry, 0, sizeof dry);
        dw = 0;
    }
    void setParams(const float* p) {
        g = static_cast<float>(std::pow(40.0, p[0]));  // 1 .. 40
        toneA = onePoleA(1500.0 * std::pow(12.0, p[1]), fs);  // 1.5 .. 18 kHz
        bias = 0.6f * p[2];
        comp = 0.35f / std::tanh(0.35f * g);  // a -9 dBFS peak comes out at its own level
    }
    // f(x) = tanh(g x + b) - tanh(b); its antiderivative F(x) = log cosh(g x + b) / g - tanh(b) x
    static double logCosh(double u) noexcept {
        const double a = std::fabs(u);
        return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994531;
    }
    double shape(int c, double x, double tb) noexcept {
        const double F = logCosh(g * x + bias) / g - tb * x;
        const double dx = x - xp[c];
        const double y = std::fabs(dx) > 1e-6 ? (F - Fp[c]) / dx : std::tanh(g * 0.5 * (x + xp[c]) + bias) - tb;
        xp[c] = x;
        Fp[c] = F;
        return y;
    }
    // L / R come back as the dry delayed by the oversampler's latency, so MIX lines up
    void process(float* L, float* R, float* wl, float* wr, int n) {
        float* io[2] = {L, R};
        float* out[2] = {wl, wr};
        const double tb = std::tanh(static_cast<double>(bias));
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                const float x = io[c][i];
                double u0, u1;
                up[c].process(x, u0, u1);
                const double s0 = shape(c, u0, tb);  // in order: ADAA keeps the previous sample
                const double s1 = shape(c, u1, tb);
                const double y = down[c].process(s0, s1);
                float v = static_cast<float>(y) * comp;
                dc[c] += dcA * (v - dc[c]);  // the bias makes DC: block it
                v -= dc[c];
                lp[c] += toneA * (v - lp[c]);
                out[c][i] = flush(lp[c]);
                const float d = dry[c][dw];
                dry[c][dw] = x;
                io[c][i] = d;
            }
            dw = (dw + 1) % kLat;
        }
    }
};

// ---------------------------------------------------------------- LOFI

struct FxRack::Lofi {
    double fs = 48000.0;
    float levels = 256.f, step = 1.f, toneA = 0.5f;
    float ph = 0.f, hl = 0.f, hr = 0.f, l1[2] = {}, l2[2] = {};
    void prepare(double f) { fs = f; reset(); }
    void reset() {
        ph = hl = hr = 0.f;
        l1[0] = l1[1] = l2[0] = l2[1] = 0.f;
    }
    void setParams(const float* p) {
        const float bits = 16.f - 13.f * p[0];  // 16 .. 3 bits
        levels = std::exp2(bits - 1.f);
        const double rateHz = fs * std::pow(1500.0 / fs, static_cast<double>(p[1]));  // fs .. 1.5 kHz
        step = static_cast<float>(rateHz / fs);
        toneA = onePoleA(800.0 * std::pow(25.0, p[2]), fs);  // 0.8 .. 20 kHz
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        for (int i = 0; i < n; ++i) {
            ph += step;
            if (ph >= 1.f) {  // sample and hold at the reduced rate
                ph -= 1.f;
                hl = std::round(clampf(inL[i], -1.f, 1.f) * levels) / levels;
                hr = std::round(clampf(inR[i], -1.f, 1.f) * levels) / levels;
            }
            // two one-poles smooth the steps (the converter's reconstruction filter)
            l1[0] += toneA * (hl - l1[0]);
            l2[0] += toneA * (l1[0] - l2[0]);
            l1[1] += toneA * (hr - l1[1]);
            l2[1] += toneA * (l1[1] - l2[1]);
            wl[i] = l2[0];
            wr[i] = l2[1];
        }
    }
};

// ---------------------------------------------------------------- COMP

struct FxRack::Comp {
    double fs = 48000.0;
    float thr = -18.f, ratio = 4.f, knee = 6.f, atk = 0.01f, rel = 0.001f, makeup = 1.f;
    float env = 0.f;  // gain reduction, dB (>= 0)
    float gr = 0.f;   // for tests / the screen
    void prepare(double f) { fs = f; reset(); }
    void reset() { env = 0.f; }
    void setParams(const float* p) {
        thr = -4.f - 32.f * p[0];  // -4 .. -36 dBFS
        ratio = 1.5f + 8.5f * p[0];
        const double aMs = 0.1 * std::pow(300.0, p[1]);  // 0.1 .. 30 ms
        const double rMs = 30.0 * std::pow(25.0, p[2]);  // 30 .. 750 ms
        atk = static_cast<float>(1.0 - std::exp(-1.0 / (aMs * 0.001 * fs)));
        rel = static_cast<float>(1.0 - std::exp(-1.0 / (rMs * 0.001 * fs)));
        // makeup: half the reduction a -6 dBFS peak would get
        const float over = -6.f - thr;
        makeup = std::pow(10.f, 0.5f * std::fmax(0.f, over * (1.f - 1.f / ratio)) / 20.f);
    }
    float curve(float db) const noexcept {  // gain reduction (dB) for a level, soft knee
        const float o = db - thr;
        if (2.f * o < -knee) return 0.f;
        if (2.f * std::fabs(o) <= knee) {
            const float t = o + 0.5f * knee;
            return (1.f - 1.f / ratio) * t * t / (2.f * knee);
        }
        return (1.f - 1.f / ratio) * o;
    }
    void process(const float* inL, const float* inR, float* wl, float* wr, int n) {
        for (int i = 0; i < n; ++i) {
            const float pk = std::fmax(std::fabs(inL[i]), std::fabs(inR[i]));  // stereo linked
            const float db = 20.f * std::log10(pk + 1e-9f);
            const float want = curve(db);
            env += (want > env ? atk : rel) * (want - env);
            const float gain = std::pow(10.f, -env / 20.f) * makeup;
            wl[i] = inL[i] * gain;
            wr[i] = inR[i] * gain;
        }
        gr = env;
    }
};

// ---------------------------------------------------------------- rack

// PULSE: a gate on the tempo grid; at each step it may repeat the previous
// step's audio from its 2 s buffer instead of passing the live input.
struct FxRack::Stutter {
    double fs = 48000.0;
    std::vector<float> l, r;
    int cap = 0, w = 0;
    double phase = 0.0;      // frames into the current step
    int stepStart = 0;       // ring index where the current step began
    int prevStart = 0, prevLen = 0;
    bool repeat = false;
    int readPos = 0;
    float env = 0.f;
    std::uint32_t rng = 12345u;
    float rate = 0.67f, gate = 0.5f, stutter = 0.35f;
    void prepare(double f) {
        fs = f;
        cap = static_cast<int>(2.0 * fs);
        l.assign(static_cast<size_t>(cap), 0.f);
        r.assign(static_cast<size_t>(cap), 0.f);
        reset();
    }
    void reset() noexcept {
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
        w = 0;
        phase = 0.0;
        stepStart = prevStart = prevLen = 0;
        repeat = false;
        env = 0.f;
    }
    void setParams(const float* p) noexcept {
        rate = p[0];
        gate = p[1];
        stutter = p[2];
    }
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, double bpm) noexcept {
        static const int kDivs[4] = {4, 8, 16, 32};
        const int div = kDivs[std::min(3, static_cast<int>(rate * 4.f))];
        const double stepLen = std::min(60.0 / bpm * 4.0 / div * fs, static_cast<double>(cap / 2));
        const float duty = 0.08f + 0.92f * gate;
        const float a = 1.f - std::exp(-1.f / (0.002f * static_cast<float>(fs)));  // 2 ms edges
        for (int i = 0; i < n; ++i) {
            l[static_cast<size_t>(w)] = inL[i];
            r[static_cast<size_t>(w)] = inR[i];
            if (phase >= stepLen) {
                phase -= stepLen;
                prevStart = stepStart;
                prevLen = static_cast<int>(stepLen);
                stepStart = w;
                rng = rng * 1664525u + 1013904223u;
                repeat = static_cast<float>(rng >> 8) / 16777216.f < stutter;
                readPos = prevStart;
            }
            float xl = inL[i], xr = inR[i];
            if (repeat) {
                xl = l[static_cast<size_t>(readPos)];
                xr = r[static_cast<size_t>(readPos)];
                if (++readPos >= cap) readPos = 0;
            }
            const float target = phase < duty * stepLen ? 1.f : 0.f;
            env += a * (target - env);
            outL[i] = xl * env;
            outR[i] = xr * env;
            phase += 1.0;
            if (++w >= cap) w = 0;
        }
    }
};

FxRack::FxRack()
    : spring_(std::make_unique<SpringTank>()), plate_(std::make_unique<Plate>()), echo_(std::make_unique<Echo>()),
      chorus_(std::make_unique<Chorus>()), phaser_(std::make_unique<Phaser>()), filter_(std::make_unique<Svf>()),
      drive_(std::make_unique<Drive>()), lofi_(std::make_unique<Lofi>()), comp_(std::make_unique<Comp>()),
      pulse_(std::make_unique<Stutter>()) {}
FxRack::~FxRack() = default;

void FxRack::prepare(double fs) {
    fs_ = fs;
    pulse_->prepare(fs);
    grain_.prepare(fs);
    spring_->prepare(fs);
    plate_->prepare(fs);
    echo_->prepare(fs);
    chorus_->prepare(fs);
    phaser_->prepare(fs);
    filter_->prepare(fs);
    drive_->prepare(fs);
    lofi_->prepare(fs);
    comp_->prepare(fs);
    setParams(p_);
}

void FxRack::reset() noexcept {
    spring_->reset();
    plate_->reset();
    echo_->reset();
    chorus_->reset();
    phaser_->reset();
    filter_->reset();
    drive_->reset();
    lofi_->reset();
    comp_->reset();
    if (pulse_) pulse_->reset();
    grain_.stopGrains();  // the GRAIN buffer itself is not reset: it keeps rolling whatever effect is selected
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
    case FX_REVERB: plate_->setParams(p_); break;
    case FX_DELAY: echo_->setParams(p_); break;
    case FX_CHORUS: chorus_->setParams(p_); break;
    case FX_PHASER: phaser_->setParams(p_); break;
    case FX_FILTER: filter_->setParams(p_); break;
    case FX_DRIVE: drive_->setParams(p_); break;
    case FX_LOFI: lofi_->setParams(p_); break;
    case FX_COMP: comp_->setParams(p_); break;
    case FX_PULSE: if (pulse_) pulse_->setParams(p_); break;
    default: break;  // GRAIN reads p_ directly
    }
}

float FxRack::shake() const noexcept { return spring_->shake; }

void FxRack::process(float* L, float* R, int n, const float* mac) noexcept {
    if (n > 64) {
        n = 64;
    }
    // the GRAIN buffer rolls all the time (8 s of the bus, whatever is selected)
    grain_.write(L, R, n);
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
    case FX_REVERB: plate_->process(inL, inR, wl, wr, n); break;
    case FX_DELAY: echo_->process(inL, inR, wl, wr, n); break;
    case FX_CHORUS: chorus_->process(inL, inR, wl, wr, n); break;
    case FX_PHASER: phaser_->process(inL, inR, wl, wr, n); break;
    case FX_FILTER: filter_->process(L, R, wl, wr, n, mac); break;
    case FX_DRIVE: drive_->process(L, R, wl, wr, n); break;
    case FX_LOFI: lofi_->process(L, R, wl, wr, n); break;
    case FX_COMP: comp_->process(L, R, wl, wr, n); break;
    case FX_PULSE: pulse_->process(L, R, wl, wr, n, bpm_); break;
    default: {  // GRAIN
        for (int i = 0; i < n; ++i) wl[i] = wr[i] = 0.f;
        GrainCloud::Params gp;
        gp.pos = clampf(p_[0] + gPosV_ / 5.f, 0.f, 1.f);
        gp.size = clampf(p_[1] + gSizeV_ / 5.f, 0.f, 1.f);
        gp.density = p_[2];
        gp.spray = 0.35f;
        grain_.render(wl, wr, n, gp, 1.f);
        break;
    }
    }
    const float mix = clampf(p_[3] + (type_ == FX_FILTER ? 0.f : macMean / 5.f), 0.f, 1.f);
    float w = wet_;
    if (isSend(type_)) {
        for (int i = 0; i < n; ++i) {
            w += w < target ? ramp : -ramp;
            w = clampf(w, 0.f, 1.f);
            const float dryG = 1.f - 0.5f * mix * w;
            L[i] = L[i] * dryG + wl[i] * mix * w * 1.4f;
            R[i] = R[i] * dryG + wr[i] * mix * w * 1.4f;
        }
    } else {
        // inserts: crossfade dry -> processed (their input is not faded, so no tail to ring out)
        for (int i = 0; i < n; ++i) {
            w += w < target ? ramp : -ramp;
            w = clampf(w, 0.f, 1.f);
            const float m = mix * w;
            L[i] = L[i] * (1.f - m) + wl[i] * m;
            R[i] = R[i] * (1.f - m) + wr[i] * m;
        }
    }
    wet_ = w;
}

}  // namespace lift::eng
