#pragma once
// RS rim (§6.4), CL claves (§6.5), MA maracas (§6.8).
#include "common.h"

namespace shogun {

struct RsVoice : Voice {
  Resonator res;
  TptOnePole hp;
  RcEnv e;
  double f = 790.6, tau = 0.012, last = 0.0;
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    hp.set(300.0, fsE);
    reset();
  }
  void reset() override {
    res.reset();
    hp.reset();
    e.reset();
    last = 0.0;
  }
  void control(const VoiceCtx& c) override {
    f = clampPitch(250.0 * std::pow(10.0, c.ue[P_RS_TUNE]) * std::exp2(c.pitchOct) * c.tolPitch, 250.0, 2500.0);
    tau = 0.012 * std::exp2(c.decayV / 2.5) * c.tolTau;  // τ = 12 ms·2^{AMT·V/2.5} (KEPT)
    e.setDecay(tau, fsE_);
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    control(c);
    res.set(f, tau, fsE_);
    res.kick(1.0);
    e.trigger();
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    res.set(f, tau, fsE_);
    last = res.tick();
    e.tick();
    L = R = hp.hp(last);
    return false;
  }
  bool quiet() const override { return e.quiet() && res.energy() < 1e-9; }
  double env() const override { return e.e; }
  double core() const override { return last; }
};

struct ClVoice : Voice {
  Resonator res;
  RcEnv e;
  double f = 1095.4, tau = 0.0759, last = 0.0;
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    reset();
  }
  void reset() override {
    res.reset();
    e.reset();
    last = 0.0;
  }
  void control(const VoiceCtx& c) override {
    f = clampPitch(400.0 * std::pow(7.5, c.ue[P_CL_TUNE]) * std::exp2(c.pitchOct) * c.tolPitch, 400.0, 3000.0);
    tau = decayTau(c.ue[P_CL_DECAY]) * c.tolTau;
    e.setDecay(tau, fsE_);
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    control(c);
    res.set(f, tau, fsE_);
    res.kick(1.0);
    e.trigger();
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    res.set(f, tau, fsE_);
    last = res.tick();
    e.tick();
    L = R = last;
    return false;
  }
  bool quiet() const override { return e.quiet() && res.energy() < 1e-9; }
  double env() const override { return e.e; }
  double core() const override { return last; }
};

struct MaVoice : Voice {
  TptSvf hp;
  RcEnv e;
  XorShift32 rng;
  double last = 0.0, lastNz = 0.0;
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    e.setAttack(0.0002, fsE);
    rng.seed(voiceSeed(MA));
    reset();
  }
  void reset() override {
    hp.reset();
    e.reset();
    last = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    // PITCH jack moves the HP corner (§5 table); TONE goes to LEVEL through ue.
    hp.set(4500.0 * std::exp2(c.pitchOct) * c.tolCut, 0.5, fsE_);
    e.setDecay(decayTau(c.ue[P_MA_DECAY]) * c.tolTau, fsE_);
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    control(c);
    e.trigger();
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    lastNz = rng.noise();
    hp.tick(lastNz);
    last = 2.0 * hp.R * hp.hp * e.e;
    e.tick();
    L = R = last;
    return false;
  }
  bool quiet() const override { return e.quiet(); }
  double env() const override { return e.e; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
};

}  // namespace shogun
