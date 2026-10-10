// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Clip voices: kept sounds played from the keys, a DRUM slice kit or a key's
// one-shot. Eight voices, linear interpolation, 2 ms attack, 10 ms release.
// The buffers belong to the host and stay alive while a voice may read them
// (the panel keeps every clip in its history). Real time safe.

namespace lift::eng {

class ClipPlayer {
public:
    static constexpr int kVoices = 8;
    void prepare(double fs) noexcept;
    void reset() noexcept;
    // rate 1 = as recorded; tag identifies the voice for release (e.g. the note)
    void trigger(const float* l, const float* r, int frames, double rate, float vel, int tag, bool oneShot) noexcept;
    void release(int tag) noexcept;
    void releaseAll() noexcept;
    // adds into L / R
    void render(float* L, float* R, int n) noexcept;
    bool active() const noexcept;

private:
    struct Voice {
        const float* l = nullptr;
        const float* r = nullptr;
        int n = 0;
        double pos = 0.0, rate = 1.0;
        float gain = 1.f, env = 0.f;
        bool on = false, rel = false, oneShot = false;
        int tag = -1;
        unsigned age = 0;
    };
    Voice v_[kVoices];
    float att_ = 0.01f, relA_ = 0.002f;
    unsigned age_ = 0;
};

}  // namespace lift::eng
