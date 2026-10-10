#pragma once
// SD: two bridged-T tones (pitch + bend envelopes) and a snappy noise path HP 1.8 kHz → LP 9 kHz (§6.3).
#include "common.h"

namespace shogun {

struct SdVoice : Voice {
  // Snappy path make-up: the band-limited noise (unit-variance source) sits ~10 dB under the tones without it.
  // DESIGN CHOICE, set so the noon snare meets testNoonIsADrumMachine (noise share ≥ 25 % of 20–200 ms energy).
  static constexpr double kSnapGain = 2.2;
  Resonator t1, t2;
  TptSvf hp, lp;
  RcEnv et, en;
  XorShift32 rng;
  double p = 0.0, b = 0.0, aP = 0.0, aB = 0.0, bend = 0.0, depth = 0.0;
  double f1 = 219.1, f2 = 219.1, tauT = 0.0759, tone = 0.5, snappy = 0.5, fMul = 1.0, f1Base = 219.1;
  double last = 0.0, lastNz = 0.0, lastT1 = 0.0, lastT2 = 0.0;

  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    hp.set(1800.0, 0.7, fsE);
    lp.set(9000.0, 0.7, fsE);
    en.setAttack(0.0002, fsE);
    rng.seed(voiceSeed(SD));
    reset();
  }
  void reset() override {
    t1.reset();
    t2.reset();
    hp.reset();
    lp.reset();
    et.reset();
    en.reset();
    p = b = 0.0;
    last = lastNz = lastT1 = lastT2 = 0.0;
  }
  void control(const VoiceCtx& c) override {
    const double* ue = c.ue;
    f1 = 120.0 * std::pow(400.0 / 120.0, ue[P_SD_TUNE]);
    f2 = f1 * std::exp2((-8.0 + 16.0 * ue[P_SD_DETUNE]) / 12.0);
    depth = 14.0 * ue[P_SD_PITCH];
    const double tauP = 0.01 + 0.12 * ue[P_SD_PITCH];
    aP = rcCoef(tauP, fsE_);
    aB = rcCoef(tauP > 0.08 ? tauP : 0.08, fsE_);
    tauT = decayTau(ue[P_SD_TDECAY]) * c.tolTau;
    et.setDecay(tauT, fsE_);
    en.setDecay(decayTau(ue[P_SD_SNDEC]) * c.tolTau, fsE_);
    tone = ue[P_SD_TONE];
    snappy = ue[P_SD_SNAPPY];
    fMul = std::exp2(c.pitchOct) * c.tolPitch;
  }
  double mulAt() const { return std::exp2((depth * p + bend * b) / 12.0) * fMul; }
  void trigger(const VoiceCtx& c, const HitInfo& h) override {
    control(c);
    bend = h.bend;
    p = 1.0;
    b = 1.0;
    const double m = mulAt();
    t1.set(clampPitch(f1 * m, 120.0, 400.0), tauT, fsE_);
    t2.set(clampPitch(f2 * m, 120.0 * 0.63, 400.0 * 1.59), tauT, fsE_);
    t1.kick(1.0);
    t2.kick(1.0);
    et.aA = 0.0;
    et.trigger();
    en.trigger();
  }
  bool tick(const VoiceCtx& c, double, double& L, double& R) override {
    const double m = mulAt();
    const double fa = clampPitch(f1 * m, 120.0, 400.0);
    if (c.sub == 0) f1Base = fa;
    t1.set(fa, tauT, fsE_);
    t2.set(clampPitch(f2 * m, 120.0 * 0.63, 400.0 * 1.59), tauT, fsE_);
    lastT1 = t1.tick();
    lastT2 = t2.tick();
    const double tones = (1.0 - tone) * lastT1 + tone * lastT2;
    hp.tick(rng.noise());
    lp.tick(hp.hp);
    lastNz = lp.lp;
    const double nz = kSnapGain * snappy * lp.lp * en.e;
    last = tones;
    et.tick();
    en.tick();
    p = flushDenormal(p * aP);
    b = flushDenormal(b * aB);
    L = R = tones + nz;
    return false;
  }
  bool quiet() const override { return et.quiet() && en.quiet() && t1.energy() < 1e-9 && t2.energy() < 1e-9; }
  double env() const override { return et.e > en.e ? et.e : en.e; }
  double pitchEnv() const override { return p; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
};

}  // namespace shogun
