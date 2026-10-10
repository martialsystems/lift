#pragma once
// CH / OH hats (§6.6) and CY cymbal (§6.7) on Schmitt metal banks (§4.5).
#include "common.h"

namespace shogun {

struct HatVoice : Voice {
  explicit HatVoice(int id) : id_(id) {}
  int id_;
  SchmittBank bank;
  TptSvf bp;
  TptOnePole hp;
  RcEnv e;
  XorShift32 rng;
  double kHat = 1.0, last = 0.0, lastNz = 0.0;
  Memo1 mTune_, mOct_, mTone_, mTau_;  // per-voice coefficient memos (bit-exact, see dsp.h Memo1)
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    e.setAttack(0.0002, fsE);
    rng.seed(voiceSeed(id_));
    reset();
  }
  void reset() override {
    bank.reset();
    bp.reset();
    hp.reset();
    e.reset();
    last = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    kHat = mTune_(2.0 * (c.ue[P_CH_TUNE] - 0.5), [](double x) { return std::exp2(x); }) * mOct_(c.pitchOct, [](double x) { return std::exp2(x); });  // HAT TUNE, shared by CH and OH
    const double kTone = mTone_(c.toneV * 2.0 / 5.0, [](double x) { return std::exp2(x); });
    bp.set(7100.0 * kTone * c.tolCut, 0.25, fsE_);
    hp.set(5000.0 * kTone * c.tolCut, fsE_);
    const double u = id_ == CH ? c.ue[P_CH_DECAY] : c.ue[P_OH_DECAY];
    if (!choked_) e.setDecay(mTau_(u, decayTau) * c.tolTau, fsE_);
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    choked_ = false;
    control(c);
    e.trigger();
  }
  void choke(double tau) override {
    if (e.stage == RcEnv::Idle) return;
    choked_ = true;
    e.discharge(tau, fsE_);
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    lastNz = rng.noise();
    const double src = bank.tick(kHat, fsE_) + 0.25 * lastNz;
    bp.tick(src);
    last = hp.hp(bp.bpNorm()) * e.e;
    e.tick();
    L = R = last;
    return false;
  }
  bool quiet() const override { return e.quiet(); }
  double env() const override { return e.e; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
  bool choked_ = false;
};

struct CyVoice : Voice {
  SchmittBank bank;
  TptSvf lo, hi;
  RcEnv eLo, eHi;
  XorShift32 rng;
  double kCy = 1.0, tone = 0.5, last = 0.0, lastNz = 0.0;
  Memo1 mTune_, mOct_, mTau_;
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    eLo.setAttack(0.0002, fsE);
    eHi.setAttack(0.0002, fsE);
    rng.seed(voiceSeed(CY));
    reset();
  }
  void reset() override {
    bank.reset();
    lo.reset();
    hi.reset();
    eLo.reset();
    eHi.reset();
    last = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    kCy = mTune_(2.0 * (c.ue[P_CY_TUNE] - 0.5), [](double x) { return std::exp2(x); }) * mOct_(c.pitchOct, [](double x) { return std::exp2(x); });
    lo.set(3440.0 * c.tolCut, 0.30, fsE_);
    hi.set(7100.0 * c.tolCut, 0.25, fsE_);
    const double tau = mTau_(c.ue[P_CY_DECAY], decayTau) * c.tolTau;
    eLo.setDecay(tau, fsE_);
    eHi.setDecay(0.5 * tau, fsE_);
    tone = c.ue[P_CY_TONE];
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    control(c);
    eLo.trigger();
    eHi.trigger();
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    const double m = bank.tick(kCy, fsE_);
    lastNz = rng.noise();
    lo.tick(m);
    hi.tick(m);
    last = (1.0 - tone) * lo.bpNorm() * eLo.e + tone * hi.bpNorm() * eHi.e + 0.15 * lastNz * eLo.e;
    eLo.tick();
    eHi.tick();
    L = R = last;
    return false;
  }
  bool quiet() const override { return eLo.quiet() && eHi.quiet(); }
  double env() const override { return eLo.e; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
};

}  // namespace shogun
