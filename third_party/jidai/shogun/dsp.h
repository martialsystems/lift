#pragma once

// SHOGUN v2.2 DSP blocks (spec SHOGUN_Redesign.md §3–§4). Framework-free, allocation-free, header-only.
// Every coefficient is computed from seconds or Hz and the block's own rate fsE (§3.1); no constant is in samples.
// States are double and are flushed below 1e-15 (§3.3, the RONIN rule; WASM has no FTZ).
// The triple wave shaper (§4.6) is the shared jidai/dsp/TripleShaper.h behind engine/wave_shaper.h.

#include "halfband_coeffs.h"

#include <jidai/dsp/Halfband.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

namespace shogun {
namespace dsp {

// Intentional exact comparison (caches, gates and bit-exact paths); std::equal_to keeps -Wfloat-equal builds quiet
// with the same result as ==.
inline bool exactEq(double a, double b) noexcept { return std::equal_to<double>{}(a, b); }
inline bool exactEq(float a, float b) noexcept { return std::equal_to<float>{}(a, b); }

constexpr double kPi = 3.14159265358979323846;
constexpr double kQuiet = 3.1622776601683795e-5;  // −90 dB voice end (§3.7)

// Bit-keyed memo of a pure function of one or more doubles: the stored value is f(key) itself, so a hit returns
// exactly what recomputing would (keys compare by bits, so -0/+0 and NaNs stay distinct). Used to skip libm calls
// whose inputs hold still between samples; the result is bit-identical to calling f every time.
inline std::uint64_t bitsOf(double x) {
  std::uint64_t b;
  std::memcpy(&b, &x, sizeof b);
  return b;
}
struct Memo1 {
  std::uint64_t k = 0;
  bool ok = false;
  double v = 0.0;
  template <class F>
  double operator()(double x, F f) {
    const std::uint64_t b = bitsOf(x);
    if (!ok || b != k) {
      k = b;
      ok = true;
      v = f(x);
    }
    return v;
  }
};
struct Memo2 {
  std::uint64_t k0 = 0, k1 = 0;
  bool ok = false;
  double v = 0.0;
  template <class F>
  double operator()(double x, double y, F f) {
    const std::uint64_t a = bitsOf(x), b = bitsOf(y);
    if (!ok || a != k0 || b != k1) {
      k0 = a;
      k1 = b;
      ok = true;
      v = f(x, y);
    }
    return v;
  }
};

inline double flushDenormal(double x) { return std::fabs(x) < 1e-15 ? 0.0 : x; }
inline double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
// One-pole / RC coefficient a = exp(−1/(τ·fs)) (§3.1).
inline double rcCoef(double tau, double fs) { return tau > 0.0 ? std::exp(-1.0 / (tau * fs)) : 0.0; }
// TPT prewarp g = tan(π·f/fsE), f clamped to [1 Hz, 0.45·fsE] (§3.1).
inline double prewarp(double f, double fsE) { return std::tan(kPi * clampd(f, 1.0, 0.45 * fsE) / fsE); }
// The KEPT drum decay law τ(u) = 8 ms·e^{4.5u} (§1.1).
inline double decayTau(double u) { return 0.008 * std::exp(4.5 * u); }

// ---------------------------------------------------------------- noise (§3.6)
struct XorShift32 {
  std::uint32_t s = 0xA341316Cu;
  void seed(std::uint32_t v) { s = v != 0 ? v : 0xA341316Cu; }
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  double uniform() { return static_cast<double>(next()) * (1.0 / 4294967296.0); }  // [0, 1)
  double bipolar() { return 2.0 * uniform() - 1.0; }                                // [−1, 1)
  // Three-uniform sum (u1+u2+u3−1.5)·2: variance exactly 1, bounded at ±3 (RONIN convention).
  double noise() { return (uniform() + uniform() + uniform() - 1.5) * 2.0; }
};
// Seed of voice v: 0xA341316C XOR (v+1)·0x9E3779B9 (§3.6).
inline std::uint32_t voiceSeed(int v) {
  return 0xA341316Cu ^ static_cast<std::uint32_t>(static_cast<std::uint32_t>(v + 1) * 0x9E3779B9u);
}

// ---------------------------------------------------------------- parameter smoother (§3.2)
struct Smoother {
  double a = 0.0;
  double s = 0.0;
  void prepare(double fs, double tau = 0.005) { a = rcCoef(tau, fs); }
  void reset(double x) { s = x; }
  double next(double x) {
    s = s + (1.0 - a) * (x - s);
    if (std::fabs(x - s) < 1e-12) s = x;
    return s;
  }
};

// ---------------------------------------------------------------- RC envelope (§4.1)
// tick() returns the current value, then advances: an instant trigger gives e[n] = a_d^n with e[0] = 1.
struct RcEnv {
  enum Stage { Idle, Attack, Decay };
  double e = 0.0;
  double aA = 0.0;  // 0: instant (e = 1 at the trigger)
  double aD = 0.0;
  double S = 0.0;   // hold/sustain level (BD2 and tom holds)
  Stage stage = Idle;
  Memo2 mA_, mD_;  // rcCoef memos (the coefficient is still assigned every call, so discharge() cannot go stale)
  void setAttack(double tau, double fs) { aA = mA_(tau, fs, rcCoef); }
  void setDecay(double tau, double fs) { aD = mD_(tau, fs, rcCoef); }
  void setSustain(double s) { S = s; }
  // Recharges from the present value (no reset): RONIN S-15 edge rule (§3.8).
  void trigger() {
    if (aA <= 0.0) {
      e = 1.0;
      stage = Decay;
    } else {
      stage = Attack;
    }
  }
  // Choke / discharge with its own time constant toward 0 (hat choke, §6.6).
  void discharge(double tau, double fs) {
    aD = rcCoef(tau, fs);
    S = 0.0;
    if (stage != Idle) stage = Decay;
  }
  double tick() {
    const double out = e;
    if (stage == Attack) {
      e += (1.0 - aA) * (1.2 - e);  // overshoot target T = 1.2
      if (e >= 1.0) {
        e = 1.0;
        stage = Decay;
      }
    } else if (stage == Decay) {
      e = S + aD * (e - S);
      e = flushDenormal(e);
    }
    return out;
  }
  bool quiet() const { return stage == Idle || (stage == Decay && e < kQuiet && S < kQuiet); }
  void reset() {
    e = 0.0;
    stage = Idle;
    S = 0.0;
  }
};

// ---------------------------------------------------------------- TPT one-pole (§4.2)
struct TptOnePole {
  double s = 0.0;
  double G = 0.0;
  Memo2 mG_;
  void set(double f, double fsE) {
    const double g = mG_(f, fsE, prewarp);
    G = g / (1.0 + g);
  }
  double lp(double x) {
    const double v = (x - s) * G;
    const double y = v + s;
    s = flushDenormal(y + v);
    return y;
  }
  double hp(double x) { return x - lp(x); }
  void reset() { s = 0.0; }
};

// ---------------------------------------------------------------- TPT state-variable filter (§4.3)
struct TptSvf {
  double s1 = 0.0, s2 = 0.0;
  double g = 0.1, R = 0.7071067811865476, D = 1.0;
  double hp = 0.0, bp = 0.0, lp = 0.0;
  void setG(double gg, double RR) {
    g = gg;
    R = RR;
    D = 1.0 / (1.0 + 2.0 * R * g + g * g);
  }
  Memo2 mG_;
  void set(double f, double RR, double fsE) { setG(mG_(f, fsE, prewarp), RR); }
  void tick(double x) {
    hp = (x - (2.0 * R + g) * s1 - s2) * D;
    bp = g * hp + s1;
    lp = g * bp + s2;
    s1 = flushDenormal(2.0 * bp - s1);
    s2 = flushDenormal(2.0 * lp - s2);
  }
  double bpNorm() const { return 2.0 * R * bp; }  // unity at the centre
  void reset() { s1 = s2 = hp = bp = lp = 0.0; }
};

// ---------------------------------------------------------------- bridged-T resonator (§4.4)
// SVF with R solved so that the pole radius equals the decay time constant exactly at any f and fsE.
// The kick E = A·sqrt(1−R²)/(2g) enters as the SVF input on the next tick (the form verify_dsp.py measures:
// peak ≈ 0.97·A, sine-phase start). Retrigger adds energy into the ringing state; nothing is reset (§3.8).
struct Resonator {
  TptSvf svf;
  double pending = 0.0;
  // set() runs every sub-sample during a pitch sweep: g and R are memoised on the exact (f, tau, fsE) bits.
  std::uint64_t mk_[3] = {0, 0, 0};
  bool mok_ = false;
  double mg_ = 0.0, mR_ = 0.0;
  void set(double f, double tau, double fsE) {
    const std::uint64_t k0 = bitsOf(f), k1 = bitsOf(tau), k2 = bitsOf(fsE);
    if (!mok_ || k0 != mk_[0] || k1 != mk_[1] || k2 != mk_[2]) {
      mk_[0] = k0;
      mk_[1] = k1;
      mk_[2] = k2;
      mok_ = true;
      const double g = prewarp(f, fsE);
      const double r2 = std::exp(-2.0 / (tau * fsE));
      double R = (1.0 + g * g) * (1.0 - r2) / (2.0 * g * (1.0 + r2));
      if (R > 4.0) R = 4.0;  // overdamped clamp
      mg_ = g;
      mR_ = R;
    }
    svf.setG(mg_, mR_);
  }
  void kick(double A) {
    const double Rk = svf.R < 0.999 ? svf.R : 0.999;
    pending += A * std::sqrt(1.0 - Rk * Rk) / (2.0 * svf.g);
  }
  double tick() {
    const double x = pending;
    pending = 0.0;
    svf.tick(x);
    return svf.lp;
  }
  double lp() const { return svf.lp; }
  double bp() const { return svf.bp; }
  // Quadrature amplitude squared (lp² + bp²): the resonator-energy end test of §3.7.
  double energy() const { return svf.lp * svf.lp + svf.bp * svf.bp; }
  void reset() {
    svf.reset();
    pending = 0.0;
  }
};

// ---------------------------------------------------------------- PolyBLEP / PolyBLAMP (§3.4)
inline double polyBlep(double t, double dt) {
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
inline double polyBlamp(double t, double dt) {
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
inline double wrap01(double x) { return x - std::floor(x); }

struct PolyBlepOsc {
  double ph = 0.0;
  double dt = 0.0;
  void setFreq(double f, double fsE) { dt = clampd(f / fsE, 0.0, 0.45); }
  void reset(double p = 0.0) { ph = p; }
  double saw() const { return 2.0 * ph - 1.0 - polyBlep(ph, dt); }
  // Square from two saws: saw(φ) − saw(φ−D). Levels 2D−2 and 2D, zero mean (DC removed by construction).
  double pulse(double D) const {
    const double t2 = wrap01(ph - D);
    return (2.0 * ph - 1.0 - polyBlep(ph, dt)) - (2.0 * t2 - 1.0 - polyBlep(t2, dt));
  }
  // Triangle aligned with sin(2πφ) (zero at φ = 0, peak at φ = 0.25), PolyBLAMP at both corners.
  double tri() const { return triAt(ph, dt); }
  static double triAt(double t, double dt) {
    const double u = wrap01(t + 0.25);
    return (1.0 - 4.0 * std::fabs(u - 0.5)) + 4.0 * dt * (polyBlamp(u, dt) - polyBlamp(wrap01(u + 0.5), dt));
  }
  // Saw aligned with sin(2πφ) (zero crossing rising at φ = 0, wrap at φ = 0.5), PolyBLEP at the wrap.
  static double sawAlignedAt(double t, double dt) {
    const double v = wrap01(t + 0.5);
    return 2.0 * v - 1.0 - polyBlep(v, dt);
  }
  void advance() {
    ph += dt;
    if (ph >= 1.0) ph -= 1.0;
  }
};

// ---------------------------------------------------------------- Schmitt metal bank (§4.5)
struct SchmittBank {
  static constexpr double kBase[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
  static constexpr double kDuty = 0.4798;
  PolyBlepOsc osc[6];
  double tol[6] = {1, 1, 1, 1, 1, 1};  // (1 + δm_k), frozen per unit (§3.5)
  void reset() {
    for (auto& o : osc) o.reset(0.0);
  }
  // Free-running, never reset by a trigger. m = (1/6)·Σ pulse_k.
  double tick(double scale, double fsE) {
    double m = 0.0;
    for (int k = 0; k < 6; ++k) {
      osc[k].setFreq(kBase[k] * scale * tol[k], fsE);
      m += osc[k].pulse(kDuty);
      osc[k].advance();
    }
    return m / 6.0;
  }
};

// ---------------------------------------------------------------- saturation, ADAA tanh (§4.7)
inline double logCosh(double x) {
  const double a = std::fabs(x);
  return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994530942;
}
// y = tanh(d·x)/tanh(d), first-order ADAA with F = ln cosh(d·x)/(d·tanh d). d ≤ 0: true bypass (y = x).
struct TanhAdaa {
  double xp = 0.0;
  void reset() { xp = 0.0; }
  double tick(double x, double d) {
    if (d <= 0.0) {
      xp = x;
      return x;
    }
    const double td = std::tanh(d);
    const double diff = x - xp;
    double y;
    if (d < 1e-2) {
      // Tiny drive: the ADAA difference quotient (logCosh(d·x) − logCosh(d·xp)) / (d·tanh d·Δx) cancels to noise
      // over ~d² and blew up (+191 dBFS on a BD1 DRIVE glide to 0). Here the curve is a straight line to 1e-4, so
      // the plain tanh(d·x)/tanh(d) needs no anti-aliasing.
      y = std::tanh(d * x) / td;
    } else if (std::fabs(diff) < 1e-5) {
      y = std::tanh(d * 0.5 * (x + xp)) / td;
    } else {
      y = (logCosh(d * x) - logCosh(d * xp)) / (d * td * diff);
    }
    xp = x;
    return y;
  }
};

// ---------------------------------------------------------------- ZDF 4-pole ladder (§4.9)
struct ZdfLadder {
  double s[4] = {0, 0, 0, 0};
  double y4p = 0.0;
  double g = 0.1, G = 0.1 / 1.1;
  int lastIters = 0;
  Memo2 mG_;
  void set(double fc, double fsE) {
    g = mG_(fc, fsE, [](double f, double fs) { return std::tan(kPi * clampd(f, 20.0, 0.45 * fs) / fs); });
    G = g / (1.0 + g);
  }
  double tick(double x, double k, double drive = 1.0) {
    const double inv = 1.0 / (1.0 + g);
    const double G2 = G * G, G3 = G2 * G, G4 = G3 * G;
    const double S = G3 * s[0] * inv + G2 * s[1] * inv + G * s[2] * inv + s[3] * inv;
    double y = y4p;
    int it = 0;
    for (; it < 4; ++it) {
      const double t = std::tanh(drive * (x - k * y));
      const double f = y - G4 * t / drive - S;
      const double fp = 1.0 + k * G4 * (1.0 - t * t);
      const double dy = f / fp;
      y -= dy;
      if (std::fabs(dy) < 1e-9) {
        ++it;
        break;
      }
    }
    lastIters = it;
    double in = std::tanh(drive * (x - k * y)) / drive;
    for (int i = 0; i < 4; ++i) {
      const double v = (in - s[i]) * G;
      const double yi = v + s[i];
      s[i] = flushDenormal(yi + v);
      in = yi;
    }
    y4p = in;
    return in;
  }
  void reset() {
    for (double& v : s) v = 0.0;
    y4p = 0.0;
  }
};

// ---------------------------------------------------------------- OTA-style SVF (§4.10)
struct OtaSvf {
  TptSvf svf;
  double V = 2.5;
  double tick(double x) {
    svf.tick(x);
    svf.s1 = V * std::tanh(svf.s1 / V);  // saturating integrator: OTA current limit
    return svf.lp;
  }
  void reset() { svf.reset(); }
};

// ---------------------------------------------------------------- decimators / upsamplers (§3.4)
// Linear-phase FIRs from scripts/gen_halfband.py. One decimator per audio output; latency 0 / 23 / 26 base samples.
template <int N>
struct FirLine {
  double buf[static_cast<std::size_t>(2 * N)] = {};
  int w = 0;
  void push(double x) {
    buf[w] = x;
    buf[w + N] = x;
    w = (w + 1 == N) ? 0 : w + 1;
  }
  // Σ h[k]·x[n−k], newest first.
  double dot(const double* h) const {
    const double* p = buf + w + N - 1;
    double y = 0.0;
    for (int k = 0; k < N; ++k) y += h[k] * p[-k];
    return y;
  }
  void reset() {
    for (double& v : buf) v = 0.0;
    w = 0;
  }
};

inline int osLatency(int M) { return M <= 1 ? 0 : (M == 2 ? 23 : 26); }

// Stage 1 (2fs ↔ fs) is the shared exact halfband (jidai/dsp/Halfband.h, 93 taps, 23 base samples per direction);
// stage 2 (4fs ↔ 2fs, 25 taps, +3 base samples) is SHOGUN's own (halfband_coeffs.h; the shared header has no 4x stage).
static_assert(jidai::dsp::Halfband93::kTaps == 93 && jidai::dsp::Halfband93::kLatencyPerDirection == 23,
              "§3.4 stage 1: 93 taps, 23 base samples");

class Decimator {
 public:
  void prepare(int M) {
    M_ = (M == 4) ? 4 : (M == 2 ? 2 : 1);
    reset();
  }
  void reset() {
    s1_.reset();
    s2_.reset();
    phase_ = 0;
    phase2_ = 0;
    u0_ = 0.0;
    out_ = 0.0;
    zeroRun_ = kSilentRun;
  }
  // Every stored sample is +0 (bit pattern 0): the last kSilentRun pushes were +0, enough to refill both stages
  // (2 × 92 stage-1 entries at 4x, after the 25-tap stage 2), or nothing non-zero was pushed since reset(). Pushing
  // +0 into a silent decimator returns +0 and leaves it silent; only the ring positions move (the engine's sleep).
  static constexpr int kSilentRun = 2 * 92 + hb::kStage2Taps + 4;
  bool silent() const { return zeroRun_ >= kSilentRun && bitsOf(out_) == 0 && bitsOf(u0_) == 0; }
  // Push one sample at M·fs. Returns true (and sets out) on the push that completes a base-rate output: the last
  // sub-sample of the base sample. The output is y[n] = Σ h[k]·u[2n−k] (centred on the FIRST sub-sample of base
  // sample n − latency), so the delay is an integer number of base samples.
  bool push(double x, double& out) {
    if (bitsOf(x) == 0) {
      if (zeroRun_ < kSilentRun) ++zeroRun_;
    } else {
      zeroRun_ = 0;
    }
    if (M_ == 1) {
      out = x;
      return true;
    }
    if (M_ == 4) {
      s2_.push(x);
      const bool even4 = (phase2_ == 0);
      phase2_ ^= 1;
      if (!even4) return false;
      x = s2_.dot(hb::kStage2);
    }
    const bool even2 = (phase_ == 0);
    phase_ ^= 1;
    if (even2) {
      u0_ = x;
      return false;
    }
    out_ = s1_.process(u0_, x);
    out = out_;
    return true;
  }
  int factor() const { return M_; }

 private:
  int M_ = 1;
  jidai::dsp::Downsampler2x s1_;
  FirLine<hb::kStage2Taps> s2_;
  int phase_ = 0;
  int phase2_ = 0;
  double u0_ = 0.0;
  double out_ = 0.0;
  int zeroRun_ = kSilentRun;
};

// RET-style upsampler: base rate in, M samples out. +23 (2×) / +26 (4×).
class Upsampler {
 public:
  void prepare(int M) {
    M_ = (M == 4) ? 4 : (M == 2 ? 2 : 1);
    reset();
  }
  void reset() {
    s1_.reset();
    s2_.reset();
  }
  void push(double x, double* out) {
    if (M_ == 1) {
      out[0] = x;
      return;
    }
    double y2[2];
    s1_.process(x, y2[0], y2[1]);  // even phase = x[n − 23] exactly, odd phase interpolated
    if (M_ == 2) {
      out[0] = y2[0];
      out[1] = y2[1];
      return;
    }
    for (int i = 0; i < 2; ++i) {  // stage 2: zero-stuff ×2, gain 2
      s2_.push(2.0 * y2[i]);
      out[2 * i] = s2_.dot(hb::kStage2);
      s2_.push(0.0);
      out[2 * i + 1] = s2_.dot(hb::kStage2);
    }
  }

 private:
  int M_ = 1;
  jidai::dsp::Upsampler2x s1_;
  FirLine<hb::kStage2Taps> s2_;
};

// ---------------------------------------------------------------- analog drift, OU process (§3.5)
// Updated at fr = fs/32 and linearly interpolated per base sample. Value in cents.
struct OuDrift {
  double rho = 0.0, sigma = 0.0, d0 = 0.0, d1 = 0.0;
  int count = 0;
  static constexpr int kDiv = 32;
  XorShift32 rng;
  void prepare(double fs, double tauD = 1.5) { rho = std::exp(-1.0 / (tauD * fs / kDiv)); }
  void setSigma(double cents) { sigma = cents; }
  double update() { return rho * d1 + sigma * std::sqrt(1.0 - rho * rho) * rng.noise(); }
  double tick() {
    if (count == 0) {
      d0 = d1;
      d1 = update();
    }
    const double t = static_cast<double>(count) / kDiv;
    count = (count + 1) % kDiv;
    return d0 + (d1 - d0) * t;
  }
  void reset() {
    d0 = d1 = 0.0;
    count = 0;
  }
};

// ---------------------------------------------------------------- pan law (§4.11, KEPT)
inline double panL(double p) { return std::cos(kPi * 0.25 * (1.0 + p)); }
inline double panR(double p) { return std::sin(kPi * 0.25 * (1.0 + p)); }

// ---------------------------------------------------------------- VCA laws (§4.8)
inline double gVelFromVolts(double v) { return 0.15 + 0.85 * clampd(v / 5.0, 0.0, 1.0); }
inline double gLevel(double u) { return 1.4125375446227544 * u * u; }  // +3 dB at full, −9 dB at noon
constexpr double kAccentVolts[3] = {2.3529411764705883, 3.7058823529411766, 5.0};  // g_vel 0.55 / 0.78 / 1.00

}  // namespace dsp
}  // namespace shogun
