// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT's synth: six voices, eight engine slots. Framework free and real time
// safe after prepare(). Voices run at twice the tape rate and share one
// halfband decimator (SHOGUN's, from the vendored jidai DSP blocks), so the
// folding, phase-distortion and FM engines stay clean at the top of the
// keyboard. Built from the Jidai DSP blocks (third_party/jidai/shogun/dsp.h:
// PolyBLEP oscillators, the ZDF ladder, the OTA SVF, RC envelopes, the
// halfband decimator) plus LIFT's own engines.

#include <cstdint>
#include <memory>

namespace lift::eng {

constexpr int kSynthVoices = 6;
enum SynthEngine : int { LOOM, BEND, FOLD, RATIO, WIRE, SWARM, SPOOL, SPARE, kSynthEngines };

// Per-sample control inputs from the patch bay (null = not patched).
struct SynthCv {
    const float* pitch = nullptr;   // volts, 1 V/oct offset on every voice
    const float* fm = nullptr;      // volts: audio-rate FM (1 V = 1/4 semitone), RATIO: +1/4 index per volt
    const float* cutoff = nullptr;  // volts: the engine's timbre control (+5 V = +20 % of its range)
};

class PolySynth {
public:
    PolySynth();
    ~PolySynth();
    PolySynth(const PolySynth&) = delete;
    PolySynth& operator=(const PolySynth&) = delete;

    void prepare(double fs);  // allocates (string delay lines); fs = the tape rate
    void reset() noexcept;
    void setEngine(int e) noexcept;
    int engine() const noexcept { return engine_; }
    void setMacros(const float m[4]) noexcept;  // the SYNTH screen's knobs, already smoothed
    void noteOn(int note, float vel) noexcept;
    void noteOff(int note) noexcept;   // note < 0: release every voice
    void allOff() noexcept;            // hard: silence now
    void setSustain(bool on) noexcept;
    void setBend(float semitones) noexcept { bend_ = semitones; }
    void setMod(float depth) noexcept { mod_ = depth; }
    // SPOOL plays this buffer (a tape track), root C4 = recorded pitch.
    void setSpoolSource(const float* l, const float* r, int frames, int loopStart, int loopEnd) noexcept;
    // Mono, n samples at the tape rate (n <= 64).
    void render(float* out, int n, const SynthCv& cv) noexcept;

    // Screen / tests
    bool voiceActive(int v) const noexcept;
    float voiceEnv(int v) const noexcept;
    int voiceNote(int v) const noexcept;
    float env() const noexcept;      // loudest voice envelope
    int lastNote() const noexcept { return lastNote_; }
    bool anyGate() const noexcept;

    struct Voice;

private:
    void renderVoice(Voice& v, float* dst, int n2, const SynthCv& cv, int n) noexcept;
    void startVoice(Voice& v, int note, float vel) noexcept;
    double fs_ = 48000.0, fsE_ = 96000.0;
    int engine_ = LOOM;
    float m_[4] = {0.35f, 0.62f, 0.48f, 0.55f};
    float bend_ = 0.f, mod_ = 0.f;
    double vibPh_ = 0.0;
    bool sustain_ = false;
    std::uint32_t age_ = 0;
    int lastNote_ = -1;
    const float* spoolL_ = nullptr;
    const float* spoolR_ = nullptr;
    int spoolFrames_ = 0, spoolLoopStart_ = 0, spoolLoopEnd_ = 0;
    std::unique_ptr<Voice[]> v_;
    std::unique_ptr<float[]> strings_;  // WIRE delay lines
    struct Dec;
    std::unique_ptr<Dec> dec_;
    float scratch_[128] = {};
    std::uint32_t rng_ = 0x9E3779B9u;
};

}  // namespace lift::eng
