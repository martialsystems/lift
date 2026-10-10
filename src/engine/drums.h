// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT drum machine: SHOGUN's analog-model drum voices (third_party/jidai/shogun,
// vendored) driven by LIFT's own kits and step sequencer. Framework free, real
// time safe after prepare() (prepare allocates). Runs at the tape rate.

#include <cstdint>
#include <memory>

namespace shogun {
class Engine;
}

namespace lift::eng {

constexpr int kDrumVoices = 14;  // BD1 BD2 SD RS CP CL MA CB CH OH CY LT MT HT (SHOGUN ids 0..13)
constexpr int kKits = 8;
constexpr int kMaxSteps = 32;

// Short names for the DRUM screen, in voice order.
extern const char* const kDrumVoiceNames[kDrumVoices];

// Which voice a kit plays on each keyboard key (0..23, C up), -1 = none.
int drumVoiceForKey(int key) noexcept;

// One kit: a name for the screen and a parameter list for the SHOGUN voices.
struct KitInfo {
    const char* name;       // "808", "909", ...
    std::uint32_t pattern[kDrumVoices];  // default 16-step pattern, bit s = step s
};
const KitInfo& kitInfo(int kit) noexcept;

// Per-voice DRUM knobs (0..1). 0.5 is the kit's own tuning for PITCH and
// DECAY; CHOKE picks the choke group (OFF, 1..4) with the kit's default.
struct DrumKnobs {
    float pitch = 0.5f;
    float choke = 0.1f;
    float decay = 0.5f;
};
DrumKnobs kitDefaultKnobs(int kit, int voice) noexcept;

class Drums {
public:
    Drums();
    ~Drums();
    Drums(const Drums&) = delete;
    Drums& operator=(const Drums&) = delete;

    void prepare(double fs);       // allocates; loads kit 0
    void reset() noexcept;         // silence every voice
    void setKit(int kit) noexcept; // reloads the kit's tuning (knobs re-applied on top)
    int kit() const noexcept { return kit_; }
    void setKnobs(int voice, const DrumKnobs& k) noexcept;  // smoothed by the engine (5 ms)
    // A hit at the start of the next rendered sample. vel 0..1.
    void hit(int voice, float vel) noexcept;
    // Render n samples (stereo). Hits can be scheduled per sample: hits[i]
    // (voice bitmask) fire at sample i, vel per voice in vel[].
    void render(float* L, float* R, int n, const std::uint32_t* hitMask, const float* vel) noexcept;
    float envelope(int voice) const noexcept;  // amplitude envelope, for the screen
    double peakGain() const noexcept { return gain_; }
    shogun::Engine& engine() noexcept { return *e_; }

private:
    void applyVoice(int v, bool now) noexcept;
    std::unique_ptr<shogun::Engine> e_;
    int kit_ = 0;
    DrumKnobs knobs_[kDrumVoices];
    double gain_ = 2.0;  // SHOGUN's mains sit low (voices at -9 dB, noon); LIFT's drum bus gain
};

}  // namespace lift::eng
