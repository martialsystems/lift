#pragma once
// CP: clap. count = 1 + floor(8u) bursts 11 ms apart, burst BP f_b = 600·1.2^s (≤ 9,244 Hz), stereo burst pans,
// tail LP(HP300(w)) opening one gap after the last burst (§6.9).
#include "common.h"

namespace shogun {

struct CpVoice : Voice {
  static constexpr int kMaxBursts = 8;
  TptSvf bp, tailLp;
  TptOnePole tailHp;
  RcEnv et;
  XorShift32 rng;
  double eB[kMaxBursts] = {};
  double panLg[kMaxBursts] = {}, panRg[kMaxBursts] = {};
  double aBurst = 0.0, attack = 0.5, fB = 600.0, lastBp = 0.0, lastNz = 0.0, last = 0.0;
  long n = 0, gap = 528, tailAt = -1;
  int count = 5, sound = 0, nextBurst = kMaxBursts;

  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    aBurst = rcCoef(0.003, fsE);
    gap = std::lround(0.011 * fsE);
    tailHp.set(300.0, fsE);
    et.setAttack(0.0002, fsE);
    rng.seed(voiceSeed(CP));
    reset();
  }
  void reset() override {
    bp.reset();
    tailLp.reset();
    tailHp.reset();
    et.reset();
    for (double& e : eB) e = 0.0;
    nextBurst = kMaxBursts;
    tailAt = -1;
    n = 0;
    last = lastBp = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    attack = c.ue[P_CP_ATTACK];
    tailLp.set(400.0 * std::pow(15.0, c.ue[P_CP_FILTER]) * c.tolCut, 0.7, fsE_);
    et.setDecay(decayTau(c.ue[P_CP_DECAY]) * c.tolTau, fsE_);
    bp.set(clampd(fB * std::exp2(c.pitchOct) * c.tolPitch, 150.0, 0.45 * fsE_), 0.3, fsE_);
  }
  void trigger(const VoiceCtx& c, const HitInfo&) override {
    count = 1 + stepIndex(c.ue[P_CP_COUNT], 8);
    sound = stepIndex(c.ue[P_CP_SOUND], 16);
    fB = 600.0 * std::pow(1.2, sound);
    control(c);
    for (int i = 0; i < count; ++i) {
      const double pan = count > 1 ? -1.0 + 2.0 * i / (count - 1) : 0.0;
      panLg[i] = panL(pan);
      panRg[i] = panR(pan);
    }
    n = 0;
    nextBurst = 0;
    tailAt = static_cast<long>(count) * gap;
  }
  bool tick(const VoiceCtx&, double, double& L, double& R) override {
    if (nextBurst < count && n == static_cast<long>(nextBurst) * gap) {
      eB[nextBurst] = 1.0;
      ++nextBurst;
    }
    if (tailAt >= 0 && n == tailAt) {
      et.trigger();
      tailAt = -1;
    }
    const double w = rng.noise();
    lastNz = w;
    bp.tick(w);
    lastBp = bp.bpNorm();
    double sl = 0.0, sr = 0.0;
    for (int i = 0; i < kMaxBursts; ++i) {
      if (dsp::exactEq(eB[i], 0.0)) continue;
      sl += eB[i] * panLg[i];
      sr += eB[i] * panRg[i];
      eB[i] = eB[i] * aBurst;
      if (eB[i] < 1e-12) eB[i] = 0.0;
    }
    tailLp.tick(tailHp.hp(w));
    const double tail = tailLp.lp * et.e;
    et.tick();
    L = attack * lastBp * sl + 0.7071067811865476 * tail;
    R = attack * lastBp * sr + 0.7071067811865476 * tail;
    last = 0.5 * (L + R);
    ++n;
    return true;
  }
  // Burst i's current value (test probe): the shared band-passed noise times burst i's envelope.
  double burst(int i) const { return i < kMaxBursts ? lastBp * eB[i] : 0.0; }
  bool quiet() const override {
    if (nextBurst < count || tailAt >= 0) return false;
    for (double e : eB)
      if (e >= kQuiet) return false;
    return et.quiet();
  }
  double env() const override {
    double m = et.e;
    for (double e : eB) m = e > m ? e : m;
    return m;
  }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
};

}  // namespace shogun
