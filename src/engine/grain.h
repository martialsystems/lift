// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// GRAIN: a granular cloud. Up to 24 overlapping Hann-windowed grains read
// either
//  * its own rolling buffer (GRAIN as an FX type): a fixed kBufferSeconds = 8 s
//    stereo ring written with the FX input every sample, independent of loop
//    length and of varispeed (it is written after the playheads, at the bus
//    rate). POS = how far back in those 8 s; or
//  * an external region (GRAIN as SYNTH engine 8): the armed loop's region,
//    POS = where in that region.
// SIZE 10..500 ms, DENSITY 2..80 grains/s, SPRAY = random position jitter and
// stereo spread. G POS / G SIZE pins add to POS / SIZE (+5 V = +1).
// Framework free; prepare() allocates, everything else is real time safe.

#include <cstdint>
#include <vector>

namespace lift::eng {

class GrainCloud {
public:
    static constexpr double kBufferSeconds = 8.0;
    static constexpr int kGrains = 24;
    struct Params {
        float pos = 0.2f, size = 0.4f, density = 0.5f, spray = 0.2f;
        double pitch = 1.0;       // playback rate of new grains
        float pitchSpread = 0.f;  // semitones of random detune per grain
    };
    void prepare(double fs);  // allocates the 8 s ring
    void reset() noexcept;
    void stopGrains() noexcept { for (auto& g : g_) g.on = false; }  // the buffer keeps rolling
    // the own rolling buffer (FX use)
    void write(const float* l, const float* r, int n) noexcept;
    int capacity() const noexcept { return cap_; }
    std::int64_t written() const noexcept { return written_; }
    // external source (engine use); null = the own buffer
    void setSource(const float* l, const float* r, int frames, int lo, int hi) noexcept;
    // adds the cloud into L / R, times gain
    void render(float* l, float* r, int n, const Params& p, float gain) noexcept;
    int activeGrains() const noexcept;

private:
    struct Grain {
        double pos = 0.0, rate = 1.0;
        int len = 0, age = 0;
        float gl = 0.f, gr = 0.f;
        bool on = false;
    };
    void spawn(const Params& p) noexcept;
    float rnd() noexcept;  // 0..1
    double fs_ = 48000.0;
    int cap_ = 0;
    std::vector<float> l_, r_;
    int w_ = 0;
    std::int64_t written_ = 0;
    const float* extL_ = nullptr;
    const float* extR_ = nullptr;
    int extN_ = 0, extLo_ = 0, extHi_ = 0;
    Grain g_[kGrains];
    double untilNext_ = 0.0;
    std::uint32_t rng_ = 0x9e3779b9u;
};

}  // namespace lift::eng
