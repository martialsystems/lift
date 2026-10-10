#pragma once
// BD1: bridged-T kick with pitch envelope, transient, noise, WAVE, DRIVE (§6.1).
#include "common.h"

namespace shogun {

struct Bd1Voice : Voice {
  Resonator body, tr;
  TptSvf nzf;
  RcEnv eb;  // = 1 at trigger (instant), decays with τ_b
  TanhAdaa drive;
  WaveSlot wave;
  XorShift32 rng;
  double p = 0.0, aP = 0.0, bend = 0.0, depth = 0.0, tauB = 0.0759, fTune = 70.0, fMul = 1.0;
  double noise = 0.0, d = 0.0, fTr = 160.0, attack = 0.5;
  double fBase = 70.0, last = 0.0, lastNz = 0.0;
  int sound = 0;

  Bd1Voice() { wave.bind(BD1); }
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    wave.prepare(fsE);
    rng.seed(voiceSeed(BD1));
    reset();
  }
  void reset() override {
    body.reset();
    tr.reset();
    nzf.reset();
    eb.reset();
    drive.reset();
    wave.reset();
    p = 0.0;
    last = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    const double* ue = c.ue;
    tauB = decayTau(ue[P_BD1_DECAY]) * c.tolTau;
    depth = 18.0 * ue[P_BD1_PITCH];
    aP = rcCoef(0.012 + 0.25 * ue[P_BD1_PITCH], fsE_);
    fTune = 35.0 * std::pow(4.0, ue[P_BD1_TUNE]);
    fMul = std::exp2(c.pitchOct) * c.tolPitch;
    nzf.set(200.0 * std::pow(40.0, ue[P_BD1_FILTER]) * c.tolCut, 0.7071067811865476, fsE_);
    noise = ue[P_BD1_NOISE];
    d = 9.0 * ue[P_BD1_DRIVE];
    attack = ue[P_BD1_ATTACK];
    eb.setDecay(tauB, fsE_);
    wave.control(c);
  }
  double freqAt(double pe) const {
    return clampPitch(fTune * std::exp2((depth + bend) * pe / 12.0) * fMul, 35.0, 140.0);
  }
  void trigger(const VoiceCtx& c, const HitInfo& h) override {
    control(c);
    sound = stepIndex(c.ue[P_BD1_SOUND], 16);
    fTr = 160.0 * std::pow(1.35, sound);
    bend = h.bend;
    p = 1.0;
    eb.aA = 0.0;
    eb.trigger();
    body.set(freqAt(1.0), tauB, fsE_);
    body.kick(1.0);
    tr.set(fTr, 0.004, fsE_);
    tr.kick(attack);
  }
  bool tick(const VoiceCtx& c, double vc, double& L, double& R) override {
    const double f = freqAt(p);
    if (c.sub == 0) fBase = f;
    body.set(f, tauB, fsE_);
    const double b = body.tick();
    const double t = tr.tick();
    nzf.tick(rng.noise());
    lastNz = nzf.lp;
    const double nz = noise * nzf.lp * eb.e;
    double x = b;
    if (wave.active()) {
      if (wave.shape > 0.0) x = WaveShaper::shapeMorph(body.lp(), body.bp(), body.svf.R, wave.shape, f / fsE_);
      if (wave.preVca) x = WaveShaper::preVcaInput(x, WaveShaper::quadAmp(body.lp(), body.bp(), body.svf.R), eb.e);
      x = wave.ts.process(x, vc);
    }
    last = b;
    const double y = drive.tick(x + t + nz, d);
    eb.tick();
    p = flushDenormal(p * aP);
    L = R = y;
    return false;
  }
  bool quiet() const override { return eb.quiet() && body.energy() < 1e-9 && tr.energy() < 1e-9; }
  double env() const override { return eb.e; }
  double pitchEnv() const override { return p; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
};

}  // namespace shogun
