// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "PatchBay.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

struct TapeRuntime;

namespace lift {

// Commands from the panel (message thread) to the audio thread. Single
// producer, single consumer, fixed size: no locks, no allocation.
enum class Cmd : uint8_t {
    Transport, Stop, Lift, Drop, Rev, Loop, Arm, Mute, Seek, NoteOn, NoteOff,
    // shift layer
    LoopSet,     // a = start frame, b = end frame (b <= a turns the loop off)
    HardStop,    // stop now, no ramp
    LiftAll,     // lift the sum of all four tracks (loop region, or the whole tape)
    UndoDrop,    // put back what the last DROP overdubbed
    Clear,       // a = track; cleared in slices over the next blocks
    Jump,        // v = frame; works while playing (Seek only while stopped)
    Character,   // a = tape character row
    RecSource    // a = 0 synth, 1 input (placeholder: silence), 2 resample (tape out)
};

struct Command {
    Cmd cmd;
    int a;
    int b;
    double v;
};

// PLACEHOLDER SOUND SOURCE. One saw + square voice with a filter and an AR
// envelope so the keyboard has something to print to tape. It stands in for
// the real engines (Loom ... Spool), which are not built yet.
struct PlaceholderVoice {
    double phase = 0.0;
    double phase2 = 0.0;
    double inc = 0.0;
    float env = 0.f;
    float lp = 0.f;
    bool gate = false;
    int note = -1;
    void render(float* out, int n, double sampleRate) noexcept;
};

class LiftProcessor : public juce::AudioProcessor {
public:
    LiftProcessor();
    ~LiftProcessor() override;

    // Tape length for the app: 60 s per track (the engine default of six
    // minutes per track costs ~550 MB).
    static constexpr int kTapeSeconds = 60;
    static constexpr int kLoopSeconds = 8;  // default loop: 4 bars at the placeholder 120 BPM

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "LIFT"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // ---- message thread API (the panel) ----
    void send(Cmd c, int a = 0, int b = 0, double v = 0.0) noexcept;
    std::atomic<float> speed{1.f};     // SPEED knob, 0.25x .. 4x
    std::atomic<float> bias{0.45f};    // BIAS knob, 0..1
    std::atomic<float> drive{1.8f};    // REC LVL knob mapped to drive
    PatchBayModel patch;
    // Shift-layer settings with no engine behind them yet (PLACEHOLDER model,
    // readable by the future sequencer/drum engines).
    std::atomic<int> seqStepDiv{16};   // 4, 8, 16, 32, or 12 for 1/8 triplets
    std::atomic<int> drumStepDiv{16};
    std::atomic<int> drumLength{16};   // steps
    std::atomic<int> drumSwing{0};     // percent
    std::atomic<int> drumKit{0};
    std::atomic<int> synthEngine{0};   // the placeholder voice plays for every engine

    // ---- audio thread -> panel ----
    std::atomic<double> uiPos{0.0};
    std::atomic<int> uiFrames{0};
    std::atomic<float> uiReelSpeed{0.f};  // signed tape speed, 0 when stopped
    std::atomic<bool> uiRecording{false};
    std::atomic<bool> uiPlaying{false};
    std::atomic<int> uiLoopStart{0};
    std::atomic<int> uiLoopEnd{0};
    std::atomic<int> uiClipFrames{0};
    std::atomic<float> uiTrackLevel[4] = {};  // peak of each track under the head (0 when muted)
    std::atomic<float> uiMasterLevel{0.f};    // output peak of the last block
    std::atomic<float> uiSynthEnv{0.f};       // placeholder voice envelope
    std::atomic<int> uiSynthNote{-1};
    std::atomic<float> uiWowPhase{0.f};       // engine wow LFO phase, radians
    std::atomic<float> uiWowDepth{0.f};       // wow + flutter depth (fraction)

    // Scope tap: the placeholder voice output, written by the audio thread into
    // a fixed single-producer/single-consumer ring (no locks, no allocation).
    // The panel drains it on the message thread. Drops samples when full.
    static constexpr int kScopeSize = 8192;
    int readScope(float* dest, int maxSamples) noexcept;

    // Test access (not real-time safe to use while audio runs).
    TapeRuntime& runtime() noexcept { return *rt_; }

private:
    void apply(const Command& c) noexcept;

    std::unique_ptr<TapeRuntime> rt_;
    bool prepared_ = false;
    double sampleRate_ = 48000.0;
    juce::AbstractFifo fifo_{256};
    Command cmds_[256];
    PlaceholderVoice voice_;
    juce::AbstractFifo scopeFifo_{kScopeSize};
    float scope_[kScopeSize] = {};
    void pushScope(const float* x, int n) noexcept;
    juce::HeapBlock<float> synth_;
    juce::HeapBlock<float> zero_;
    juce::HeapBlock<float> resample_;   // last chunk of tape output, for RESAMPLE
    juce::HeapBlock<float> undo_[2];    // what the last DROP wrote over
    int undoStart_ = -1;
    int undoFrames_ = 0;
    int undoTrack_ = 0;
    int clearTrack_ = -1;
    int clearPos_ = 0;
    int recSource_ = 0;
    int synthSize_ = 0;
    float lastBias_ = -1.f;
    float lastDrive_ = -1.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiftProcessor)
};

}  // namespace lift
