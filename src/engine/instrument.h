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
#include "engine/clipvoice.h"
#include "engine/fx.h"
#include "engine/patch.h"
#include "engine/synth.h"

#include <atomic>
#include <cstdint>

namespace lift::eng {

// The tape is the host's. A block calls it in graph order: transportBlock
// (N_TRANSPORT), headBlock per loop (N_H1..N_H4), inputBlock (N_INPUT) and
// mixBlock (N_MIX). Audio here is +-1 full scale; the instrument turns it
// into volts (x 5) on the jacks.
struct TapeMixIo {
    const float* srcL = nullptr;  // synth + drums through FX
    const float* srcR = nullptr;
    const float* inL = nullptr;   // the IN path (inputBlock's result)
    const float* inR = nullptr;
    const float* biasCv = nullptr;  // volts: +5 V = the whole BIAS range (null = none)
    bool recPress = false;          // REC jack / column: a rising edge this block
    float* outL = nullptr;          // the master
    float* outR = nullptr;
    float* send = nullptr;          // the FX send sum (mono)
};
struct TapeHost {
    virtual ~TapeHost() = default;
    // Advance the transport one block; eoc: 5 V pulses on loop wrap.
    virtual void transportBlock(float* eoc, int n) noexcept = 0;
    // Loop t's playback for the block (mono, +-1, before the fader). Only the
    // armed loop gets speedCv (1 V = 1 semitone), rev and scrubCv (+-5 V).
    virtual void headBlock(int t, const float* speedCv, bool rev, const float* scrubCv, float* head, int n) noexcept = 0;
    // The IN path: AUDIO L / R cables (volts, null = unpatched; R null = L)
    // or the interface inputs. Writes +-1 audio.
    virtual void inputBlock(const float* cableL, const float* cableR, float* outL, float* outR, int n) noexcept = 0;
    virtual void mixBlock(const TapeMixIo& io, int n) noexcept = 0;
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
    int transpose = 0;      // semitones (QUANT key / root)
    int quantScale = 0;     // QUANT: 0 major, 1 minor, 2 major pentatonic, 3 minor pentatonic, 4 chromatic
    float drumGateMs = 10.f;  // the DRUM jack's pulse length (1 .. 250 ms)
    int arm = 0;            // the armed loop (SPEED / SCRUB / REVERSE move it)
};
// QUANT: volts (1 V/oct) to the nearest note of the scale, root in semitones.
float quantize(float volts, int scale, int root) noexcept;
const char* const kQuantScaleNames[5] = {"MAJOR", "MINOR", "MAJ PENTA", "MIN PENTA", "CHROMATIC"};

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

    void setPatch(const PatchPlan& p) noexcept;
    // Patterns: [kit][voice] step bitmasks, written by the message thread.
    void setPatterns(const std::atomic<std::uint32_t>* p) noexcept { patterns_ = p; }

    void block(TapeHost& tape, const InstrumentCtl& ctl, float* outL, float* outR) noexcept;  // kBlock samples

    PolySynth synth;
    Drums drums;
    ClipPlayer clips;  // kept sounds on keys / DRUM slices (mixed with the synth, before FX)
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
    bool gateTaken() const noexcept { return gateTaken_; }  // GATE patched: keys / SEQ no longer trigger the synth
    float grainPos() const noexcept { return gPos_; }        // G POS / G SIZE columns (volts, block mean)
    float grainSize() const noexcept { return gSize_; }
    float follow() const noexcept { return follow_; }        // FOLLOW row, volts

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
    int sliceVoice_ = 0;
    bool accentPending_ = false;
    std::uint32_t lastHits_ = 0;
    std::uint32_t hitMask_[kBlock] = {};
    float vel_[kDrumVoices] = {};
    // utilities
    double lfo2Ph_ = 0.0;
    float rnd_ = 0.f, rndFrom_ = 0.f, rndTo_ = 0.f, rndT_ = 1.f;
    float env2_ = 0.f;
    bool env2Gate_ = false;
    float follow_ = 0.f;
    float lastVel_ = 0.8f;
    float accentLeft_ = 0.f;
    float pulseLeft_ = 0.f;
    bool gateTaken_ = false;    // the plan patches GATE beyond the keyboard's own cable
    bool pitchTaken_ = false;   // the plan replaces the PITCH normal (a cable or a pitch-row pin)
    bool vcaPinned_ = false;
    bool clkExternal_ = false;  // CLK IN fed by anything but CLOCK itself    // a pin in the VCA column replaces ENV -> VCA
    bool recGate_ = false;
    float gPos_ = 0.f, gSize_ = 0.f;
    float inL_[kBlock] = {}, inR_[kBlock] = {};
    float send_[kBlock] = {};
    float head_[kBlock] = {};
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
    float clipL_[kBlock] = {}, clipR_[kBlock] = {};
    float srcL_[kBlock] = {}, srcR_[kBlock] = {};
    float* outL_ = nullptr;
    float* outR_ = nullptr;
};

}  // namespace lift::eng
