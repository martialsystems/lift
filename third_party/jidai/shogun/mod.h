#pragma once
// Modulation (§8): 4 LFOs with per-voice (OWN VOICE) instances, per-voice sources, 32-row matrix, the shared
// sum/clamp law u_eff = clamp(u_base + Σ d·C(s)·via + AMT·V/5, 0, 1). Fixed-size, no allocation.

#include "dsp.h"
#include "params.h"

namespace shogun {
namespace mod {

using namespace dsp;

enum Source : int {
  SRC_NONE,
  SRC_LFO1,
  SRC_LFO2,
  SRC_LFO3,
  SRC_LFO4,
  SRC_ENV,      // per voice
  SRC_PENV,     // per voice
  SRC_VEL,      // per voice
  SRC_ACC,      // per voice
  SRC_RNDHIT,   // per voice
  SRC_NOTE,     // lead/bass
  SRC_RND,      // global, stepped once per step
  SRC_MODW,     // MIDI CC1
  SRC_AT,       // channel aftertouch
  kSourceCount
};
inline const char* const kSourceNames[kSourceCount] = {"OFF", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "ENV", "PITCH ENV",
                                                       "VEL", "ACC", "RND/HIT", "NOTE", "RND", "MOD W", "AT"};
inline bool perVoiceSource(int s) { return s >= SRC_ENV && s <= SRC_NOTE; }
inline bool bipolarSource(int s) { return s == SRC_RNDHIT || s == SRC_NOTE; }  // LFOs depend on POL

enum Curve : int { LIN, EXP, LOG, SCRV, kCurveCount };
inline const char* const kCurveNames[kCurveCount] = {"LIN", "EXP", "LOG", "S-CRV"};

// Row curve C(s) (§8.4).
inline double curve(int c, double s) {
  switch (c) {
    case EXP: return (s < 0 ? -1.0 : 1.0) * s * s;
    case LOG: return (s < 0 ? -1.0 : 1.0) * std::sqrt(std::fabs(s));
    case SCRV: return s * (1.5 - 0.5 * s * s);
    default: return s;
  }
}
// The shared law (§8.4): u_eff = clamp(u_base + m + AMT·V/5, 0, 1).
inline double effectiveU(double uBase, double m, double amt, double volts) {
  return clampd(uBase + m + amt * volts / 5.0, 0.0, 1.0);
}

constexpr int kRows = 32;
struct Row {
  int src = SRC_NONE;
  int srcVoice = -1;  // per-voice sources: −1 = the destination's own voice, else a VoiceId
  int dst = -1;       // ParamId
  double depth = 0.0; // fraction of the full knob range, [−1, 1]
  int via = SRC_NONE; // optional scaler source in [0, 1] (VEL, MOD W, …)
  int viaVoice = -1;
  int curve = LIN;
  bool on = true;
};

// ---------------------------------------------------------------- LFO (§8.2, §8.3)
enum LfoShape : int { L_SIN, L_TRI, L_RAMP, L_SAW, L_SQR, L_SH };
enum LfoMode : int { M_FREE_RUN, M_RETRIG, M_ONE_SHOT };
constexpr int kRetrigBar = 0, kRetrigAny = 1, kRetrigOwn = 2;  // then 3 + voice

struct LfoInstance {
  double ph = 0.0;      // [0, 1)
  double hold = 0.0;    // S&H value
  double y = 0.0;       // declick/slew state
  double raw = 0.0;     // pre-declick shape value b (test probe, §15.4 testOwnVoiceRetrigIndependent)
  double fade = 1.0;    // FADE rise
  bool done = false;    // ONE-SHOT finished
  bool started = false;
  XorShift32 rng;
};

class Lfo {
 public:
  int index = 0;
  LfoInstance global;
  LfoInstance inst[kVoices];
  int lastVoice = -1;  // the jack carries the instance of the most recently triggered voice (§8.3)
  // Parameters (read from ue once per base sample).
  double rateU = 0.5, phase = 0.0, slew = 0.0, pw = 0.5, depthOut = 1.0, fadeS = 0.0;
  int shape = L_SIN, mode = M_FREE_RUN, retrigBy = kRetrigBar, div = 15;
  bool sync = false, bi = true;
  double fs = 48000.0;

  void prepare(double sampleRate, int idx) {
    fs = sampleRate;
    index = idx;
    reset();
  }
  void reset() {
    auto init = [&](LfoInstance& in, std::uint32_t salt) {
      in = LfoInstance{};
      in.rng.seed(0xA341316Cu ^ (static_cast<std::uint32_t>(index + 1) * 0x85EBCA6Bu) ^ salt);
    };
    init(global, 0u);
    for (int v = 0; v < kVoices; ++v) init(inst[v], static_cast<std::uint32_t>(v + 1) * 0x9E3779B9u);
    lastVoice = -1;
  }
  void read(const double* ue) {
    const int base = P_LFO_1_RATE + index * (P_LFO_2_RATE - P_LFO_1_RATE);
    rateU = ue[base + 0];
    shape = stepIndex(ue[base + 1], 6);
    phase = ue[base + 2];
    slew = ue[base + 3];
    pw = clampd(ue[base + 4], 0.02, 0.98);
    sync = stepIndex(ue[base + 5], 2) == 1;
    div = stepIndex(ue[base + 6], kLfoDivCount);
    bi = stepIndex(ue[base + 7], 2) == 1;
    mode = stepIndex(ue[base + 8], 3);
    retrigBy = stepIndex(ue[base + 9], 3 + kVoices);
    depthOut = ue[base + 10];
    fadeS = 2.0 * ue[base + 11];
  }
  bool ownVoice() const { return mode != M_FREE_RUN && retrigBy == kRetrigOwn; }
  double divBeats() const { return kLfoDivBeats[div]; }
  double freqHz(double bpm) const { return sync ? bpm / (60.0 * divBeats()) : 0.01 * std::pow(4000.0, rateU); }

  // Retrigger events (BAR, ANY TRIG, voice TRIG, OWN VOICE).
  void onTrigger(int voice) {
    if (ownVoice()) {
      restart(inst[voice]);
      lastVoice = voice;
      return;
    }
    if (mode == M_FREE_RUN) return;
    if (retrigBy == kRetrigAny || retrigBy == 3 + voice) restart(global);
  }
  void onBar() {
    if (mode != M_FREE_RUN && retrigBy == kRetrigBar) restart(global);
  }

  // Advance every live instance by one base sample. ppq/locked: the host/internal song position (SYNC FREE-RUN
  // re-anchors φ = frac(ppq/D) every sample, so blocks and jumps land on the exact phase, §8.2).
  double kRate_ = -1.0, kBpm_ = -1.0, kSlew_ = -1.0, kFade_ = -1.0, kFs_ = -1.0; int kSync_ = -1, kDiv_ = -1;
  double cF_ = 0.0, cA_ = 0.0, cAF_ = 0.0;
  void tick(double bpm, double ppq, bool locked) {
    if (!(dsp::exactEq(kRate_, rateU) && dsp::exactEq(kBpm_, bpm) && dsp::exactEq(kSlew_, slew) && dsp::exactEq(kFade_, fadeS) && dsp::exactEq(kFs_, fs) && kSync_ == (int)sync && kDiv_ == div)) {
      kRate_ = rateU; kBpm_ = bpm; kSlew_ = slew; kFade_ = fadeS; kFs_ = fs; kSync_ = (int)sync; kDiv_ = div;
      cF_ = freqHz(bpm);
      const double T0 = cF_ > 0.0 ? 1.0 / cF_ : 1.0;
      cA_ = rcCoef(std::fmax(0.00025, slew * T0 / 4.0), fs);
      cAF_ = fadeS > 0.0 ? rcCoef(fadeS, fs) : 0.0;
    }
    const double f = cF_;
    const double dphi = f / fs;
    const double a = cA_;
    const double aFade = cAF_;
    if (mode == M_FREE_RUN && sync && locked) {
      const double target = ppq / divBeats();
      advanceAnchored(global, target - std::floor(target), dphi, a);
    } else {
      advance(global, dphi, a, aFade);
    }
    if (ownVoice()) {
      for (auto& in : inst)
        if (in.started) advance(in, dphi, a, aFade);
    }
  }

  // Value s of the instance used for a destination of voice v (−1: global or most recent own-voice instance).
  double value(int v) const {
    const LfoInstance& in = instanceFor(v);
    const double b = in.y * in.fade;
    return bi ? b : 0.5 * (b + 1.0);
  }
  double rawValue(int v) const { return instanceFor(v).raw; }
  double jackVolts() const { return 5.0 * value(-1) * depthOut; }
  const LfoInstance& instanceFor(int v) const {
    if (!ownVoice()) return global;
    if (v >= 0 && v < kVoices) return inst[v];
    return lastVoice >= 0 ? inst[lastVoice] : global;
  }

  // Shape at phase t (§8.2), with PolyBLEP corrections for dt = Δφ.
  double shapeAt(double t, double dt, const LfoInstance& in) const {
    const double k = pw;
    switch (shape) {
      case L_SIN: return std::sin(2.0 * kPi * t);
      case L_TRI: return t < k ? -1.0 + 2.0 * t / k : 1.0 - 2.0 * (t - k) / (1.0 - k);
      case L_RAMP: return 2.0 * t - 1.0 - polyBlep(t, dt);
      case L_SAW: return 1.0 - 2.0 * t + polyBlep(t, dt);
      case L_SQR: return (t < k ? 1.0 : -1.0) + polyBlep(t, dt) - polyBlep(wrap01(t - k), dt);
      default: return in.hold;
    }
  }

 private:
  void restart(LfoInstance& in) {
    in.ph = 0.0;
    in.done = false;
    in.started = true;
    if (fadeS > 0.0) in.fade = 0.0;
    if (shape == L_SH) in.hold = in.rng.bipolar();
    evaluate(in, 0.0);
  }
  void evaluate(LfoInstance& in, double dphi) {
    const double t = wrap01(in.ph + phase);
    in.raw = shapeAt(t, dphi > 0.0 ? dphi : 1e-9, in);
  }
  void step(LfoInstance& in, double a, double aFade) {
    in.y = flushDenormal(a * in.y + (1.0 - a) * in.raw);
    if (fadeS > 0.0) in.fade = 1.0 - aFade * (1.0 - in.fade);
    else in.fade = 1.0;
  }
  void advance(LfoInstance& in, double dphi, double a, double aFade) {
    in.started = true;
    evaluate(in, dphi);
    step(in, a, aFade);
    if (in.done) return;
    in.ph += dphi;
    if (in.ph >= 1.0) {
      if (mode == M_ONE_SHOT) {
        in.ph = 1.0 - 1e-12;
        in.done = true;
      } else {
        in.ph -= std::floor(in.ph);
        if (shape == L_SH) in.hold = in.rng.bipolar();
      }
    }
  }
  void advanceAnchored(LfoInstance& in, double ph, double dphi, double a) {
    if (ph < in.ph && shape == L_SH) in.hold = in.rng.bipolar();  // a wrap flags an S&H draw
    in.ph = ph;
    in.started = true;
    evaluate(in, dphi);
    step(in, a, 0.0);
  }
};

// Per-voice source values (filled by the engine every base sample).
struct VoiceSources {
  double env = 0.0, penv = 0.0, vel = 0.0, acc = 0.0, rndHit = 0.0, note = 0.0;
};

class ModSystem {
 public:
  Lfo lfo[4];
  Row rows[kRows];
  VoiceSources vs[kVoices];
  double rnd = 0.0, modW = 0.0, at = 0.0;
  double prev[kParamCount] = {}, next[kParamCount] = {};
  int counter = 0;
  bool anyRows = false;

  void prepare(double fs) {
    for (int i = 0; i < 4; ++i) lfo[i].prepare(fs, i);
    reset();
  }
  void reset() {
    for (auto& l : lfo) l.reset();
    for (auto& v : vs) v = VoiceSources{};
    for (int p = 0; p < kParamCount; ++p) {
      prev[p] = next[p] = 0.0;
      isActive[p] = false;
    }
    nActive = 0;
    counter = 0;
  }
  void clearRows() {
    for (auto& r : rows) r = Row{};
    anyRows = false;
  }
  // Adds a row in the first free slot; returns its index or −1.
  int addRow(const Row& r) {
    for (int i = 0; i < kRows; ++i)
      if (rows[i].src == SRC_NONE || rows[i].dst < 0) {
        rows[i] = r;
        anyRows = true;
        return i;
      }
    return -1;
  }
  void removeRow(int i) {
    if (i < 0 || i >= kRows) return;
    rows[i] = Row{};
    anyRows = false;
    for (const auto& r : rows) anyRows = anyRows || (r.src != SRC_NONE && r.dst >= 0);
  }

  double source(int s, int voice) const {
    switch (s) {
      case SRC_LFO1:
      case SRC_LFO2:
      case SRC_LFO3:
      case SRC_LFO4: return lfo[s - SRC_LFO1].value(voice);
      case SRC_RND: return rnd;
      case SRC_MODW: return modW;
      case SRC_AT: return at;
      default: break;
    }
    if (voice < 0 || voice >= kVoices) return 0.0;
    const VoiceSources& v = vs[voice];
    switch (s) {
      case SRC_ENV: return v.env;
      case SRC_PENV: return v.penv;
      case SRC_VEL: return v.vel;
      case SRC_ACC: return v.acc;
      case SRC_RNDHIT: return v.rndHit;
      case SRC_NOTE: return v.note;
      default: return 0.0;
    }
  }
  // m for one row at its destination: d·C(s)·via.
  double rowValue(const Row& r) const {
    if (!r.on || r.src == SRC_NONE || r.dst < 0) return 0.0;
    const int dstVoice = kParams[r.dst].voice;
    const int sv = perVoiceSource(r.src) ? (r.srcVoice >= 0 ? r.srcVoice : dstVoice) : dstVoice;
    const double s = source(r.src, sv);
    double via = 1.0;
    if (r.via != SRC_NONE) {
      const int vv = perVoiceSource(r.via) ? (r.viaVoice >= 0 ? r.viaVoice : dstVoice) : dstVoice;
      via = clampd(source(r.via, vv), 0.0, 1.0);
    }
    return r.depth * curve(r.curve, s) * via;
  }
  // Every 16 base samples: next ← Σ rows; per sample m interpolates prev → next (§8.4 evaluation rate).
  // Only destinations in `active` are touched (a removed destination glides back to 0 over one window, then drops).
  int active[kParamCount] = {};
  int nActive = 0;
  bool isActive[kParamCount] = {};
  void update() {
    for (int i = 0; i < nActive; ++i) {
      const int d = active[i];
      prev[d] = next[d];
      next[d] = 0.0;
    }
    for (const auto& r : rows) {
      if (!r.on || r.src == SRC_NONE || r.dst < 0 || !kParams[r.dst].mod) continue;
      if (!isActive[r.dst]) {
        isActive[r.dst] = true;
        active[nActive++] = r.dst;
        prev[r.dst] = next[r.dst] = 0.0;
      }
      next[r.dst] += rowValue(r);
    }
    int w = 0;
    for (int i = 0; i < nActive; ++i) {
      const int d = active[i];
      bool used = !dsp::exactEq(prev[d], 0.0) || !dsp::exactEq(next[d], 0.0);
      if (!used)
        for (const auto& r : rows) used = used || (r.on && r.dst == d && r.src != SRC_NONE);
      if (used) active[w++] = d;
      else isActive[d] = false;
    }
    nActive = w;
    anyRows = nActive > 0;
  }
  // m for parameter p at the current base sample.
  double offset(int p) const { return prev[p] + (next[p] - prev[p]) * (counter / 16.0); }
  // Advance the 16-sample interpolation clock; returns true when a fresh update is due.
  bool tickClock() {
    counter = (counter + 1) & 15;
    return counter == 0;
  }
};

}  // namespace mod
}  // namespace shogun
