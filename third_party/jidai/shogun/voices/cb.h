#pragma once
// CB: cowbell. Two PolyBLEP pulses f and 1.4815·f (D = 0.4798), BP 2.64 kHz R 0.2, two-stage envelope,
// phase reset at the trigger with the two-instance 1 ms crossfade (§6.10, §3.8).
#include "common.h"

namespace shogun {

struct CbVoice : Voice {
  struct Inst {
    PolyBlepOsc o1, o2;
    double eFast = 0.0, eSlow = 0.0, gain = 0.0, step = 0.0;
    bool on = false;
  };
  Inst inst[2];
  int cur = 0;
  TptSvf bp;
  double f = 600.0, aFast = 0.0, aSlow = 0.0, kTone = 1.0, last = 0.0, tol2 = 1.0;
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    aFast = rcCoef(0.015, fsE);
    reset();
  }
  void reset() override {
    for (auto& i : inst) i = Inst{};
    cur = 0;
    bp.reset();
    last = 0.0;
  }
  void control(const VoiceCtx& c) override {
    f = clampPitch(300.0 * std::pow(4.0, c.ue[P_CB_TUNE]) * std::exp2(c.pitchOct) * c.tolPitch, 300.0, 1200.0);
    aSlow = rcCoef(decayTau(c.ue[P_CB_DECAY]) * c.tolTau, fsE_);
    kTone = std::exp2(c.toneV * 2.0 / 5.0);  // TONE jack: BP centre ±2 oct
    bp.set(2640.0 * kTone * c.tolCut, 0.2, fsE_);
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    control(c);
    Inst& old = inst[cur];
    if (old.on) old.step = old.gain / (0.001 * fsE_);
    cur ^= 1;
    Inst& in = inst[cur];
    in.o1.reset(0.0);
    in.o2.reset(0.0);
    in.eFast = 1.0;
    in.eSlow = 1.0;
    in.gain = 1.0;
    in.step = 0.0;
    in.on = true;
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    double src = 0.0;
    for (auto& in : inst) {
      if (!in.on) continue;
      in.o1.setFreq(f, fsE_);
      in.o2.setFreq(1.4815 * f * tol2, fsE_);
      const double s = 0.5 * (in.o1.pulse(SchmittBank::kDuty) + in.o2.pulse(SchmittBank::kDuty));
      src += in.gain * s * (0.6 * in.eFast + 0.4 * in.eSlow);
      in.o1.advance();
      in.o2.advance();
      in.eFast = flushDenormal(in.eFast * aFast);
      in.eSlow = flushDenormal(in.eSlow * aSlow);
      if (in.step > 0.0) {
        in.gain -= in.step;
        if (in.gain <= 0.0) in = Inst{};
      } else if (0.6 * in.eFast + 0.4 * in.eSlow < kQuiet) {
        in = Inst{};
      }
    }
    bp.tick(src);
    last = bp.bpNorm();
    L = R = last;
    return false;
  }
  bool quiet() const override { return !inst[0].on && !inst[1].on && bp.s1 * bp.s1 + bp.s2 * bp.s2 < 1e-12; }
  double env() const override {
    double m = 0.0;
    for (const auto& in : inst)
      if (in.on) m = std::fmax(m, in.gain * (0.6 * in.eFast + 0.4 * in.eSlow));
    return m;
  }
  double core() const override { return last; }
};

}  // namespace shogun
