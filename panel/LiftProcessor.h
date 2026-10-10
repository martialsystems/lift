// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "PatchBay.h"
#include "KnobSmoother.h"
#include "SlotStore.h"
#include "UiState.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

struct TapeRuntime;
#include "tape/engine.h"

namespace lift {

// Paint-time accounting for the UI bench (tests/ui_bench.cpp): total ms spent
// in the panel's paint calls. Costs one clock read per paint.
inline std::atomic<double>& paintStatsMs() noexcept {
    static std::atomic<double> ms{0.0};
    return ms;
}
struct ScopedPaintStats {
    double t0 = juce::Time::getMillisecondCounterHiRes();
    ~ScopedPaintStats() {
        const double d = juce::Time::getMillisecondCounterHiRes() - t0;
        paintStatsMs().store(paintStatsMs().load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
    }
};

// Commands from the panel (message thread) to the audio thread. Single
// producer, single consumer, fixed size: no locks, no allocation.
enum class Cmd : uint8_t {
    Transport, Stop, Lift, Drop, Rev, Loop, Arm, Mute, Seek,
    NoteOn,      // a = note, b = velocity 1..127 (0 = 100)
    NoteOff,     // a = note (-1 = every note the panel holds)
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
// the real engines (Loom ... Spool), which are not built yet. Mono, last-note
// priority (legato back to a held note), velocity, pitch bend (+-2 semitones),
// mod wheel vibrato and sustain pedal. Always runs at the tape rate (48 kHz).
struct PlaceholderVoice {
    double phase = 0.0;
    double phase2 = 0.0;
    double vibPhase = 0.0;
    float env = 0.f;
    float lp = 0.f;
    float vel = 0.8f;
    float bend = 0.f;   // semitones
    float mod = 0.f;    // 0..1 vibrato depth (CC1)
    // The SYNTH screen's four knobs (smoothed, 0..1), the same for every
    // engine on this placeholder: 0 detune / timbre, 1 filter cutoff,
    // 2 filter envelope amount (centre = none), 3 decay / release.
    float macro[4] = {0.35f, 0.62f, 0.48f, 0.55f};
    float fenv = 0.f;   // filter envelope
    bool gate = false;
    bool sustain = false;
    int note = -1;
    int held[16] = {};
    int depth = 0;
    void noteOn(int n, float velocity) noexcept;
    void noteOff(int n) noexcept;  // n < 0: release every held note
    void setSustain(bool on) noexcept;
    void allOff() noexcept;
    void render(float* out, int n) noexcept;
};

// Message thread -> audio thread events are Commands; these go the other way
// (MIDI that the message thread acts on: knob CCs, program change).
struct MidiEvent {
    uint8_t kind;  // 0 CC (a = number, b = value), 1 program change (a = slot)
    int a;
    int b;
};

class LiftProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    LiftProcessor();
    ~LiftProcessor() override;

    // Tape length for the app: 60 s per track (the engine default of six
    // minutes per track costs ~550 MB).
    static constexpr int kTapeSeconds = 60;
    static constexpr int kLoopSeconds = 8;  // default loop: 4 bars at 120 BPM
    static constexpr int kStateVersion = 1; // saved-state format version

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "LIFT"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    // Plug-in state: the same bytes as a save slot (tape audio included). The
    // standalone keeps no session state (no autosave): it reopens the last
    // saved slot instead.
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

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
    // TAPE knobs (SPEED, BIAS, REC LVL) -> the engine atomics above.
    void setTapeKnobs(const std::array<float, 4>& tape) noexcept;
    // Every screen knob, screen * 4 + knob (0..1). The audio thread follows
    // them with per-control smoothing (KnobSmoother, 20 ms): TAPE SPEED /
    // BIAS / REC LVL / SCRUB, SYNTH macros, MIX level / pan / low / high of
    // the armed track, IN gain. The panel sets them as they turn.
    std::atomic<float> knobs[20];
    void setKnobValue(int screen, int k, float v) noexcept;
    void setAllKnobs(const std::array<std::array<float, 4>, 5>& enc) noexcept;
    static constexpr double kScrubSeconds = 4.0;  // a full SCRUB sweep moves this much tape

    // ---- saved state (message thread) ----
    // The panel's state. The panel pushes its changes here; loads, MIDI knob
    // CCs and MIDI learn come back to the panel through the Listener.
    struct Listener {
        virtual ~Listener() = default;
        virtual void flushState() = 0;                    // push unsent changes now
        virtual void stateLoaded(int slot) = 0;           // slot 0 = host state
        virtual void knobFromMidi(int target, float value) = 0;
        virtual void learned(int cc, int target) = 0;
        virtual void slotMessage(const juce::String& text) = 0;
    };
    void setListener(Listener* l) noexcept { listener_ = l; }
    const UiState& uiState() const noexcept { return ui_; }
    void setUiState(const UiState& s);

    enum class LoadResult { Ok, Empty, BadSlot, BadData, NewerVersion };
    bool saveSlot(int slot);
    LoadResult loadSlot(int slot);
    bool reopenLast();                  // load the slot saved or loaded last
    int currentSlot() const noexcept { return currentSlot_; }
    SlotStore& slots() noexcept { return slots_; }
    void setSlotFolder(const juce::File& f) { slots_ = SlotStore(f); }
    juce::MemoryBlock saveState();
    LoadResult loadState(const void* data, size_t size, int slot);

    // ---- MIDI (see panel/MIDI.md) ----
    // Fixed CC map: knob target (screen * 4 + knob) for a CC, -1 = none.
    static int fixedCcTarget(int cc, int currentScreen) noexcept;
    static bool reservedCc(int cc) noexcept;  // CCs the voice/bank use; never learned
    std::atomic<int> learnTarget{-1};  // armed by the panel: the next CC binds to it
    void setKnob(int target, float value);
    void drainMidiEvents();            // the 30 Hz timer; tests call it directly
    std::atomic<double> tempoBpm{120.0};
    std::atomic<bool> clockSlaved{false};   // following MIDI clock in
    std::atomic<int> transportSerial{0};    // bumped when MIDI starts or stops the tape
    std::atomic<bool> midiPlay{false};      // what that MIDI start/stop left the transport doing

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

    // Scope tap: the placeholder voice output (48 kHz), written by the audio
    // thread into a fixed single-producer/single-consumer ring (no locks, no
    // allocation). The panel drains it on the message thread.
    static constexpr int kScopeSize = 8192;
    int readScope(float* dest, int maxSamples) noexcept;

    // Test access (not real-time safe to use while audio runs).
    TapeRuntime& runtime() noexcept { return *rt_; }
    const PlaceholderVoice& voice() const noexcept { return voice_; }

private:
    void apply(const Command& c) noexcept;
    void ensureTracks();
    void renderInternal(float* outL, float* outR, int m) noexcept;  // m <= kChunk, at 48 kHz
    void handleMidiIn(const juce::MidiMessage& m, int samplePos) noexcept;
    void post(uint8_t kind, int a, int b) noexcept;
    void timerCallback() override;
    std::atomic<int> pendingLoaded_{0};

    std::unique_ptr<TapeRuntime> rt_;
    bool tracksReady_ = false;
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
    float lastBias_ = -1.f;
    float lastDrive_ = -1.f;
    // knob smoothing and what it drives (audio thread)
    void stepKnobs(int n) noexcept;
    void renderSub(float* outL, float* outR, int off, int m) noexcept;
    KnobSmoother sm_[20];
    float appliedMix_[4] = {-1.f, -1.f, -1.f, -1.f};
    int appliedArm_ = -1;
    float inGain_ = 1.f, inGainPrev_ = 1.f;
    // input monitor through the record electronics (drive, bias), so REC LVL
    // and BIAS are heard while playing, as on a deck's source monitor
    TapeEngine monitor_;
    juce::HeapBlock<float> monL_, monR_;
    // SCRUB: knob movement becomes tape movement with a little reel inertia
    float scrubPrev_ = -1.f;
    double scrubPending_ = 0.0;   // frames still to move
    float scrubGain_ = 0.f;       // head output while the tape moves by hand
    float trackPeak_[4] = {};

    // 48 kHz tape -> device rate. Exact pass-through at 48 kHz; otherwise a
    // 4-point (Catmull-Rom) interpolator pulling 48 kHz chunks on demand.
    double ratio_ = 1.0;       // internal samples per output sample
    double frac_ = 0.0;
    float hist_[2][4] = {};
    juce::HeapBlock<float> intL_, intR_;
    int intPos_ = 0, intLen_ = 0;

    // MIDI
    juce::AbstractFifo midiFifo_{512};
    MidiEvent midiEvents_[512];
    int bankMsb_ = 0;
    juce::int64 sampleCount_ = 0;    // device samples since prepare
    juce::int64 lastClock_ = -1;
    double clockIntervals_[24] = {};
    int clockN_ = 0, clockW_ = 0;
    double clockOutPhase_ = 0.0;     // device samples until the next clock out
    bool wasPlaying_ = false;
    int outNotes_[32] = {};          // panel notes to echo: note | (vel << 8), negative = off
    int outNoteCount_ = 0;

    // state
    UiState ui_;
    SlotStore slots_;
    int currentSlot_ = 0;
    Listener* listener_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiftProcessor)
};

}  // namespace lift
