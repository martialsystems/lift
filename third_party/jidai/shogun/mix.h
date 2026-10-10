#pragma once
// Mixer, buses A–D, master and output stage (§9). Everything here runs in the oversampled domain (fsE) except the
// delay, which runs at the base rate on a decimated send (see Engine).

#include <algorithm>
#include <cstddef>
#include <vector>

#include "dsp.h"

namespace shogun {
namespace mix {

using namespace dsp;

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// Feed-forward log-domain compressor with a 6 dB soft knee (Giannoulis, Massberg, Reiss, JAES 60(6), 2012).
struct Compressor {
  double yL = 0.0, aA = 0.0, aR = 0.0, T = 0.0, ratio = 2.0, W = 6.0, makeup = 0.0;
  void set(double threshDb, double r, double attack, double release, double makeupDb, double fs) {
    T = threshDb;
    ratio = r < 1.0 ? 1.0 : r;
    aA = rcCoef(attack, fs);
    aR = rcCoef(release, fs);
    makeup = makeupDb;
  }
  double staticCurve(double xDb) const {
    const double d = 2.0 * (xDb - T);
    if (d < -W) return xDb;
    if (std::fabs(d) <= W) {
      const double t = xDb - T + W / 2.0;
      return xDb + (1.0 / ratio - 1.0) * t * t / (2.0 * W);
    }
    return T + (xDb - T) / ratio;
  }
  // Returns the linear gain for a stereo-linked peak detector value.
  double gain(double peak) {
    const double xDb = 20.0 * std::log10(peak + 1e-12);
    const double gc = staticCurve(xDb) - xDb;
    if (gc < yL) yL = aA * yL + (1.0 - aA) * gc;
    else yL = aR * yL + (1.0 - aR) * gc;
    yL = flushDenormal(yL);
    return dbToGain(yL + makeup);
  }
  void reset() { yL = 0.0; }
};

// DRIVE (§9.2): y = tanh(G·x)/sqrt(G), G = 10^{24u/20}; ADAA; u = 0 bypass.
struct Drive {
  TanhAdaa l, r;
  double G = 1.0;
  bool on = false;
  void set(double u) {
    on = u > 0.0;
    G = dbToGain(24.0 * u);
  }
  void process(double& L, double& R) {
    if (!on) {
      l.xp = L;
      r.xp = R;
      return;
    }
    // tanh(G·x)/sqrt(G) = [tanh(G·x)/tanh(G)]·tanh(G)/sqrt(G)
    const double k = std::tanh(G) / std::sqrt(G);
    L = l.tick(L, G) * k;
    R = r.tick(R, G) * k;
  }
  void reset() {
    l.reset();
    r.reset();
  }
};

// TONE: tilt at 800 Hz, ±6 dB (t = 2u − 1).
struct Tilt {
  TptOnePole l, r;
  double gLo = 1.0, gHi = 1.0;
  bool on = false;
  void prepare(double fsE) {
    l.set(800.0, fsE);
    r.set(800.0, fsE);
  }
  void set(double u) {
    const double t = 2.0 * u - 1.0;
    on = std::fabs(t) > 1e-9;
    gLo = dbToGain(-6.0 * t);
    gHi = dbToGain(6.0 * t);
  }
  void process(double& L, double& R) {
    const double lpL = l.lp(L), lpR = r.lp(R);
    if (!on) return;
    L = lpL * gLo + (L - lpL) * gHi;
    R = lpR * gLo + (R - lpR) * gHi;
  }
  void reset() {
    l.reset();
    r.reset();
  }
};

// One bus A–D: DRIVE → TONE → COMPRESSOR → LEVEL (§9.2).
struct Bus {
  Drive drive;
  Tilt tilt;
  Compressor comp;
  double level = 1.0, mix = 1.0, lastGain = 1.0;
  bool compOn = false;
  void prepare(double fsE) {
    tilt.prepare(fsE);
    reset();
  }
  void reset() {
    drive.reset();
    tilt.reset();
    comp.reset();
    lastGain = 1.0;
  }
  // sc: optional sidechain peak (a voice); < 0 = use the bus itself.
  void process(double& L, double& R, double sc) {
    drive.process(L, R);
    tilt.process(L, R);
    if (compOn) {
      const double peak = sc >= 0.0 ? sc : std::fmax(std::fabs(L), std::fabs(R));
      const double g = comp.gain(peak);
      lastGain = g;
      L = mix * L * g + (1.0 - mix) * L;
      R = mix * R * g + (1.0 - mix) * R;
    }
    L *= level;
    R *= level;
  }
};

// Master WIDTH (§9.3): M/S with w ∈ [0, 2]; S below 120 Hz never widens (kept mono-compatible). w = 1 is identity.
struct Width {
  TptOnePole sLp;
  double w = 1.0;
  void prepare(double fsE) { sLp.set(120.0, fsE); }
  void process(double& L, double& R) {
    const double M = 0.5 * (L + R), S = 0.5 * (L - R);
    const double lo = sLp.lp(S);
    if (dsp::exactEq(w, 1.0)) return;
    const double S2 = lo * (w < 1.0 ? w : 1.0) + (S - lo) * w;
    L = M + S2;
    R = M - S2;
  }
  void reset() { sLp.reset(); }
};

// CEILING/CLIP (§9.3.6): y = C·tanh(x/C) with ADAA when on; off passes untouched. OVER when |x| > C.
struct Clip {
  TanhAdaa l, r;
  double C = 1.0;
  bool on = false, over = false;
  void process(double& L, double& R) {
    if (std::fabs(L) > C || std::fabs(R) > C) over = true;
    if (!on) return;
    // C·tanh(x/C) = C·tanh(1)·[tanh(1·(x/C))/tanh(1)]
    const double k = C * std::tanh(1.0);
    L = l.tick(L / C, 1.0) * k;
    R = r.tick(R / C, 1.0) * k;
  }
  void reset() {
    l.reset();
    r.reset();
    over = false;
  }
};

// Stereo tempo delay at the base rate (§9.3.4; the room is deferred). The wet path's decimator + upsampler latency
// (2·L base samples) is subtracted from the delay time so the echoes land exactly on the beat.
struct Delay {
  std::vector<float> bufL, bufR;
  int w = 0, size = 1;
  double fb = 0.35, time = 0.0;
  TptOnePole lpL, lpR;
  void prepare(double fs, double maxSeconds) {
    size = static_cast<int>(fs * maxSeconds) + 4;
    bufL.assign(static_cast<size_t>(size), 0.0f);
    bufR.assign(static_cast<size_t>(size), 0.0f);
    w = 0;
  }
  void set(double samples, double feedback, double lpHz, double fs) {
    time = clampd(samples, 1.0, static_cast<double>(size - 3));
    fb = feedback;
    lpL.set(lpHz, fs);
    lpR.set(lpHz, fs);
  }
  double read(const std::vector<float>& b, double d) const {
    double pos = static_cast<double>(w) - d;
    while (pos < 0.0) pos += size;
    const int i0 = static_cast<int>(pos);
    const double fr = pos - i0;
    const int i1 = (i0 + 1) % size;
    return (1.0 - fr) * static_cast<double>(b[static_cast<size_t>(i0)]) + fr * static_cast<double>(b[static_cast<size_t>(i1)]);
  }
  void process(double inL, double inR, double& outL, double& outR) {
    const double dl = read(bufL, time), dr = read(bufR, time);
    outL = dl;
    outR = dr;
    bufL[static_cast<size_t>(w)] = static_cast<float>(flushDenormal(inL + fb * lpL.lp(dl)));
    bufR[static_cast<size_t>(w)] = static_cast<float>(flushDenormal(inR + fb * lpR.lp(dr)));
    w = (w + 1) % size;
  }
  void reset() {
    std::fill(bufL.begin(), bufL.end(), 0.0f);
    std::fill(bufR.begin(), bufR.end(), 0.0f);
    lpL.reset();
    lpR.reset();
  }
};

}  // namespace mix
}  // namespace shogun
