// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT's insert effects (the FX pad). LIFT's own DSP, written from the
// textbook structures named below; no third-party code. Framework free, real
// time safe after prepare().
//
// On screen the three are DRIP, SPACE and ECHO (the approved effect names).
//
//   DRIP    the spring reverb, a digital spring tank: three springs, each a feedback loop with a
//           cascade of stretched first-order allpasses (the low "chirp"), a
//           short plain allpass loop (the bright high chirp), a pre-echo tap
//           (the drip), and a transient detector that shakes the tank (the
//           boing). After the parametric spring structure of Valimaki, Parker
//           and Abel (JAES 2010) and Parker's dispersion filters (2011).
//           Knobs: TONE, DECAY, TENSION, MIX.
//   SPACE   plate / hall reverb (separate from the spring): figure-of-eight tank after Dattorro (JAES 1997).
//           Knobs: SIZE, DECAY, TONE, MIX.
//   ECHO    tape echo: one head, darkening, saturating feedback, wow.
//           Knobs: TIME, FEEDBACK, TONE, MIX.

#include <cstdint>
#include <memory>

namespace lift::eng {

enum FxType : int { FX_SPRING, FX_PLATE, FX_ECHO, kFxTypes };
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
    // to MIX (+5 V = +100 %); on SPRING a rising edge also kicks the tank.
    void process(float* L, float* R, int n, const float* mac) noexcept;
    float shake() const noexcept;  // spring tank motion, for tests / the screen

    struct SpringTank;
    struct Plate;
    struct Echo;

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
};

}  // namespace lift::eng
