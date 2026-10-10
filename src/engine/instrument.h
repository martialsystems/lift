// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Everything around the tape: the patch graph and its nodes (clock, keyboard
// CV, drum machine, utility modules, synth, FX), one fixed 32-sample block at
// a time at the tape rate. Framework free and real time safe after prepare().
// The tape itself is the host's (TapeHost), so the tape engine stays LIFT's
// own and this file does not know how it records.
//
// Utility modules follow RONIN's (Source/Modular): MG -> LFO, EG -> ENV,
// SampleHold -> S&H, VCA, Integrator -> SLEW; QUANT uses jidai-common's pitch
// law (1 V/oct, 0 V = C4).

#include "engine/drums.h"
#include "engine/fx.h"
#include "engine/patch.h"
#include "engine/synth.h"

#include <atomic>
#include <cstdint>

namespace lift::eng {

struct TapeHost {
    virtual ~TapeHost() = default;
    // One block: src = what the tape records / monitors (synth + drums
    // through FX). speedCv / biasCv: volts per sample or null; reverse: the
    // REVERSE gate (true = high). Writes the output mix and the two heads
    // (tape playback, +-1 full scale).
    virtual void tapeBlock(const float* srcL, const float* srcR, const float* speedCv, bool reversePatched,
                           bool reverse, const float* biasCv, float* outL, float* outR, float* head1, float* head2,
                           int n) noexcept = 0;
};

// Settings the host sets per block (the panel's shift layer, transport).
struct InstrumentCtl {
    bool running = false;   // the transport plays (sequencers run)
    double bpm = 120.0;
    int stepDiv = 16;       // drum step: 4, 8, 16, 32, or 12 (1/8 triplets)
    int arpDiv = 16;        // B PITCH/GATE step
    int length = 16;        // drum pattern steps
    int swing = 0;          // percent
    int kit = 0;
    int selVoice = 0;       // the drum voice the DRUM knobs and SLICE jack play
    bool fxOn = true;
    int transpose = 0;      // semitones (QUANT key)
};

class Instrument {
public:
    Instrument();
    void prepare(double fs);  // allocates
    void reset() noexcept;

    // Note events (audio thread)
    void noteOn(int note, float vel) noexcept;   // synth + A PITCH/GATE + the arpeggiator's held notes
    void noteOff(int note) noexcept;             // note < 0: all
    void drumHit(int voice, float vel) noexcept; // next block start
    void allOff() noexcept;

    void setPatch(const PatchPlan& p) noexcept { router.setPlan(p); }
    // Patterns: [kit][voice] step bitmasks, written by the message thread.
    void setPatterns(const std::atomic<std::uint32_t>* p) noexcept { patterns_ = p; }

    void block(TapeHost& tape, const InstrumentCtl& ctl, float* outL, float* outR) noexcept;  // kBlock samples

    PolySynth synth;
    Drums drums;
    FxRack fx;
    Router router;

    // read-outs (screen, tests)
    int step() const noexcept { return lastStep_; }               // drum step on show, -1 stopped
    std::int64_t samples() const noexcept { return sampleCount_; }
    std::int64_t lastTickSample() const noexcept { return lastTick_; }
    int ticks() const noexcept { return tickCount_; }
    std::uint32_t takeHits() noexcept { const std::uint32_t h = lastHits_; lastHits_ = 0; return h; }
    const float* synthOut() const noexcept { return synthBuf_; }
    float lfo() const noexcept { return lfoOut_; }

private:
    void runNode(int node, TapeHost& tape, const InstrumentCtl& ctl) noexcept;
    double fs_ = 48000.0;
    const std::atomic<std::uint32_t>* patterns_ = nullptr;
    // clock
    double pos_ = 0.0;          // steps since start
    int nextStep_ = 0;
    bool wasRunning_ = false;
    int tickAt_[kBlock];        // per sample: step index ticking here, -1 none
    int lastStep_ = -1;
    std::int64_t sampleCount_ = 0, lastTick_ = -1;
    int tickCount_ = 0;
    float clkHigh_ = 0.f;       // samples left high
    float rstHigh_ = 0.f;
    bool clkInPrev_ = false, rstInPrev_ = false, rstPending_ = false;
    int extStep_ = 0;
    // keys
    int held_[16] = {};
    int nHeld_ = 0;
    int lastNote_ = 60;
    float aGate_ = 0.f;
    int arpIdx_ = 0;
    float bPitch_ = 0.f, bGateLeft_ = 0.f;
    int arpNote_ = -1;
    // drums
    std::uint32_t pendingHits_ = 0;
    float pendingVel_[kDrumVoices] = {};
    float drumGate_ = 0.f;
    bool sliceIn_ = false;
    std::uint32_t lastHits_ = 0;
    std::uint32_t hitMask_[kBlock] = {};
    float vel_[kDrumVoices] = {};
    // utilities
    double lfoPh_ = 0.0;
    float lfoOut_ = 0.f;
    float env_ = 0.f;
    float sh_ = 0.f;
    std::uint32_t rng_ = 0x2545F491u;
    float slew_ = 0.f;
    // synth CV voice (GATE jack)
    bool cvGate_ = false;
    int cvNote_ = 60;
    float pitchRel_[kBlock] = {};
    bool isHeld(int note) const noexcept;
    // audio buffers
    float synthBuf_[kBlock] = {};
    float drumL_[kBlock] = {}, drumR_[kBlock] = {};
    float srcL_[kBlock] = {}, srcR_[kBlock] = {};
    float head1_[kBlock] = {}, head2_[kBlock] = {};
    float* outL_ = nullptr;
    float* outR_ = nullptr;
};

}  // namespace lift::eng
