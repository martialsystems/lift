#pragma once
// Common voice frame (§5). A voice produces its core signal at fsE = M·fs, one sub-sample per tick(). The engine
// applies DC block → VCA (g_vel · g_level · calib) → OUT tap → RET → choke → PAN → bus around it.

#include "../dsp.h"
#include "../params.h"
#include "../wave_shaper.h"

namespace shogun {

using namespace dsp;

// Per base sample: everything a voice reads, already summed (knob/automation smoothed + p-lock + mod + CV, §8.4).
struct VoiceCtx {
  double fs = 48000.0;
  double fsE = 96000.0;
  const double* ue = nullptr;  // effective u of every parameter, indexed by ParamId
  double pitchOct = 0.0;       // PITCH jack: AMT·V octaves (1 V/oct), drum f multiplier 2^pitchOct
  double toneV = 0.0;          // TONE jack: AMT·V volts (for the k_tone = 2^{V·2/5} laws)
  double decayV = 0.0;         // DECAY jack: AMT·V volts (RS τ law)
  double tolPitch = 1.0;       // 2^{(δp + drift)/1200}
  double tolTau = 1.0;         // 1 + δτ
  double tolCut = 1.0;         // 1 + δf
  int sub = 0;                 // sub-sample index 0..M−1 within this base sample (set by the engine per tick)
  const bool* moving = nullptr;     // per ParamId: smoothing ramp in progress (engine), null = none
  const bool* modulated = nullptr;  // per ParamId: a matrix row, CV jack or VEL term moves it this sample, null = none
};

// One hit (trigger) as the sequencer, a jack or MIDI delivers it.
struct HitInfo {
  double gVel = 1.0;     // velocity gain (§4.8), already scaled by ACCENT and VEL>LEVEL
  double velNorm = 1.0;  // VEL source (g_vel − 0.15)/0.85
  double acc = 0.5;      // ACC source 0 / 0.5 / 1
  double bend = 0.0;     // step bend, semitones
  double note = 48.0;    // lead/bass note
  bool tie = false;      // lead/bass glide without retrigger
};

struct Voice {
  virtual ~Voice() = default;
  virtual void prepare(double fs, double fsE) {
    fs_ = fs;
    fsE_ = fsE;
  }
  virtual void reset() = 0;
  virtual void trigger(const VoiceCtx& c, const HitInfo& h) = 0;
  // Called once per base sample before the M sub-sample ticks (coefficient updates that run at base rate).
  virtual void control(const VoiceCtx& c) { (void)c; }
  // One sub-sample at fsE. Writes the core (pre-VCA) signal. Stereo voices write L and R and return true.
  virtual bool tick(const VoiceCtx& c, double vc, double& L, double& R) = 0;
  virtual bool quiet() const = 0;
  virtual double env() const = 0;  // amplitude envelope 0..1 (ENV jack, ENV source)
  virtual double pitchEnv() const { return 0.0; }
  virtual double core() const { return 0.0; }  // last core sample (internal VC source for other voices)
  virtual double noiseSample() const { return 0.0; }
  virtual void choke(double tau) { (void)tau; }
  double fs_ = 48000.0, fsE_ = 96000.0;
};

// The WAVE block of a WAVE voice: parameters read from ue each base sample (§4.6).
// The plan's "steady" covers only the amount controls (jidai-common 1.1.2): WAVE macro, WAVE 1-3 trims and VC > AMT.
// SYM and VC > SYM have no effect on a stage whose amount is 0, so they never keep it running.
inline bool sameAmounts(const TripleShaperParams& a, const TripleShaperParams& b) {
  if (!(dsp::exactEq(a.macro, b.macro))) return false;
  for (int i = 0; i < 3; ++i)
    if (!(dsp::exactEq(a.trim[i], b.trim[i]) && dsp::exactEq(a.vcAmt[i], b.vcAmt[i]))) return false;
  return true;
}

struct WaveSlot {
  WaveShaper ts;
  WaveParams id{};
  bool preVca = false;
  double shape = 0.0;
  TripleShaperParams prev{};
  bool havePrev = false;
  void bind(int voice) { id = waveParams(voice); }
  void prepare(double fsE) { ts.prepare(fsE); }
  // Once per base sample: the parameters and the block plan (one base sample = one planning block, §4.6).
  void control(const VoiceCtx& c) {
    const double* ue = c.ue;
    TripleShaperParams p;
    p.macro = ue[id.macro];
    for (int i = 0; i < 3; ++i) {
      p.trim[i] = bip(ue[id.trim[i]]);
      p.sym[i] = bip(ue[id.sym[i]]);
      p.vcAmt[i] = bip(ue[id.vcAmt[i]]);
      p.vcSym[i] = bip(ue[id.vcSym[i]]);
    }
    p.shape = ue[id.shape];
    p.preVca = stepIndex(ue[id.routing], 2) == 1;
    p.levelComp = stepIndex(ue[id.levelComp], 2) == 1;
    preVca = p.preVca;
    shape = p.shape;
    // Steady: the same amount controls as the previous block (or the first block), none ramping or modulated.
    bool steady = !havePrev || sameAmounts(p, prev);
    const int ids[] = {id.macro, id.trim[0], id.trim[1], id.trim[2], id.vcAmt[0], id.vcAmt[1], id.vcAmt[2]};
    for (int k : ids)
      if ((c.moving && c.moving[k]) || (c.modulated && c.modulated[k])) steady = false;
    prev = p;
    havePrev = true;
    ts.setParams(p, steady, ue[id.vcLevel] > 0.0);  // VC is live when its level is above 0 (a source is always set)
  }
  void reset() {
    ts.reset();
    havePrev = false;
  }
  bool active() const { return !ts.bypassed(); }
};

// Pitch clamp for the drum PITCH jack: f_eff clamped to [0.25·f_min, 4·f_max] of the voice's tune range (§5).
inline double clampPitch(double f, double fMin, double fMax) { return clampd(f, 0.25 * fMin, 4.0 * fMax); }

}  // namespace shogun
