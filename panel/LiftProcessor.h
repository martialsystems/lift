// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "PatchBay.h"
#include "KnobSmoother.h"
#include "SlotStore.h"
#include "UiState.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>
#include <atomic>
#include <memory>

struct TapeRuntime;
#include "tape/engine.h"
#include "engine/instrument.h"
#include "engine/resample.h"

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
    RecSource,   // a = 0 synth, 1 input (placeholder: silence), 2 resample (tape out)
    DrumHit,     // a = voice, b = velocity 1..127
    DrumKnob,    // a = voice, b = 0 pitch / 1 choke / 2 decay, v = 0..1
    FxType,      // a = effect (eng::FxType)
    AllNotesOff, // every synth voice, now
    UndoPass,    // discard an overdub pass: a = pass number (0 = the one in progress / newest), b = track
    UndoAudio,   // undo the last DROP onto a loop (same as UndoDrop)
    DropClip,    // a = track: overdub the pending clip (pendingDrop_) at the playhead, wrapped in the loop
    SliceHit,    // a = slice (DRUM screen keys with a slice kit loaded), b = velocity
    Cassette,    // a = 1 the cassette stage on (record + playback electronics), 0 off
};

struct Command {
    Cmd cmd;
    int a;
    int b;
    double v;
};

// Message thread -> audio thread events are Commands; these go the other way
// (MIDI that the message thread acts on: knob CCs, program change).
struct MidiEvent {
    uint8_t kind;  // 0 CC (a = number, b = value), 1 program change (a = slot)
    int a;
    int b;
};

class LiftProcessor : public juce::AudioProcessor, private juce::Timer, private eng::TapeHost {
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
    std::atomic<float> liftBack{4.f};  // LIFT encoder: seconds of the capture buffer the next LIFT keeps
    bool overdubbing() const noexcept { return uiRecording.load(std::memory_order_relaxed); }
    const char* audioOpName(int op) const noexcept;

    // ---- resampling (P3): docs/SIGNAL-ORDER.md, src/engine/resample.h ----
    // Every keep is trimmed (loudness to eng::kTargetLufs, true-peak limited)
    // and comes with its default selection. Message thread.
    using ClipPtr = std::shared_ptr<const eng::Clip>;
    ClipPtr keepLast(double seconds);                         // LIFT: the last N s heard
    ClipPtr keepRange(std::int64_t from, std::int64_t to);    // REC: one pass, capture frames
    ClipPtr keepTracks();                                     // SHIFT+LIFT: the four loops' sum
    ClipPtr makeClip(std::vector<float> l, std::vector<float> r, bool loop);
    float preKeepLufs(double seconds) const noexcept { return capture_.loudness(seconds); }
    std::int64_t captureNow() const noexcept { return capture_.written(); }
    const eng::CaptureRing& capture() const noexcept { return capture_; }
    std::atomic<std::int64_t> recCapStart{-1};   // capture frame where the current REC pass began
    // places (the audio thread reads raw pointers; live_ keeps every placed clip alive)
    void dropToLoop(const ClipPtr& c, int track);
    void setKeysClip(const ClipPtr& c);          // null: the synth plays the keys again
    void setKeyClip(int note, const ClipPtr& c); // one key plays it as a one-shot (drum sound)
    void setSliceKit(const ClipPtr& c, int slices);  // DRUM keys play up to 24 slices
    void setAudition(const ClipPtr& c);          // SELECT open: the keys audition the selection
    void addToPool(const ClipPtr& c) { pool_.push_back(c); }
    const std::vector<ClipPtr>& pool() const noexcept { return pool_; }
    bool sliceKitLoaded() const noexcept { return sliceKit_.load() != nullptr; }
    ClipPtr slotClip() const;                    // the clip a loaded slot carried (newest keep)
    // Standalone export: the clip, T1-T4 and the master (the capture: what
    // you heard) as 24-bit WAV at the tape rate (48 kHz) into `folder`.
    bool exportWavs(const juce::File& folder, const ClipPtr& clip, juce::StringArray* written = nullptr);
    std::atomic<int> uiPassCount{0};    // overdub passes started (the panel's history follows it)
    std::atomic<float> uiLimiterGr{0.f};
    std::atomic<float> bias{0.45f};    // BIAS knob, 0..1
    std::atomic<float> drive{1.8f};    // REC LVL knob mapped to drive
    PatchBayModel patch;
    // Shift-layer settings with no engine behind them yet (PLACEHOLDER model,
    // readable by the future sequencer/drum engines).
    std::atomic<int> seqStepDiv{16};   // 4, 8, 16, 32, or 12 for 1/8 triplets
    std::atomic<int> drumStepDiv{16};
    std::atomic<int> drumLength{16};   // steps
    std::atomic<int> drumSwing{0};     // percent
    // P4: QUANT scale (eng::kQuantScaleNames), the DRUM jack's pulse length
    // (ms) and the REC-jack option "this jack presses LIFT"
    std::atomic<int> quantScale{0};
    std::atomic<float> drumGateMs{10.f};
    std::atomic<bool> recJackLifts{false};
    std::atomic<int> drumKit{0};
    std::atomic<int> synthEngine{0};   // eng::SynthEngine
    std::atomic<int> drumVoice{0};     // the voice the DRUM knobs (and the SLICE jack) play
    std::atomic<bool> fxOn{false};      // the FX pad: insert on / bypassed
    std::atomic<int> transposeSemis{0};
    // DRUM patterns: [kit * 14 + voice], bit s = step s (32 steps). The panel
    // writes them, the sequencer reads them (no locks).
    std::atomic<uint32_t> drumPattern[eng::kKits * eng::kDrumVoices];
    // FX knobs (hold FX and turn the four knobs), 0..1, smoothed on the audio side
    std::atomic<float> fxKnobs[4];
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
    // patterns, drum knobs, FX, kit and engine: from a state to the engine (prev: send only changes)
    void applyExtras(const UiState& s, const UiState* prev);
    const eng::Instrument& instrument() const noexcept { return inst_; }  // tests
    // Empty every drum pattern (tests that measure the tape alone). Message thread.
    void clearDrumPatterns() {
        UiState s = ui_;
        for (auto& k : s.drumPat) k.fill(0u);
        setUiState(s);
    }

    enum class LoadResult { Ok, Empty, BadSlot, BadData, NewerVersion };
    bool saveSlot(int slot);
    // The same, with the file written on a background thread so the message
    // thread never waits on the disk; `done` runs on the message thread.
    void saveSlotAsync(int slot, std::function<void(bool)> done);
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
    std::atomic<float> uiSynthEnv{0.f};       // loudest synth voice envelope
    std::atomic<int> uiSynthNote{-1};
    std::atomic<float> uiVoiceEnv[eng::kSynthVoices] = {};  // the six voice lights
    std::atomic<int> uiDrumStep{-1};          // sequencer step on show, -1 stopped
    std::atomic<float> uiJackLevel[eng::kJacks] = {};  // OUT jack signal level (peak, slow release)
    std::atomic<uint32_t> uiDrumHits{0};      // voices hit since the panel last cleared it (fetch_or)
    std::atomic<float> uiDrumEnv[eng::kDrumVoices] = {};
    std::atomic<float> uiSpringShake{0.f};
    std::atomic<bool> uiGateOpen{false};      // IN THRESH gate open
    std::atomic<float> uiWowPhase{0.f};       // engine wow LFO phase, radians
    std::atomic<float> uiWowDepth{0.f};       // wow + flutter depth (fraction)

    // Scope tap: the synth output (48 kHz), written by the audio
    // thread into a fixed single-producer/single-consumer ring (no locks, no
    // allocation). The panel drains it on the message thread.
    static constexpr int kScopeSize = 8192;
    int readScope(float* dest, int maxSamples) noexcept;

    // Test access (not real-time safe to use while audio runs).
    TapeRuntime& runtime() noexcept { return *rt_; }
    eng::Instrument& instrument() noexcept { return inst_; }
    // Offline render at the tape rate (tests, demo renders): n samples, any n.
    void renderTape(float* outL, float* outR, int n) noexcept;
    // Standalone "Save current state...": the same bytes as a slot, the file written in the background.
    void saveStateToFileAsync(const juce::File& f, std::function<void(bool)> done);

private:
    void apply(const Command& c) noexcept;
    void ensureTracks();
    void renderInternal(float* outL, float* outR, int m) noexcept;  // any m, at 48 kHz, in 32-sample blocks
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
    eng::Instrument inst_;
    eng::PatchPlan plan_;
    float jackLvl_[eng::kJacks] = {};
    uint32_t patchSeq_ = 0xffffffffu;
    void transportBlock(float* eoc, int n) noexcept override;
    void headBlock(int t, const float* speedCv, bool rev, const float* scrubCv, float* head, int n) noexcept override;
    void inputBlock(const float* cableL, const float* cableR, float* outL, float* outR, int n) noexcept override;
    void mixBlock(const eng::TapeMixIo& io, int n) noexcept override;
    void jackPress() noexcept;
    void renderBlock32(float* outL, float* outR) noexcept;
    float blkL_[eng::kBlock] = {}, blkR_[eng::kBlock] = {};
    int blkPos_ = eng::kBlock;   // read position in the last rendered 32-sample block
    KnobSmoother fxSm_[4];
    float gateEnv_ = 0.f;        // IN THRESH gate
    int gateHold_ = 0;
    float biasKnob_ = 0.45f, driveKnob_ = 1.8f, threshKnob_ = 0.55f;
    eng::DrumKnobs drumKnobs_[eng::kDrumVoices];
    static int gmDrumVoice(int note) noexcept;  // MIDI channel 10 note -> drum voice
    bool gateOpen_ = false;
    bool userRev_ = false;       // REV as the panel set it (the REVERSE jack overrides while patched)
    bool cvRev_ = false;
    float head2Hist_[2] = {};
    juce::AbstractFifo scopeFifo_{kScopeSize};
    float scope_[kScopeSize] = {};
    void pushScope(const float* x, int n) noexcept;
    juce::HeapBlock<float> synth_;
    juce::HeapBlock<float> zero_;
    juce::HeapBlock<float> resample_;   // last chunk of tape output, for RESAMPLE
    juce::HeapBlock<float> undo_[2];    // what the last DROP wrote over
    int undoLo_ = 0, undoSpan_ = 0;     // the loop region it wrapped in
    juce::HeapBlock<uint8_t> passMark_; // overdub pass marks / backup (TapeRuntime::passMark)
    juce::HeapBlock<float> passBak_[2];
    int undoPassPos_ = -1;
    int lastPass_ = 0;              // UNDO PASS restoring in slices
    uint8_t undoPassId_ = 0;
    int undoPassTrack_ = 0;
    eng::CaptureRing capture_;
    eng::MasterLimiter limiter_;
    std::atomic<const eng::Clip*> pendingDrop_{nullptr};
    std::atomic<const eng::Clip*> keysClip_{nullptr};
    std::atomic<const eng::Clip*> audition_{nullptr};
    std::atomic<const eng::Clip*> keyClip_[128] = {};
    struct SliceKit {
        ClipPtr clip;
        int start[25] = {};
        int count = 0;
    };
    std::atomic<const SliceKit*> sliceKit_{nullptr};
    std::vector<std::shared_ptr<const SliceKit>> liveKits_;
    std::vector<ClipPtr> live_;         // placed clips stay alive (until the editor closes the session)
    std::vector<ClipPtr> pool_;
    int nextClipId_ = 1;
    void keepAlive(const ClipPtr& c);
    void playNote(int note, float vel) noexcept;
    void stopNote(int note) noexcept;
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
    // last member: destroyed first, so a save in flight finishes while the rest is alive
    juce::ThreadPool io_{1};
};

}  // namespace lift
