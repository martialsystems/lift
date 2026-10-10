// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "tape/engine.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

// Audio thread. No allocation, no disk, no socket.

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr float kPiF = 3.14159265358979323846f;
constexpr float kRefIps = 15.f;
constexpr float kEmphHz = 3150.f;
constexpr int kCoeffEvery = 8;
constexpr float kMaxWowSamples = 1000.f;
constexpr float kMaxFlutterSamples = 400.f;

float clampf(float x, float lo, float hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

void biquad_unity(TapeBiquad& q) noexcept {
    q.b0 = 1.0;
    q.b1 = 0.0;
    q.b2 = 0.0;
    q.a1 = 0.0;
    q.a2 = 0.0;
}

void biquad_clear(TapeBiquad& q) noexcept {
    for (int c = 0; c < 2; ++c) {
        q.z1[c] = 0.0;
        q.z2[c] = 0.0;
    }
}

void biquad_set(TapeBiquad& q, double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
    q.b0 = b0 / a0;
    q.b1 = b1 / a0;
    q.b2 = b2 / a0;
    q.a1 = a1 / a0;
    q.a2 = a2 / a0;
}

void svf_set(TapeSvf& f, double fs, double f0, double k) noexcept {
    const double g = tan(kTwoPi * 0.5 * f0 / fs);
    f.k = k;
    f.a1 = 1.0 / (1.0 + g * (g + k));
    f.a2 = g * f.a1;
    f.a3 = g * f.a2;
}

void design_lowpass(TapeSvf& f, double fs, double f0, double Q) noexcept {
    svf_set(f, fs, f0, 1.0 / Q);
    f.bellGain = 0.0;
}

void design_peak(TapeSvf& f, double fs, double f0, double Q, double gainDb) noexcept {
    const double A = pow(10.0, gainDb / 40.0);
    svf_set(f, fs, f0, 1.0 / (Q * A));
    f.bellGain = f.k * (A * A - 1.0);
}

void svf_clear(TapeSvf& f) noexcept {
    for (int c = 0; c < 2; ++c) {
        f.ic1[c] = 0.0;
        f.ic2[c] = 0.0;
    }
}

// Low pass output, or the input plus the boosted band for a bell.
float svf_run(TapeSvf& f, int c, float in, bool bell) noexcept {
    const double v0 = static_cast<double>(in);
    const double v3 = v0 - f.ic2[c];
    const double v1 = f.a1 * f.ic1[c] + f.a2 * v3;
    const double v2 = f.ic2[c] + f.a2 * f.ic1[c] + f.a3 * v3;
    f.ic1[c] = 2.0 * v1 - f.ic1[c];
    f.ic2[c] = 2.0 * v2 - f.ic2[c];
    if (fabs(f.ic1[c]) < 1e-30) {
        f.ic1[c] = 0.0;
    }
    if (fabs(f.ic2[c]) < 1e-30) {
        f.ic2[c] = 0.0;
    }
    return static_cast<float>(bell ? v0 + f.bellGain * v1 : v2);
}

void design_high_shelf(TapeBiquad& q, double fs, double f0, double gainDb) noexcept {
    if (gainDb == 0.0) {
        biquad_unity(q);
        return;
    }
    const double A = pow(10.0, gainDb / 40.0);
    const double w0 = kTwoPi * f0 / fs;
    const double cw = cos(w0);
    const double alpha = sin(w0) / 2.0 * sqrt(2.0);
    const double two = 2.0 * sqrt(A) * alpha;
    biquad_set(q, A * ((A + 1.0) + (A - 1.0) * cw + two), -2.0 * A * ((A - 1.0) + (A + 1.0) * cw),
               A * ((A + 1.0) + (A - 1.0) * cw - two), (A + 1.0) - (A - 1.0) * cw + two,
               2.0 * ((A - 1.0) - (A + 1.0) * cw), (A + 1.0) - (A - 1.0) * cw - two);
}

float biquad_run(TapeBiquad& q, int c, float in) noexcept {
    const double x = static_cast<double>(in);
    const double y = q.b0 * x + q.z1[c];
    q.z1[c] = q.b1 * x - q.a1 * y + q.z2[c];
    q.z2[c] = q.b2 * x - q.a2 * y;
    return static_cast<float>(y);
}

float white(uint32_t& rng) noexcept {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(rng)) / 2147483648.f;
}

double fs_of(const TapeEngine& e) noexcept {
    return e.sampleRate > 0.0 ? e.sampleRate : static_cast<double>(kSampleRate);
}

void design_record(TapeEngine& e) noexcept {
    const double fs = fs_of(e);
    design_high_shelf(e.preEmph, fs, kEmphHz, e.p.preEmphDb);
    design_high_shelf(e.deEmph, fs, kEmphHz, -e.p.preEmphDb);
    // Bias sets the tape's HF sensitivity on record: over-biasing erases the
    // short wavelengths as they are written (dull), under-biasing leaves them
    // hot (bright and gritty). Flat at the nominal bias (0.5).
    design_high_shelf(e.biasEq, fs, 4500.0, 16.f * (0.5f - e.p.bias));
}

// Head bump and gap loss move with the speed of tape past the head.
void design_playback(TapeEngine& e, float speed) noexcept {
    const double fs = fs_of(e);
    const float s = clampf(e.p.speedIps * speed / kRefIps, 0.001f, 8.f);
    const float biasHf = 1.3f - 0.6f * clampf(e.p.bias, 0.f, 1.f);
    const double bumpHz = clampf(e.p.bumpHz * s, 10.f, static_cast<float>(fs * 0.2));
    design_peak(e.bump, fs, bumpHz, e.p.bumpQ > 0.1f ? e.p.bumpQ : 0.1f, e.p.bumpDb);
    const double gapHz = clampf(e.p.lowpassHz * s * biasHf, 40.f, static_cast<float>(fs * 0.45));
    const double gapHz2 = clampf(static_cast<float>(gapHz * 1.6), 60.f, static_cast<float>(fs * 0.47));
    design_lowpass(e.gapA, fs, gapHz, 0.66);
    design_lowpass(e.gapB, fs, gapHz2, 0.55);
    e.coeffSpeed = speed;
    e.dirty = false;
}

// Rate independent hysteresis: a play operator (the magnetic domain lag that
// bias works against) blended with the input, into a soft saturation. Under
// bias widens the loop, which gives crossover grit; asym adds even harmonics.
inline float hysteresis(float x, float& play, float drive, float makeup, float width, float asym) noexcept {
    const float h = x * drive;
    const float lo = h - width;
    const float hi = h + width;
    play = play < lo ? lo : (play > hi ? hi : play);
    const float m = tanhf(0.5f * h + 0.5f * play);
    return (m + asym * m * m) * makeup;
}

float hermite(const float* buf, int write, float delaySamples) noexcept {
    const float rd = static_cast<float>(write) - delaySamples;
    const float fl = floorf(rd);
    const float t = rd - fl;
    const int i = static_cast<int>(fl);
    const float xm1 = buf[(i - 1) & kTapeDelayMask];
    const float x0 = buf[i & kTapeDelayMask];
    const float x1 = buf[(i + 1) & kTapeDelayMask];
    const float x2 = buf[(i + 2) & kTapeDelayMask];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

}  // namespace

// Flush to zero and denormals as zero, x86 (MXCSR) and arm64 (FPCR).
TapeNoDenormals::TapeNoDenormals() noexcept : saved(0) {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    saved = static_cast<intptr_t>(_mm_getcsr());
    _mm_setcsr(static_cast<unsigned int>(saved) | 0x8040u);
#elif defined(__aarch64__)
    uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    saved = static_cast<intptr_t>(fpcr);
    fpcr |= (1ull << 24);
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

TapeNoDenormals::~TapeNoDenormals() noexcept {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr(static_cast<unsigned int>(saved));
#elif defined(__aarch64__)
    const uint64_t fpcr = static_cast<uint64_t>(saved);
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

void tape_engine_init(TapeEngine& e) noexcept {
    e.sampleRate = static_cast<double>(kSampleRate);
    e.os = nullptr;
    e.latency = 0;
    tape_engine_reset(e);
    tape_engine_set_character(e, character_row(0));
}

void tape_engine_set_character(TapeEngine& e, const Character& row) noexcept {
    TapeParams p;
    p.speedIps = kRefIps;
    p.drive = row.drive;
    p.bias = 0.5f;
    p.asym = row.asym;
    p.preEmphDb = row.preEmph * 40.f;
    p.bumpHz = row.bumpHz;
    p.bumpDb = row.bumpDb;
    p.bumpQ = row.bumpQ;
    p.lowpassHz = row.lowpassHz;
    p.wowHz = row.wowHz;
    p.wowDepth = row.wowDepth;
    p.flutterHz = row.flutterHz;
    p.flutterDepth = row.flutterDepth;
    p.hissDb = row.hissDb;
    p.speedGlide = 0.05f;
    tape_engine_set_params(e, p);
}

void tape_engine_set_params(TapeEngine& e, const TapeParams& p) noexcept {
    e.p = p;
    e.p.speedIps = clampf(p.speedIps, 1.875f, 30.f);
    e.p.drive = clampf(p.drive, 0.25f, 16.f);
    e.p.bias = clampf(p.bias, 0.f, 1.f);
    e.p.wowDepth = clampf(p.wowDepth, 0.f, 0.02f);
    e.p.flutterDepth = clampf(p.flutterDepth, 0.f, 0.01f);
    e.p.wowHz = clampf(p.wowHz, 0.05f, 4.f);
    e.p.flutterHz = clampf(p.flutterHz, 2.f, 40.f);
    e.p.speedGlide = clampf(p.speedGlide, 0.001f, 2.f);
    design_record(e);
    design_playback(e, e.speedNow > 0.f ? e.speedNow * e.ramp : 1.f);
    e.dirty = true;
}

void tape_engine_reset(TapeEngine& e) noexcept {
    biquad_clear(e.preEmph);
    biquad_clear(e.biasEq);
    biquad_clear(e.deEmph);
    svf_clear(e.bump);
    svf_clear(e.gapA);
    svf_clear(e.gapB);
    for (int c = 0; c < 2; ++c) {
        e.hystPlay[c] = 0.f;
        e.dcX[c] = 0.f;
        e.dcY[c] = 0.f;
        e.hissLp[c] = 0.f;
        for (int i = 0; i < kTapeDelaySize; ++i) {
            e.delay[c][i] = 0.f;
        }
    }
    e.delayWrite = 0;
    e.wowPhase = 0.0;
    e.flutterPhase = 0.0;
    e.flutterAmp = 0.f;
    e.flutterAmp2 = 0.f;
    e.rng = 0x1234567u;
    e.speedNow = 1.f;
    e.ramp = 1.f;
    e.rampInc = 0.f;
    e.stopping = false;
    e.coeffCountdown = 0;
    e.dirty = true;
    if (e.os != nullptr) {
        e.os->reset();
    }
}

int tape_engine_latency(const TapeEngine& e) noexcept {
    return e.latency;
}

void tape_engine_record(TapeEngine& e, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept {
    if (outL == nullptr || outR == nullptr || n <= 0) {
        return;
    }
    TapeNoDenormals guard;
    // Hotter prints come back louder and more saturated, as on a real deck,
    // but only by the square root of drive so the level stays usable.
    const float drive = e.p.drive;
    const float makeup = 1.f / sqrtf(drive);
    // Loop width: near zero at nominal bias, wide (crossover grit) when under-biased.
    const float under = 1.f - e.p.bias;
    const float width = 0.08f * under * under * under * under * under;
    const float asym = e.p.asym * (1.f + 2.f * clampf(0.5f - e.p.bias, 0.f, 0.5f));
    const float dcR = 1.f - static_cast<float>(kTwoPi * 5.0 / fs_of(e));
    float* out[2] = {outL, outR};
    const float* in[2] = {inL, inR};
    for (int start = 0; start < n; start += kTapeMaxBlock) {
        const int m = n - start < kTapeMaxBlock ? n - start : kTapeMaxBlock;
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < m; ++i) {
                const float x = in[c] != nullptr ? in[c][start + i] : 0.f;
                out[c][start + i] = biquad_run(e.preEmph, c, x);
            }
        }
        float* chans[2] = {outL + start, outR + start};
        juce::dsp::AudioBlock<float> block(chans, 2, static_cast<size_t>(m));
        if (e.os != nullptr) {
            juce::dsp::AudioBlock<float> up = e.os->processSamplesUp(block);
            const int un = static_cast<int>(up.getNumSamples());
            for (int c = 0; c < 2; ++c) {
                float* d = up.getChannelPointer(static_cast<size_t>(c));
                for (int i = 0; i < un; ++i) {
                    d[i] = hysteresis(d[i], e.hystPlay[c], drive, makeup, width, asym);
                }
            }
            e.os->processSamplesDown(block);
        } else {
            for (int c = 0; c < 2; ++c) {
                for (int i = 0; i < m; ++i) {
                    chans[c][i] = hysteresis(chans[c][i], e.hystPlay[c], drive, makeup, width, asym);
                }
            }
        }
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < m; ++i) {
                const float y = biquad_run(e.biasEq, c, biquad_run(e.deEmph, c, chans[c][i]));
                const float d = y - e.dcX[c] + dcR * e.dcY[c];
                e.dcX[c] = y;
                e.dcY[c] = d;
                chans[c][i] = d;
            }
        }
    }
}

float tape_engine_next_speed(TapeEngine& e, float varispeed) noexcept {
    const float a = 1.f - expf(-1.f / (e.p.speedGlide * static_cast<float>(fs_of(e))));
    e.speedNow += a * (varispeed - e.speedNow);
    if (e.rampInc != 0.f) {
        e.ramp += e.rampInc;
        if (e.ramp <= 0.f) {
            e.ramp = 0.f;
            e.rampInc = 0.f;
        } else if (e.ramp >= 1.f) {
            e.ramp = 1.f;
            e.rampInc = 0.f;
        }
    }
    return e.speedNow * e.ramp;
}

void tape_engine_play(TapeEngine& e, float& l, float& r) noexcept {
    const double fs = fs_of(e);
    const float speed = e.speedNow * e.ramp;
    if (--e.coeffCountdown <= 0) {
        e.coeffCountdown = kCoeffEvery;
        if (e.dirty || fabsf(speed - e.coeffSpeed) > 1e-3f * (e.coeffSpeed > 0.01f ? e.coeffSpeed : 0.01f)) {
            design_playback(e, speed);
        }
    }
    float x[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
        float y = svf_run(e.bump, c, x[c], true);
        y = svf_run(e.gapA, c, y, false);
        y = svf_run(e.gapB, c, y, false);
        e.delay[c][e.delayWrite] = y;
    }
    // Wow and flutter: a fractional delay whose slope is the pitch error.
    // Peak deviation = A * w / fs, so A = depth * fs / w.
    const float aw = clampf(static_cast<float>(e.p.wowDepth * fs / (kTwoPi * e.p.wowHz)), 0.f, kMaxWowSamples);
    const float af =
        clampf(static_cast<float>(e.p.flutterDepth * fs / (kTwoPi * e.p.flutterHz)), 0.f, kMaxFlutterSamples);
    // Flutter amount drifts between 60 % and 100 % (two pole smoothed noise,
    // so the drift itself adds no pitch jitter).
    const float fa = static_cast<float>(kTwoPi * 1.5 / fs);
    e.flutterAmp += fa * (white(e.rng) - e.flutterAmp);
    e.flutterAmp2 += fa * (e.flutterAmp - e.flutterAmp2);
    const float famp = 0.8f + 0.2f * tanhf(60.f * e.flutterAmp2);
    const float d = aw + af + 3.f + aw * sinf(static_cast<float>(e.wowPhase)) +
                    af * famp * sinf(static_cast<float>(e.flutterPhase));
    e.wowPhase += kTwoPi * e.p.wowHz / fs;
    if (e.wowPhase > kTwoPi) {
        e.wowPhase -= kTwoPi;
    }
    e.flutterPhase += kTwoPi * e.p.flutterHz / fs;
    if (e.flutterPhase > kTwoPi) {
        e.flutterPhase -= kTwoPi;
    }
    // Hiss: white through a one pole near 9 kHz, normalised to the RMS target.
    const float ha = 1.f - expf(-2.f * kPiF * 9000.f / static_cast<float>(fs));
    const float hissRms = e.p.hissDb <= -119.f ? 0.f : powf(10.f, e.p.hissDb / 20.f);
    const float hissGain = hissRms * sqrtf(3.f * (2.f - ha) / ha);
    // The repro head output follows tape motion, so a stop ramps to silence.
    const float g = e.ramp * e.ramp;
    float outv[2];
    for (int c = 0; c < 2; ++c) {
        const float wet = hermite(e.delay[c], e.delayWrite, d);
        e.hissLp[c] += ha * (white(e.rng) - e.hissLp[c]);
        outv[c] = (wet + e.hissLp[c] * hissGain) * g;
    }
    e.delayWrite = (e.delayWrite + 1) & kTapeDelayMask;
    l = outv[0];
    r = outv[1];
}

void tape_engine_stop(TapeEngine& e, float seconds) noexcept {
    e.stopping = true;
    if (seconds <= 0.f) {
        e.ramp = 0.f;
        e.rampInc = 0.f;
        return;
    }
    e.rampInc = -1.f / (seconds * static_cast<float>(fs_of(e)));
}

void tape_engine_start(TapeEngine& e, float seconds) noexcept {
    e.stopping = false;
    if (seconds <= 0.f) {
        e.ramp = 1.f;
        e.rampInc = 0.f;
        return;
    }
    e.rampInc = 1.f / (seconds * static_cast<float>(fs_of(e)));
}

bool tape_engine_halted(const TapeEngine& e) noexcept {
    return e.stopping && e.ramp <= 0.f;
}
