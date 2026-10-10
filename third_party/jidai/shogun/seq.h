#pragma once
// Pattern data (§10.2, §14 "seq"). Fixed-size, no allocation. 16 tracks in MAIN voice order, up to 32 steps.

#include <cstdint>
#include <cstring>

namespace shogun {

constexpr int kMaxSteps = 32;
constexpr int kMaxLocks = 8;
constexpr int kRatchets[6] = {1, 2, 3, 4, 6, 8};

struct StepLock {
  std::int16_t param = -1;  // ParamId
  float u = 0.0f;
};

struct Step {
  bool on = false;          // drums: hit; lead/bass: note (off = rest)
  std::uint8_t acc = 2;     // accent level 1..3 (2.353 / 3.706 / 5.0 V)
  float prob = 1.0f;        // 0..1, seeded per (pattern seed, bar)
  float micro = 0.0f;       // −0.5..+0.5 of a step
  std::uint8_t flam = 0;    // 0 = none, k+1 for flam type k (0..15): hits 2 + (k mod 4), gap 3.75 ms·(1 + floor(k/4))
  std::uint8_t ratchet = 1; // 1, 2, 3, 4, 6, 8
  float bend = 0.0f;        // semitones (BD1/BD2/SD/toms)
  std::int8_t note = 48;    // lead/bass
  bool tie = false;         // lead/bass: glide, no retrigger
  std::uint8_t nLocks = 0;
  StepLock locks[kMaxLocks];
  bool setLock(int param, float u) {
    for (int i = 0; i < nLocks; ++i)
      if (locks[i].param == param) {
        locks[i].u = u;
        return true;
      }
    if (nLocks >= kMaxLocks) return false;
    locks[nLocks].param = static_cast<std::int16_t>(param);
    locks[nLocks].u = u;
    ++nLocks;
    return true;
  }
  void clearLocks() { nLocks = 0; }
};

struct Track {
  int len = 16;          // 1..32
  int scale = -1;        // −1 = follow CLOCK:SCALE, else 0..3 = 1/32, 1/16, 1/8T, 1/8
  double swing = -1.0;   // −1 = follow CLOCK:SWING, else sw ∈ [0.5, 0.75]
  double shift = 0.0;    // u: SHIFT = round(u·30 ms·fs) (KEPT)
  Step steps[kMaxSteps];
};

struct Pattern {
  char name[32] = "001 INIT";  // §14.1
  std::uint32_t seed = 0x5A31C0DEu;
  Track tracks[16];
  void setName(const char* n) {
    std::strncpy(name, n, sizeof name - 1);
    name[sizeof name - 1] = 0;
  }
};

// Steps per quarter for a scale index (1/32, 1/16, 1/8T, 1/8).
inline double stepsPerQuarter(int scale) {
  static const double k[4] = {8.0, 4.0, 3.0, 2.0};
  return k[scale < 0 ? 1 : (scale > 3 ? 3 : scale)];
}
// Old shuffle s (0..15) → swing (§10.2): sw = 0.5 + s/90.
inline double swingFromShuffle(int s) { return 0.5 + s / 90.0; }

}  // namespace shogun
