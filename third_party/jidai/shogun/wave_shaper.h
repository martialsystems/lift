#pragma once
// SHOGUN WAVE block (spec v2.2 §4.6) on the shared jidai::dsp::TripleShaper (third_party/jidai-common, see
// VENDORED.md). The stage law, ADAA stages, macro stagger, ShaperControls, LevelComp and DcBlocker are the shared
// ones. Only SHOGUN-specific pieces live here:
//   - TripleShaperParams / setParams(): the voice-facing parameter set (incl. SHAPE, ROUTING, LEVEL COMP switch);
//   - SHAPE morph from resonator quadrature or an oscillator phase (shapeMorph, morphAt, quadAmp);
//   - PRE-VCA input normalisation (preVcaInput);
//   - the exact v2.1 single-stage migration (migrateV21, v21Law).
//
// Stage skipping is the shared per-block plan (jidai-common 1.1.2 TripleShaper::planBlock): a stage is a wire for a
// block when its amount is 0 for the whole block, at ANY SYM (the stage law is y = x at a = 0 for every b): the block's
// amount controls (WAVE macro, WAVE 1-3 trims, VC > AMT depths) are steady (no smoothing ramp, no modulation, no
// control change), its a is exactly 0, and no live VC has AMT depth on it. SYM and VC > SYM never keep a stage running.
// A stage whose a merely passes through 0 (VC, a ramp, a matrix row) keeps its ADAA, so the half-sample delay never
// toggles at audio rate (spec §4.6 / verify_folder.py: skip only if 0 over the whole render). SHOGUN reads its
// parameters once per base sample, so its block is one base sample (M samples at fsEff): setParams() plans it. The
// caller says whether the amount controls are steady and whether a VC source is live.
// Bypass (amount 0, any SYM): WAVE 0 with the WAVE 1-3 trims at 0, no live VC > AMT depth and SHAPE at 0 returns the
// input bit for bit at any SYM, SYM matrix row or VC > SYM depth. Every stage is a wire (histories kept), and the
// LEVEL COMP block is skipped whole: its 8 Hz detector DC block is not fed and LevelComp::track() is not called
// (§4.6 "LEVEL COMP skipped"), so nothing after the stages can colour a bypassed voice. Before 1.1.2 a SYM other than
// 0 at amount 0 ran ADAA on a straight line plus LEVEL COMP: half a sample late and duller treble.
// LEVEL COMP is the shared LevelComp fed by a shared 8 Hz DcBlocker on the wet path. Its detector floor is 1e-30 on
// both powers (documented upstream); the pre-1.1 local copy floored only the wet power, at 1e-12.

#include <jidai/dsp/TripleShaper.h>

#include <cmath>

namespace shogun {

struct TripleShaperParams {
  double macro = 0.0;                 // front WAVE knob m ∈ [0,1], after the mod/CV sum
  double trim[3] = {0.0, 0.0, 0.0};   // WAVE 1/2/3 per-stage amount trims ∈ [−1,1] (incl. matrix rows)
  double sym[3] = {0.0, 0.0, 0.0};    // SYM 1/2/3 ∈ [−1,1] (incl. matrix rows)
  double vcAmt[3] = {0.0, 0.0, 0.0};  // VC → AMT depth dA_i ∈ [−1,1]
  double vcSym[3] = {0.0, 0.0, 0.0};  // VC → SYM depth dB_i ∈ [−1,1]
  double shape = 0.0;                 // SHAPE ∈ [0,1], sine → triangle → saw (applied with shapeMorph before process)
  bool preVca = false;                // ROUTING: false = POST, true = PRE-VCA (caller applies preVcaInput)
  bool levelComp = true;              // LEVEL COMP: ON for new patches, OFF for migrated ones
};

class WaveShaper {
 public:
  static constexpr double kPi = jidai::dsp::kPi;
  static constexpr const double (&kK)[3] = jidai::dsp::kShaperK;

  void prepare(double fsEff) {
    comp_.prepare(fsEff);
    dc_.prepare(fsEff, 8.0);  // 8 Hz DC block on the wet detector path
    reset();
  }
  void reset() {
    st_.reset();
    comp_.reset();
    dc_.reset();
    plan();
  }

  // Parameters for the next block (one base sample in the engine) and that block's plan.
  // steady: the amount controls (macro, trims, VC > AMT depths) did not move since the previous block and none is
  //         ramping or modulated; SYM may move (a static render, e.g. every test that sets the parameters once, is
  //         steady).
  // vcLive: a VC source contributes (FOLD VC jack or internal source at a non-zero level).
  void setParams(const TripleShaperParams& p, bool steady = true, bool vcLive = true) {
    p_ = p;
    steady_ = steady;
    vcLive_ = vcLive;
    ctl_ = jidai::dsp::ShaperControls{};
    ctl_.macro = p.macro;
    for (int i = 0; i < 3; ++i) {
      ctl_.trim[i] = p.trim[i];
      ctl_.sym[i] = p.sym[i];
      ctl_.vcToAmt[i] = p.vcAmt[i];
      ctl_.vcToSym[i] = p.vcSym[i];
    }
    // Amount 0, any SYM (jidai-common 1.1.2 isBypass ignores SYM, matrix SYM and VC > SYM): the stages, the detector
    // DC block and LEVEL COMP are all skipped. SHAPE > 0 changes the body before the stages, so it is never a bypass.
    bypass_ = !(p.shape > 0.0) && ctl_.isBypass(vcLive);
    plan();
  }
  const TripleShaperParams& params() const { return p_; }
  const jidai::dsp::ShaperControls& controls() const { return ctl_; }

  bool bypassed() const { return bypass_; }

  // One sample at fsEff. x: body (after SHAPE / PRE-VCA), vc: audio-rate VC (unsmoothed, V/5).
  double process(double x, double vc = 0.0) {
    if (bypass_) {
      st_.process(x, kWireCtl, 0.0);  // every stage planned as a wire: records x[n-1] = x
      return x;
    }
    const double in = x;
    const double y = st_.process(x, ctl_, vc);
    if (!p_.levelComp) return y;
    // LEVEL COMP: C = sqrt(<x²>/<y²>), clamped to ±12 dB, smoothed 5 ms; the wet detector reads y after an 8 Hz DC
    // block (the asymmetric DC from SYM is removed by the voice's own DC block after the chain).
    const double yac = dc_.process(y);
    return y * comp_.gainFor(in * in, yac * yac);
  }

  bool stageIsWire(int i) const { return st_.stageIsWire(i); }
  double stageA(int i, double vc = 0.0) const {
    return clamp(macroStage(i, p_.macro) + p_.trim[i] + vc * p_.vcAmt[i], 0.0, 1.0);
  }
  double stageB(int i, double vc = 0.0) const { return clamp(p_.sym[i] + vc * p_.vcSym[i], -1.0, 1.0); }
  double levelGain() const { return comp_.gain(); }

  // ---------------------------------------------------------------- pure math (shared law)
  static double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
  static double macroStage(int i, double m) {
    double c[3];
    jidai::dsp::macroAmounts(m, c);
    return c[i];
  }
  static double stage(double x, double a, double b, double K) { return jidai::dsp::ShaperStage::f(x, a, b, K); }
  static double stageAntiderivative(double x, double a, double b, double K) {
    return jidai::dsp::ShaperStage::F(x, a, b, K);
  }
  // The static (non-ADAA) chain, for migration checks and reference renders.
  static double chainStatic(double x, const TripleShaperParams& p, double vc = 0.0) {
    for (int i = 0; i < 3; ++i) {
      const double a = clamp(macroStage(i, p.macro) + p.trim[i] + vc * p.vcAmt[i], 0.0, 1.0);
      const double b = clamp(p.sym[i] + vc * p.vcSym[i], -1.0, 1.0);
      if (dsp::exactEq(a, 0.0) && dsp::exactEq(b, 0.0)) continue;
      x = stage(x, a, b, kK[i]);
    }
    return x;
  }

  // ---------------------------------------------------------------- SHOGUN-only: SHAPE, PRE-VCA, migration
  // SHAPE from a resonator's quadrature states (lp, bp, damping R): sine → triangle → saw, phase and amplitude
  // following the body exactly. dt = f/fsEff for the PolyBLEP/PolyBLAMP corrections. SHAPE 0 returns lp exactly.
  // The quadrature term is (bp + R·lp)/sqrt(1 − R²), which removes the damping-induced amplitude ripple.
  static double shapeMorph(double lp, double bp, double R, double shape, double dt) {
    if (shape <= 0.0) return lp;
    const double Rc = R < 0.99 ? R : 0.99;
    const double q = (bp + Rc * lp) / std::sqrt(1.0 - Rc * Rc);
    const double r = std::sqrt(lp * lp + q * q);
    const double phase = std::atan2(lp, q) / (2.0 * kPi);
    return morphAt(lp, r, phase, shape, dt);
  }
  // Same, from an oscillator phase (BD2 uses its VCO phase directly; r = 1, sine = its own sine).
  static double morphAt(double sine, double r, double phase, double shape, double dt) {
    if (shape <= 0.0) return sine;
    const double t = phase - std::floor(phase);
    const double tri = r * triAt(t, dt);
    if (shape <= 0.5) return (1.0 - 2.0 * shape) * sine + 2.0 * shape * tri;
    const double saw = r * sawAt(t, dt);
    return (2.0 - 2.0 * shape) * tri + (2.0 * shape - 1.0) * saw;
  }
  // Quadrature amplitude r = sqrt(lp² + q²) (PRE-VCA normalisation).
  static double quadAmp(double lp, double bp, double R) {
    const double Rc = R < 0.99 ? R : 0.99;
    const double q = (bp + Rc * lp) / std::sqrt(1.0 - Rc * Rc);
    return std::sqrt(lp * lp + q * q);
  }
  // PRE-VCA: x = (body'/max(r, 1e-6))·env.
  static double preVcaInput(double body, double r, double env) { return body / (r > 1e-6 ? r : 1e-6) * env; }

  // Exact migration from the v2.1 / sim single-stage WAVE w = v/127: m = w/2, WAVE 2 = −(w − 0.5) for w > 0.5,
  // SYM 0, SHAPE 0, LEVEL COMP OFF, ROUTING POST (PRE-VCA for BD2).
  static TripleShaperParams migrateV21(double w, bool bd2 = false) {
    TripleShaperParams p;
    p.macro = 0.5 * w;
    p.trim[1] = -macroStage(1, p.macro);
    p.levelComp = false;
    p.preVca = bd2;
    return p;
  }
  // The v2.1 single-stage law: (1 − w)·x + w·sin(π/2·(1 + 4w)·x).
  static double v21Law(double x, double w) { return (1.0 - w) * x + w * std::sin(0.5 * kPi * (1.0 + 4.0 * w) * x); }

 private:
  // The block plan: in bypass every stage is a wire; otherwise the shared planBlock decides.
  void plan() {
    if (bypass_) st_.planBlock(kWireCtl, true, false);
    else st_.planBlock(ctl_, steady_, vcLive_);
  }
  static double polyBlep(double t, double dt) {
    if (t < dt) {
      t /= dt;
      return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
      t = (t - 1.0) / dt;
      return t * t + t + t + 1.0;
    }
    return 0.0;
  }
  static double polyBlamp(double t, double dt) {
    if (t < dt) {
      t = t / dt - 1.0;
      return -t * t * t / 3.0;
    }
    if (t > 1.0 - dt) {
      t = (t - 1.0) / dt + 1.0;
      return t * t * t / 3.0;
    }
    return 0.0;
  }
  static double triAt(double t, double dt) {
    double u = t + 0.25;
    u -= std::floor(u);
    double u2 = u + 0.5;
    u2 -= std::floor(u2);
    return (1.0 - 4.0 * std::fabs(u - 0.5)) + 4.0 * dt * (polyBlamp(u, dt) - polyBlamp(u2, dt));
  }
  static double sawAt(double t, double dt) {
    double v = t + 0.5;
    v -= std::floor(v);
    return 2.0 * v - 1.0 - polyBlep(v, dt);
  }

  static inline const jidai::dsp::ShaperControls kWireCtl{};
  TripleShaperParams p_{};
  jidai::dsp::ShaperControls ctl_{};
  jidai::dsp::TripleShaper st_;
  jidai::dsp::LevelComp comp_;
  jidai::dsp::DcBlocker dc_;
  bool steady_ = true, vcLive_ = true;
  bool bypass_ = true;
};

}  // namespace shogun
