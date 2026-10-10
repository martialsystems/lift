// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Exact-halfband 2x up/down pair, 93 taps (SHOGUN_Redesign.md 3.4 stage 1). Header-only C++17, no deps.
//
//   Edges at the 2x rate: pass 0.2125, stop 0.2875 (pass <= 20.4 kHz, stop >= 27.6 kHz at fs = 48 kHz).
//   Designed equiripple with the halfband constraint (scipy remez on the 46-tap odd branch, interleaved, centre 0.5):
//   stopband 111.6 dB, passband ripple 4.5e-5 dB. Taps at even offsets from the centre are exactly 0, and so are
//   the two end taps, so each direction costs 23 MACs per base sample.
//   Latency: (93-1)/2 = 46 samples at 2fs = 23 base samples PER DIRECTION.
//   An up -> process -> down chain (an effect on external audio) is 46 base samples.
//
//   Upsampler2x::process (x, y0, y1)   one base sample in, two 2x samples out (y0 = x delayed 23 exactly)
//   Downsampler2x::process (u0, u1)    two 2x samples in, one base sample out
// States are double; reset() clears them.

namespace jidai::dsp {

struct Halfband93
{
    static constexpr int kTaps = 93;
    static constexpr int kSide = 23;                // nonzero taps per side (odd offsets 1, 3, ..., 45)
    static constexpr int kLatencyPerDirection = 23; // base samples
    // h[46 +- (2j+1)] = odd[j]; h[46] = 0.5; every other tap is 0.
    static const double* odd() noexcept
    {
        static const double t[kSide] = {
        0.31756000191720085,
        -0.10387284801445018,
        0.060007585563567861,
        -0.040485631710322073,
        0.029168777086991157,
        -0.021671480782880455,
        0.016314828794272491,
        -0.012317055659975533,
        0.0092615035202843372,
        -0.006901073063122997,
        0.0050750910690782556,
        -0.0036702901842250533,
        0.0026012661241049916,
        -0.0018002885204330306,
        0.0012118491643317252,
        -0.00078973722576147743,
        0.00049537888144551097,
        -0.00029685449968259694,
        0.0001681804345037971,
        -8.8700658838796237e-05,
        4.2470806935745889e-05,
        -1.7614233690896581e-05,
        5.9364683306040488e-06
        };
        return t;
    }
};

class Upsampler2x
{
public:
    void reset() noexcept
    {
        for (double& v : hist_) v = 0.0;
        pos_ = 0;
    }
    // y0 is the 2fs sample at even phase (exactly x[n-23]); y1 is the interpolated odd phase.
    void process (double x, double& y0, double& y1) noexcept
    {
        pos_ = (pos_ + kLen - 1) % kLen;
        hist_[pos_] = x;
        hist_[pos_ + kLen] = x;                    // mirrored, so taps read contiguously
        const double* h = hist_ + pos_;            // h[k] = x[n-k], k = 0..45
        const double* a = Halfband93::odd();
        double acc = 0.0;
        for (int j = 0; j < Halfband93::kSide; ++j)
            acc += a[j] * (h[23 + j] + h[22 - j]);
        y0 = h[23];
        y1 = 2.0 * acc;
    }

private:
    static constexpr int kLen = 46;
    double hist_[2 * kLen] {};
    int pos_ = 0;
};

class Downsampler2x
{
public:
    void reset() noexcept
    {
        for (double& v : hist_) v = 0.0;
        pos_ = 0;
    }
    // u0 = u[2n], u1 = u[2n+1]. Output y[n] = sum h[k] u[2n-k] (centre on u[2n-46]).
    double process (double u0, double u1) noexcept
    {
        push (u0);
        const double* h = hist_ + pos_;            // h[k] = u[2n-k], k = 0..91
        const double* a = Halfband93::odd();
        double acc = 0.5 * h[46];
        for (int j = 0; j < Halfband93::kSide; ++j)
            acc += a[j] * (h[46 - (2 * j + 1)] + h[46 + (2 * j + 1)]);
        push (u1);
        return acc;
    }

private:
    static constexpr int kLen = 92;
    void push (double v) noexcept
    {
        pos_ = (pos_ + kLen - 1) % kLen;
        hist_[pos_] = v;
        hist_[pos_ + kLen] = v;
    }
    double hist_[2 * kLen] {};
    int pos_ = 0;
};

} // namespace jidai::dsp
