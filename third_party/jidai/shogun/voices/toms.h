#pragma once
// LTC / MTC / HTC: toms and congas (§6.11). Resonator body (+ conga partial 2.30·f at 0.6·τ), step bend τ 80 ms,
// Decay 127 ring law (τ_r 20 s × S 0.55 / τ 0.35 s, 4 s hold, 50 ms release), shared TOM NZ, triple wave shaper.
#include "common.h"

namespace shogun {

struct TomVoice : Voice {
  explicit TomVoice(int id) : id_(id) {
    wave.bind(id);
    if (id == LTC) { fMin = 70.0; fMax = 180.0; }
    else if (id == MTC) { fMin = 100.0; fMax = 280.0; }
    else { fMin = 140.0; fMax = 400.0; }
    pTune = id == LTC ? P_LTC_TUNE : (id == MTC ? P_MTC_TUNE : P_HTC_TUNE);
    pDecay = id == LTC ? P_LTC_DECAY : (id == MTC ? P_MTC_DECAY : P_HTC_DECAY);
    pConga = id == LTC ? P_LTC_CONGA : (id == MTC ? P_MTC_CONGA : P_HTC_CONGA);
    pNz = id == LTC ? P_LTC_NZ : (id == MTC ? P_MTC_NZ : P_HTC_NZ);
  }
  int id_, pTune, pDecay, pConga, pNz;
  double fMin, fMax;
  Resonator r1, r2;
  RcEnv e;
  TptOnePole nzLp;
  WaveSlot wave;
  XorShift32 rng;
  double shiftOct = 0.0;  // LIFT: whole-range transpose in octaves (setShift), for the lower 909 toms
  void setShift(double oct) { shiftOct = oct; }
  double b = 0.0, aB = 0.0, bend = 0.0, fTune = 100.0, fMul = 1.0, tauR = 0.0759, fBase = 100.0;
  double nzEnv = 0.0, aNz = 0.0, nzAmt = 0.5, last = 0.0, lastNz = 0.0;
  double ring = 1.0, aRing = 0.0, aRel = 0.0;
  long n = 0, holdN = 0;
  bool conga = false, nz = false, ringMode = false;
  Memo1 mTune_, mOct_, mTau_, mBend_;  // per-voice coefficient memos (bit-exact, see dsp.h Memo1)

  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    wave.prepare(fsE);
    aB = rcCoef(0.080, fsE);
    aNz = rcCoef(0.12, fsE);
    aRing = rcCoef(0.35, fsE);
    aRel = rcCoef(0.050, fsE);
    holdN = std::lround(4.0 * fsE);
    nzLp.set(4000.0, fsE);
    rng.seed(voiceSeed(id_));
    reset();
  }
  void reset() override {
    r1.reset();
    r2.reset();
    e.reset();
    nzLp.reset();
    wave.reset();
    b = nzEnv = 0.0;
    ring = 1.0;
    ringMode = false;
    last = lastNz = 0.0;
  }
  void control(const VoiceCtx& c) override {
    const double* ue = c.ue;
    const double ratio = fMax / fMin;
    fTune = fMin * mTune_(ue[pTune], [ratio](double u) { return std::pow(ratio, u); });
    fMul = mOct_(c.pitchOct, [](double x) { return std::exp2(x); }) * c.tolPitch;
    nzAmt = ue[P_TOM_NOISE];
    const bool full = ue[pDecay] >= 126.5 / 127.0;
    tauR = (full ? 20.0 : mTau_(ue[pDecay], decayTau)) * c.tolTau;
    e.setDecay(tauR, fsE_);
    wave.control(c);
  }
  double freq() { return clampPitch(fTune * mBend_(bend * b / 12.0, [](double x) { return std::exp2(x); }) * fMul * std::exp2(shiftOct), fMin, fMax); }
  void trigger(const VoiceCtx& c, const HitInfo& h) override {
    control(c);
    conga = stepIndex(c.ue[pConga], 2) == 1;
    nz = stepIndex(c.ue[pNz], 2) == 1;
    ringMode = c.ue[pDecay] >= 126.5 / 127.0;
    bend = h.bend;
    b = 1.0;
    const double f = freq();
    r1.set(f, tauR, fsE_);
    r1.kick(1.0);
    if (conga) {
      r2.set(2.30 * f, 0.6 * tauR, fsE_);
      r2.kick(1.0);
    }
    e.trigger();
    ring = 1.0;
    n = 0;
    if (nz) nzEnv = 1.0;
  }
  bool tick(const VoiceCtx& c, double vc, double& L, double& R) override {
    const double f = freq();
    if (c.sub == 0) fBase = f;
    r1.set(f, tauR, fsE_);
    double y0 = r1.tick();
    double R1 = r1.svf.R;
    if (conga || r2.energy() > 0.0) {
      r2.set(clampd(2.30 * f, 1.0, 0.45 * fsE_), 0.6 * tauR, fsE_);
      y0 += 0.35 * r2.tick();
    }
    double x = y0;
    if (wave.active()) {
      if (wave.shape > 0.0) x = WaveShaper::shapeMorph(r1.lp(), r1.bp(), R1, wave.shape, f / fsE_);
      if (wave.preVca) x = WaveShaper::preVcaInput(x, WaveShaper::quadAmp(r1.lp(), r1.bp(), R1), e.e);
      x = wave.ts.process(x, vc);
    }
    last = y0;
    lastNz = rng.noise();
    double nzOut = 0.0;
    if (nzEnv > 0.0) {
      nzOut = nzAmt * nzLp.lp(lastNz) * nzEnv;
      nzEnv = nzEnv * aNz;
      if (nzEnv < 1e-12) nzEnv = 0.0;
    }
    double g = 1.0;
    if (ringMode) {
      if (n < holdN) ring = 0.55 + aRing * (ring - 0.55);
      else ring = ring * aRel;
      g = ring;
      ++n;
    }
    e.tick();
    b = flushDenormal(b * aB);
    L = R = x * g + nzOut;
    return false;
  }
  // The ring envelope value as the old engine printed it (S + (1−S)·e^{−n/τ}, then × e^{−(n−4 s)/50 ms}).
  double ringEnv() const { return ringMode ? ring : e.e; }
  bool quiet() const override {
    if (nzEnv >= kQuiet) return false;
    if (ringMode) return ring < kQuiet;
    // The voice envelope sets the −90 dB end (§3.7); the resonator guard (−84 dB) only catches a swept body.
    return e.quiet() && r1.energy() < 4.0 * kQuiet * kQuiet && r2.energy() < 4.0 * kQuiet * kQuiet;
  }
  double env() const override { return ringMode ? ring : e.e; }
  double pitchEnv() const override { return b; }
  double core() const override { return last; }
  double noiseSample() const override { return lastNz; }
};

}  // namespace shogun
