// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT's insert effects (the FX pad): nine classic effects, LIFT's own DSP,
// written from the textbook structures named below; no third-party code.
// Framework free, real time safe after prepare(). Knob 4 is MIX on every one.
//
//   SPRING  digital spring reverb (its own effect, separate from REVERB): three
//           springs, each a feedback loop with a cascade of stretched first-order
//           allpasses (the low dispersive chirp), a short plain allpass loop (the
//           bright high chirp), a pre-echo tap (the drip), and a transient
//           detector that shakes the tank (the boing). After the parametric
//           spring structure of Valimaki, Parker and Abel (JAES 2010).
//           TONE, DECAY, TENSION, MIX.
//   REVERB  plate (small SIZE) to hall (large SIZE): figure-of-eight tank after
//           Dattorro (JAES 1997), predelay grows with SIZE. SIZE, DECAY, TONE, MIX.
//   DELAY   tape-style stereo delay: darkening, saturating feedback, wow, glide.
//           TIME, FEEDBACK, TONE, MIX.
//   CHORUS  chorus to flanger: three modulated taps (chorus) morphing to one
//           short tap with feedback (flanger), Hermite interpolation.
//           RATE, DEPTH, FLANGE, MIX.
//   PHASER  eight first-order allpass stages per side, swept exponentially,
//           quadrature stereo LFO, feedback. RATE, DEPTH, FEEDBACK, MIX.
//   FILTER  zero-delay-feedback state variable filter (trapezoidal / TPT), LP to
//           BP to HP morph, mild drive. FX MAC sweeps the cutoff.
//           CUTOFF, RESO, MODE, MIX.
//   DRIVE   saturation: asymmetric tanh with first-order antiderivative
//           antialiasing (ADAA), tone tilt after. DRIVE, TONE, BIAS, MIX.
//   LOFI    bit depth and sample-rate reduction with a smoothing filter after.
//           BITS, RATE, TONE, MIX.
//   COMP    stereo-linked feed-forward compressor, soft knee, log-domain
//           attack / release, auto makeup (MIX = parallel compression).
//           AMOUNT, ATTACK, RELEASE, MIX.

#include <cstdint>
#include <memory>

namespace lift::eng {

enum FxType : int { FX_SPRING, FX_REVERB, FX_DELAY, FX_CHORUS, FX_PHASER, FX_FILTER, FX_DRIVE, FX_LOFI, FX_COMP, kFxTypes };
extern const char* const kFxNames[kFxTypes];
extern const char* const kFxKnobNames[kFxTypes][4];
extern const float kFxDefaults[kFxTypes][4];

class FxRack {
public:
    FxRack();
    ~FxRack();
    FxRack(const FxRack&) = delete;
    FxRack& operator=(const FxRack&) = delete;

    void prepare(double fs);  // allocates
    void reset() noexcept;
    void setType(int t) noexcept;
    int type() const noexcept { return type_; }
    void setParams(const float p[4]) noexcept;  // 0..1, smoothed by the caller
    void setBypass(bool b) noexcept { bypass_ = b; }
    // In place, stereo. mac: FX MAC volts per sample (null = unpatched): adds
    // to MIX (+5 V = +100 %); on SPRING a rising edge also kicks the tank; on
    // FILTER it sweeps the cutoff instead (1 V = 1 octave).
    void process(float* L, float* R, int n, const float* mac) noexcept;
    float shake() const noexcept;  // spring tank motion, for tests / the screen

    struct SpringTank;
    struct Plate;
    struct Echo;
    struct Chorus;
    struct Phaser;
    struct Svf;
    struct Drive;
    struct Lofi;
    struct Comp;

private:
    double fs_ = 48000.0;
    int type_ = FX_SPRING;
    bool bypass_ = false;
    float p_[4] = {0.5f, 0.5f, 0.5f, 0.35f};
    float wet_ = 0.f;      // bypass crossfade, 0..1
    float macPrev_ = 0.f;
    std::unique_ptr<SpringTank> spring_;
    std::unique_ptr<Plate> plate_;
    std::unique_ptr<Echo> echo_;
    std::unique_ptr<Chorus> chorus_;
    std::unique_ptr<Phaser> phaser_;
    std::unique_ptr<Svf> filter_;
    std::unique_ptr<Drive> drive_;
    std::unique_ptr<Lofi> lofi_;
    std::unique_ptr<Comp> comp_;
};

}  // namespace lift::eng
