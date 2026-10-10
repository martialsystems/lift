#pragma once
// The single SHOGUN parameter table (§3.2, §15.2). Every parameter is u ∈ [0,1] with a SECTION:LABEL id.
// Generated part: params_table.h (scripts/gen_params.py). This header adds the laws shared by engine, plugin and web.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "dsp.h"
#include "params_table.h"

namespace shogun {

constexpr int kVoices = 16;
enum VoiceId : int { BD1, BD2, SD, RS, CP, CL, MA, CB, CH, OH, CY, LTC, MTC, HTC, LEAD, BASS };
inline const char* const kVoiceNames[kVoices] = {"BD1", "BD2", "SD", "RS", "CP", "CL", "MA", "CB",
                                                 "CH",  "OH",  "CY", "LTC", "MTC", "HTC", "LEAD", "BASS"};
inline bool isDrum(int v) { return v < LEAD; }
inline bool isWaveVoice(int v) { return v == BD1 || v == BD2 || v == LTC || v == MTC || v == HTC; }

inline int findParam(const char* id) {
  for (int i = 0; i < kParamCount; ++i)
    if (std::strcmp(kParams[i].id, id) == 0) return i;
  return -1;
}

// Stepped controls: index = min(N−1, floor(N·u)) (§3.2, §8.4).
// No libm floor (this runs ~90 times a sample): for x = N·u in (0, N−1) truncation is floor; at or above N−1 the
// clamp gives N−1 and at or below 0 (or NaN) it gives 0, the same index as min(N−1, max(0, floor(x))) for every x.
inline int stepIndex(double u, int n) {
  const double x = u * n;
  if (!(x > 0.0)) return 0;
  if (x >= static_cast<double>(n - 1)) return n - 1;
  return static_cast<int>(x);
}
inline double stepU(int index, int n) { return (index + 0.5) / n; }
inline double bip(double u) { return 2.0 * u - 1.0; }

// Common per-voice parameter block (the same offsets for every voice).
struct VoiceParams {
  int level, pan, send, output, mute, solo, choke, velLevel, velDecay, accAmt, trigMerge;
};
inline VoiceParams voiceParams(int v) {
  char buf[48];
  auto f = [&](const char* label) {
    std::snprintf(buf, sizeof buf, "%s:%s", kVoiceNames[v], label);
    return findParam(buf);
  };
  return {f("LEVEL"), f("PAN"), f("SEND"), f("OUTPUT"), f("MUTE"), f("SOLO"),
          f("CHOKE"), f("VEL>LEVEL"), f("VEL>DECAY"), f("ACC AMT"), f("TRIG MERGE")};
}

// Parameter ids of a WAVE voice's triple wave shaper block (§4.6).
struct WaveParams {
  int macro, trim[3], sym[3], vcAmt[3], vcSym[3], shape, vcLevel, routing, levelComp, vcSrc;
};
inline WaveParams waveParams(int v) {
  char buf[48];
  auto f = [&](const char* label) {
    std::snprintf(buf, sizeof buf, "%s:%s", kVoiceNames[v], label);
    return findParam(buf);
  };
  auto fi = [&](const char* label, int i) {
    std::snprintf(buf, sizeof buf, "%s:%s %d", kVoiceNames[v], label, i + 1);
    return findParam(buf);
  };
  WaveParams w{};
  w.macro = f("WAVE");
  for (int i = 0; i < 3; ++i) {
    w.trim[i] = fi("WAVE", i);
    w.sym[i] = fi("SYM", i);
    w.vcAmt[i] = fi("VC>AMT", i);
    w.vcSym[i] = fi("VC>SYM", i);
  }
  w.shape = f("SHAPE");
  w.vcLevel = f("VC LEVEL");
  w.routing = f("ROUTING");
  w.levelComp = f("LEVEL COMP");
  w.vcSrc = f("VC SRC");
  return w;
}

// Display text for a parameter value (LCD, tooltip, host automation lane). Left out of the freestanding wasm build
// (no printf there); the web page formats values itself.
#ifndef SHOGUN_NO_FORMAT
inline void formatParam(int p, double u, char* out, int n) {
  const ParamInfo& pi = kParams[p];
  const std::size_t cap = static_cast<std::size_t>(n);
  if (pi.kind == ParamKind::Stepped || pi.kind == ParamKind::Toggle) {
    const int idx = stepIndex(u, pi.steps);
    if (std::strcmp(pi.law, "sound") == 0) {
      if (idx == 0) std::snprintf(out, cap, "CLN");
      else std::snprintf(out, cap, "%d", idx);
      return;
    }
    const char* c = pi.choices;
    for (int i = 0; i < idx && c; ++i) {
      c = std::strchr(c, '|');
      if (c) ++c;
    }
    if (!c || !*c) {
      std::snprintf(out, cap, "%d", idx);
      return;
    }
    const char* e = std::strchr(c, '|');
    const int len = e ? static_cast<int>(e - c) : static_cast<int>(std::strlen(c));
    std::snprintf(out, cap, "%.*s", len, c);
    return;
  }
  const char* law = pi.law;
  double a = 0, b = 0;
  if (std::sscanf(law, "hz %lf %lf", &a, &b) == 2) {
    const double hz = a * std::pow(b / a, u);
    if (hz >= 1000.0) std::snprintf(out, cap, "%.2f kHz", hz / 1000.0);
    else std::snprintf(out, cap, "%.1f Hz", hz);
  } else if (std::sscanf(law, "st %lf %lf", &a, &b) == 2) {
    std::snprintf(out, cap, "%+.1f st", a + (b - a) * u);
  } else if (std::sscanf(law, "ratio %lf %lf", &a, &b) == 2) {
    std::snprintf(out, cap, "x%.2f", a * std::pow(b / a, u));
  } else if (std::strcmp(law, "decay") == 0) {
    std::snprintf(out, cap, "%.0f ms", 1000.0 * dsp::decayTau(u));
  } else if (std::strcmp(law, "level") == 0) {
    const double g = dsp::gLevel(u);
    if (g <= 1e-6) std::snprintf(out, cap, "-inf dB");
    else std::snprintf(out, cap, "%+.1f dB", 20.0 * std::log10(g));
  } else if (std::strcmp(law, "volume") == 0) {
    const double g = 2.0 * u * u;
    if (g <= 1e-6) std::snprintf(out, cap, "-inf dB");
    else std::snprintf(out, cap, "%+.1f dB", 20.0 * std::log10(g));
  } else if (std::strcmp(law, "bpm") == 0) {
    std::snprintf(out, cap, "%.1f BPM", 40.0 + 160.0 * u);
  } else if (std::strcmp(law, "swing") == 0) {
    std::snprintf(out, cap, "%.1f %%", 50.0 + 25.0 * u);
  } else if (std::strcmp(law, "drive") == 0) {
    if (u <= 0.0) std::snprintf(out, cap, "OFF");
    else std::snprintf(out, cap, "%.2f", 9.0 * u);
  } else if (std::strcmp(law, "drive24") == 0) {
    if (u <= 0.0) std::snprintf(out, cap, "OFF");
    else std::snprintf(out, cap, "+%.1f dB", 24.0 * u);
  } else if (std::strcmp(law, "ceiling") == 0) {
    std::snprintf(out, cap, "%.1f dBFS", -6.0 + 6.0 * u);
  } else if (std::strcmp(law, "thresh") == 0) {
    std::snprintf(out, cap, "%.1f dB", -40.0 + 40.0 * u);
  } else if (std::strcmp(law, "ratio") == 0) {
    std::snprintf(out, cap, "%.1f:1", 1.0 + 19.0 * u);
  } else if (std::strcmp(law, "attack") == 0) {
    std::snprintf(out, cap, "%.1f ms", 0.1 * std::pow(1000.0, u));
  } else if (std::strcmp(law, "release") == 0) {
    std::snprintf(out, cap, "%.0f ms", 10.0 * std::pow(100.0, u));
  } else if (std::strcmp(law, "makeup") == 0) {
    std::snprintf(out, cap, "+%.1f dB", 24.0 * u);
  } else if (std::strcmp(law, "lforate") == 0) {
    std::snprintf(out, cap, "%.3f Hz", 0.01 * std::pow(4000.0, u));
  } else if (std::strcmp(law, "deg") == 0) {
    std::snprintf(out, cap, "%.0f deg", 360.0 * u);
  } else if (std::strcmp(law, "fade") == 0) {
    std::snprintf(out, cap, "%.2f s", 2.0 * u);
  } else if (std::strcmp(law, "a4") == 0) {
    std::snprintf(out, cap, "%.1f Hz", 415.0 + 51.0 * u);
  } else if (std::strcmp(law, "cents") == 0) {
    std::snprintf(out, cap, "%+.0f ct", 100.0 * bip(u));
  } else if (std::strcmp(law, "glide") == 0) {
    std::snprintf(out, cap, "%.0f ms", 2.0 * std::exp(5.5 * u));
  } else if (std::strcmp(law, "shape") == 0) {
    std::snprintf(out, cap, "%s %.0f %%", u < 0.5 ? "TRI" : "SAW", 100.0 * u);
  } else if (pi.kind == ParamKind::Bipolar) {
    std::snprintf(out, cap, "%+.0f %%", 100.0 * bip(u));
  } else {
    std::snprintf(out, cap, "%.0f %%", 100.0 * u);
  }
}
#endif

// Laws used by the engine.
inline double tempoBpm(double u) { return 40.0 + 160.0 * u; }
inline double a4Hz(double u) { return 415.0 + 51.0 * u; }
inline double glideTau(double u) { return 0.002 * std::exp(5.5 * u); }
inline double masterVolume(double u) { return 2.0 * u * u; }  // 0.707 → 0 dB

}  // namespace shogun
