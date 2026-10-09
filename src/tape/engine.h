// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/character.h"

#include <cstdint>

// Tape emulation core. Platform free: JUCE DSP only, no UI, no device code.
// Record chain (block): drive, pre-emphasis, 4x oversampled hysteresis with
// bias, de-emphasis. Playback chain (per sample, on the tape mix): head bump and
// gap loss that follow tape speed, wow and flutter as a modulated fractional
// delay, hiss, and the stop and start ramps. Allocation lives in
// engine_prepare.cpp only.

namespace juce::dsp {
template <typename SampleType>
class Oversampling;
}

constexpr int kTapeMaxBlock = 512;
constexpr int kTapeDelaySize = 4096;
constexpr int kTapeDelayMask = kTapeDelaySize - 1;

struct TapeParams {
    float speedIps;      // 7.5, 15 or 30. Head bump and gap loss scale with speed.
    float drive;         // record level into the hysteresis, linear, 0.25 to 16
    float bias;          // 0 under-biased (gritty, bright) to 1 over-biased (clean, dull)
    float asym;          // even harmonic amount
    float preEmphDb;     // record pre-emphasis, undone after the nonlinearity
    float bumpHz;        // head bump frequency at 15 ips
    float bumpDb;
    float bumpQ;
    float lowpassHz;     // gap and spacing loss corner at 15 ips
    float wowHz;
    float wowDepth;      // peak pitch deviation as a fraction, 0.001 = 0.1 %
    float flutterHz;
    float flutterDepth;  // peak pitch deviation as a fraction
    float hissDb;        // RMS noise floor in dBFS, -120 turns it off
    float speedGlide;    // varispeed smoothing time in seconds
};

// Double precision: low corners (head bump) are noisy in float.
struct TapeBiquad {
    double b0, b1, b2, a1, a2;
    double z1[2];
    double z2[2];
};

// Trapezoidal state variable filter: stays smooth while its corner moves with
// tape speed. Used as a low pass or as a bell.
struct TapeSvf {
    double a1, a2, a3;
    double k;
    double bellGain;
    double ic1[2];
    double ic2[2];
};

struct TapeEngine {
    TapeParams p;
    double sampleRate;
    juce::dsp::Oversampling<float>* os;
    int latency;
    // record chain
    TapeBiquad preEmph;
    TapeBiquad deEmph;
    float hystPlay[2];
    float dcX[2];
    float dcY[2];
    float* scratch[2];
    // playback chain
    TapeSvf bump;
    TapeSvf gapA;
    TapeSvf gapB;
    float coeffSpeed;
    int coeffCountdown;
    bool dirty;
    float delay[2][kTapeDelaySize];
    int delayWrite;
    double wowPhase;
    double flutterPhase;
    float flutterAmp;
    float flutterAmp2;
    float hissLp[2];
    uint32_t rng;
    // speed
    float speedNow;
    float ramp;
    float rampInc;
    bool stopping;
};

void tape_engine_init(TapeEngine& e) noexcept;
void tape_engine_set_character(TapeEngine& e, const Character& row) noexcept;
void tape_engine_set_params(TapeEngine& e, const TapeParams& p) noexcept;
void tape_engine_reset(TapeEngine& e) noexcept;

// engine_prepare.cpp: off the audio thread.
void tape_engine_prepare(TapeEngine& e, double sampleRate);
void tape_engine_release(TapeEngine& e);

// Record chain. Any n; works in kTapeMaxBlock chunks. Output lags input by
// tape_engine_latency samples. Without prepare it passes audio through.
void tape_engine_record(TapeEngine& e, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;
int tape_engine_latency(const TapeEngine& e) noexcept;

// Per sample. Returns the tape speed factor for the playhead: smoothed
// varispeed times the stop or start ramp.
float tape_engine_next_speed(TapeEngine& e, float varispeed) noexcept;
// Per sample, after next_speed. Runs the playback chain on the tape mix.
void tape_engine_play(TapeEngine& e, float& l, float& r) noexcept;

void tape_engine_stop(TapeEngine& e, float seconds) noexcept;
void tape_engine_start(TapeEngine& e, float seconds) noexcept;
// True once a stop ramp has reached zero. Cleared by tape_engine_reset or start.
bool tape_engine_halted(const TapeEngine& e) noexcept;

// Flush to zero for the current scope. Use around block processing.
struct TapeNoDenormals {
    intptr_t saved;
    TapeNoDenormals() noexcept;
    ~TapeNoDenormals() noexcept;
};
