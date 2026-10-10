#pragma once
// BD2: VCO kick, PolyBLAMP triangle → tanh sine shaper, click resonator, hold law, two-instance retrigger (§6.2).
#include "common.h"

namespace shogun {

struct Bd2Voice : Voice {
  struct Inst {
    double ph = 0.0, gain = 0.0, step = 0.0;  // gain ramps to 0 over 1 ms when the instance is released
    bool on = false;
  };
  Inst inst[2];
  int cur = 0;
  RcEnv env_;
  Resonator click;
  WaveSlot wave;
  XorShift32 rng;
  double b = 0.0, aB = 0.0, bend = 0.0, fTune = 67.08, fMul = 1.0, tone = 0.5, fBase = 67.08, last = 0.0, lastNz = 0.0;
  bool hold = false;

  Bd2Voice() { wave.bind(BD2); }
  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    wave.prepare(fsE);
    rng.seed(voiceSeed(BD2));
    env_.setAttack(0.0002, fsE);
    aB = rcCoef(0.080, fsE);
    reset();
  }
  void reset() override {
    for (auto& i : inst) i = Inst{};
    cur = 0;
    env_.reset();
    click.reset();
    wave.reset();
    b = 0.0;
    last = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    const double* ue = c.ue;
    fTune = 45.0 * std::pow(100.0 / 45.0, ue[P_BD2_TUNE]);
    fMul = std::exp2(c.pitchOct) * c.tolPitch;
    tone = ue[P_BD2_TONE];
    hold = ue[P_BD2_DECAY] >= 126.5 / 127.0;  // Decay = 127 (§6.2, KEPT)
    if (hold) {
      env_.setSustain(0.70);
      env_.setDecay(0.40 * c.tolTau, fsE_);
    } else {
      env_.setSustain(0.0);
      env_.setDecay(decayTau(ue[P_BD2_DECAY]) * c.tolTau, fsE_);
    }
    wave.control(c);
  }
  double freq() const { return clampPitch(fTune * std::exp2(bend * b / 12.0) * fMul, 45.0, 100.0); }
  void trigger(const VoiceCtx& c, const HitInfo& h) override {
    control(c);
    bend = h.bend;
    b = 1.0;
    // Two instances (§3.8): the ringing one fades over 1 ms, the new one starts at phase 0.
    Inst& old = inst[cur];
    if (old.on) old.step = old.gain / (0.001 * fsE_);
    cur ^= 1;
    inst[cur].ph = 0.0;
    inst[cur].gain = 1.0;
    inst[cur].step = 0.0;
    inst[cur].on = true;
    env_.trigger();
    click.set(2.0 * fTune * fMul, 0.005, fsE_);
    click.kick(tone);
  }
  static double sineShape(double tri) { return std::tanh(1.5 * tri) / 0.90514825364486640; }  // tanh(1.5)
  bool tick(const VoiceCtx& c, double vc, double& L, double& R) override {
    const double f = freq();
    if (c.sub == 0) fBase = f;
    const double dt = clampd(f / fsE_, 0.0, 0.45);
    double body = 0.0;
    for (auto& in : inst) {
      if (!in.on) continue;
      double s = sineShape(PolyBlepOsc::triAt(in.ph, dt));
      if (wave.shape > 0.0) s = WaveShaper::morphAt(s, 1.0, in.ph, wave.shape, dt);
      body += in.gain * s;
      in.ph += dt;
      if (in.ph >= 1.0) in.ph -= 1.0;
      if (in.step > 0.0) {
        in.gain -= in.step;
        if (in.gain <= 0.0) in = Inst{};
      }
    }
    const double e = env_.e;
    double y;
    if (wave.active()) {
      y = wave.preVca ? wave.ts.process(body * e, vc) : wave.ts.process(body, vc) * e;
    } else {
      y = body * e;
    }
    last = body;
    lastNz = rng.noise();
    const double ck = click.tick();
    env_.tick();
    b = flushDenormal(b * aB);
    L = R = y + ck;
    return false;
  }
  bool quiet() const override {
    return env_.quiet() && click.energy() < 1e-9 && !(inst[0].on && inst[0].step > 0.0) &&
           !(inst[1].on && inst[1].step > 0.0);
  }
  double env() const override { return env_.e; }
  double pitchEnv() const override { return b; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
  double clickSample() const { return click.lp(); }
};

}  // namespace shogun
