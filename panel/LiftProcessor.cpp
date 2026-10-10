// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "LiftProcessor.h"

#include "LiftEditor.h"

#include "audio/prepare.h"
#include "audio/process_block.h"
#include "tape/edit.h"
#include "tape/transport.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace lift {

namespace {
constexpr float kStopRampSeconds = 0.35f;
constexpr int kChunk = 512;
constexpr char kMagic[8] = {'L', 'I', 'F', 'T', 'S', 'L', 'O', 'T'};
constexpr int kTapeScreen = 2;

inline float catmull(const float* h, float t) noexcept {
    // 4-point Catmull-Rom between h[1] and h[2]
    const float a = -0.5f * h[0] + 1.5f * h[1] - 1.5f * h[2] + 0.5f * h[3];
    const float b = h[0] - 2.5f * h[1] + 2.f * h[2] - 0.5f * h[3];
    const float c = -0.5f * h[0] + 0.5f * h[2];
    return ((a * t + b) * t + c) * t + h[1];
}

bool littleEndian() noexcept {
    const uint16_t x = 1;
    uint8_t b = 0;
    std::memcpy(&b, &x, 1);
    return b == 1;
}

int trimmedLength(const float* x, int n) noexcept {
    while (n > 0 && x[n - 1] == 0.f) {
        --n;
    }
    return n;
}

void writeFloats(juce::MemoryOutputStream& out, const float* x, int n) {
    if (littleEndian()) {
        out.write(x, static_cast<size_t>(n) * sizeof(float));
    } else {
        for (int i = 0; i < n; ++i) {
            out.writeFloat(x[i]);
        }
    }
}

bool readFloats(juce::MemoryInputStream& in, std::vector<float>& v, int n) {
    if (n < 0 || in.getNumBytesRemaining() < static_cast<juce::int64>(n) * 4) {
        return false;
    }
    v.resize(static_cast<size_t>(n));
    if (littleEndian()) {
        return in.read(v.data(), n * 4) == n * 4;
    }
    for (int i = 0; i < n; ++i) {
        v[static_cast<size_t>(i)] = in.readFloat();
    }
    return true;
}

// Copy (resampling from `rate` to the 48 kHz tape rate) into dst[0..cap).
void putTape(const std::vector<float>& src, int rate, float* dst, int cap) {
    std::fill(dst, dst + cap, 0.f);
    if (src.empty()) {
        return;
    }
    if (rate == kSampleRate) {
        std::copy(src.begin(), src.begin() + juce::jmin(cap, static_cast<int>(src.size())), dst);
        return;
    }
    const double r = static_cast<double>(rate) / kSampleRate;  // input samples per output sample
    const int outN = juce::jmin(cap, static_cast<int>(std::floor(static_cast<double>(src.size()) / r)));
    if (outN <= 0) {
        return;
    }
    // pad the end so the interpolator never reads past the source
    std::vector<float> padded(src);
    padded.resize(src.size() + 8, 0.f);
    juce::LagrangeInterpolator li;
    li.process(r, padded.data(), dst, outN);
}

}  // namespace

// ------------------------------------------------------------------ voice

void PlaceholderVoice::noteOn(int n, float velocity) noexcept {
    int k = 0;
    for (int i = 0; i < depth; ++i) {
        if (held[i] != n) {
            held[k++] = held[i];
        }
    }
    depth = k;
    if (depth == 16) {
        for (int i = 1; i < 16; ++i) {
            held[i - 1] = held[i];
        }
        --depth;
    }
    held[depth++] = n;
    note = n;
    vel = juce::jlimit(0.05f, 1.f, velocity);
    if (!gate || depth == 1) {
        fenv = 1.f;  // the filter envelope starts on each new (non-legato) note
    }
    gate = true;
}

void PlaceholderVoice::noteOff(int n) noexcept {
    if (n < 0) {
        depth = 0;
    } else {
        int k = 0;
        for (int i = 0; i < depth; ++i) {
            if (held[i] != n) {
                held[k++] = held[i];
            }
        }
        depth = k;
        if (n != note) {
            return;
        }
    }
    if (depth > 0) {
        note = held[depth - 1];  // legato back to the last held note
    } else if (!sustain) {
        gate = false;
    }
}

void PlaceholderVoice::setSustain(bool on) noexcept {
    sustain = on;
    if (!on && depth == 0) {
        gate = false;
    }
}

void PlaceholderVoice::allOff() noexcept {
    depth = 0;
    sustain = false;
    gate = false;
}

void PlaceholderVoice::render(float* out, int n) noexcept {
    constexpr float sr = static_cast<float>(kSampleRate);
    const float att = 1.f - std::exp(-1.f / (0.004f * sr));
    // DECAY: release 0.18 s at the default (0.55), 18 ms .. 2.2 s
    const float relSec = 0.18f * std::exp2((macro[3] - 0.55f) * 6.f);
    const float rel = 1.f - std::exp(-1.f / (relSec * sr));
    const float fdec = 1.f - std::exp(-1.f / (juce::jmax(0.03f, relSec * 1.5f) * sr));
    // CUTOFF: 2.4 kHz at the default (0.62), 77 Hz .. 16 kHz; ENV AMT sweeps
    // it by up to +-3 octaves on each note (none at the default 0.48)
    const float cutBase = 2400.f * std::exp2((macro[1] - 0.62f) * 8.f);
    const float envOct = (macro[2] - 0.48f) * 6.f;
    // DETUNE: the square runs 0.4 % sharp at the default (0.35), up to 3 %
    const double m0 = static_cast<double>(macro[0]) / 0.35;
    const double ratio2 = 1.0 + 0.004 * m0 * m0;
    const double base = note >= 0 ? 440.0 * std::pow(2.0, (note - 69 + static_cast<double>(bend)) / 12.0) / kSampleRate : 0.0;
    const double vibInc = 5.5 / kSampleRate;
    const float vibDepth = 0.5f * mod;  // up to +-0.5 semitone
    float lpA = 0.f;
    for (int i = 0; i < n; ++i) {
        env += (gate ? att : rel) * ((gate ? 1.f : 0.f) - env);
        fenv += fdec * (0.f - fenv);
        if (env < 1e-5f && !gate) {
            env = 0.f;
            out[i] = 0.f;
            continue;
        }
        if ((i & 7) == 0) {  // filter coefficient at control rate
            const float fc = juce::jlimit(40.f, 16000.f, cutBase * std::exp2(envOct * fenv));
            lpA = 1.f - std::exp(-2.f * 3.14159265f * fc / sr);
        }
        double inc = base;
        if (vibDepth > 0.f) {
            vibPhase += vibInc;
            if (vibPhase >= 1.0) {
                vibPhase -= 1.0;
            }
            inc *= 1.0 + 0.0577623 * vibDepth * std::sin(6.283185307179586 * vibPhase);  // ln2/12 per semitone
        }
        phase += inc;
        if (phase >= 1.0) {
            phase -= 1.0;
        }
        phase2 += inc * ratio2;
        if (phase2 >= 1.0) {
            phase2 -= 1.0;
        }
        const float saw = static_cast<float>(2.0 * phase - 1.0);
        const float sq = phase2 < 0.5 ? 0.6f : -0.6f;
        lp += lpA * ((saw + sq) * 0.5f - lp);
        out[i] = lp * env * 0.5f * (0.25f + 0.75f * vel);
    }
}

// ------------------------------------------------------------------ setup

LiftProcessor::LiftProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      rt_(std::make_unique<TapeRuntime>()) {
    transport_init(*rt_);
    rt_->mute[2] = true;  // the panel opens with M3 lit
    rt_->loopStart = 0;
    rt_->loopEnd = kLoopSeconds * kSampleRate;
    intL_.allocate(kChunk, true);
    intR_.allocate(kChunk, true);
    synth_.allocate(kChunk, true);
    zero_.allocate(kChunk, true);
    resample_.allocate(kChunk, true);
    monL_.allocate(kChunk, true);
    monR_.allocate(kChunk, true);
    tape_engine_init(monitor_);
    for (auto& k : knobs) {
        k.store(0.f);
    }
    setAllKnobs(ui_.enc);
    for (int i = 0; i < 20; ++i) {
        sm_[i].snap(knobs[i].load());
    }
    if (wrapperType == wrapperType_Standalone) {
        reopenLast();  // on launch the standalone reopens the last saved slot
    }
    startTimerHz(30);
}

LiftProcessor::~LiftProcessor() {
    stopTimer();
    release_tracks(*rt_);
    tape_engine_release(monitor_);
}

bool LiftProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LiftProcessor::ensureTracks() {
    if (tracksReady_) {
        return;
    }
    prepare_tracks(*rt_, kTapeSeconds * kSampleRate);
    tape_engine_prepare(monitor_, static_cast<double>(kSampleRate));
    tape_engine_set_character(monitor_, character_row(rt_->character));
    lastBias_ = -1.f;
    if (rt_->loopEnd > rt_->frames) {
        rt_->loopEnd = rt_->frames;
    }
    undo_[0].allocate(static_cast<size_t>(rt_->frames), true);
    undo_[1].allocate(static_cast<size_t>(rt_->frames), true);
    tracksReady_ = true;
    uiFrames.store(rt_->frames);
}

void LiftProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    // The tape, its engine and the placeholder voice always run at the tape
    // rate (48 kHz); the output is resampled to the device rate, so pitch and
    // tape length are the same at 44.1, 48, 88.2 or 96 kHz.
    ensureTracks();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    ratio_ = kSampleRate / sampleRate_;
    if (std::abs(ratio_ - 1.0) < 1e-9) {
        ratio_ = 1.0;
    }
    frac_ = 0.0;
    std::memset(hist_, 0, sizeof(hist_));
    intPos_ = intLen_ = 0;
    sampleCount_ = 0;
    lastClock_ = -1;
    clockN_ = clockW_ = 0;
    clockOutPhase_ = 0.0;
    for (auto& k : sm_) {
        k.prepare(static_cast<double>(kSampleRate));
    }
    for (int i = 0; i < 20; ++i) {
        sm_[i].snap(knobs[i].load());
    }
    scrubPrev_ = -1.f;
    scrubPending_ = 0.0;
    scrubGain_ = 0.f;
    appliedArm_ = -1;
    lastBias_ = -1.f;
    prepared_ = true;
}

void LiftProcessor::send(Cmd c, int a, int b, double v) noexcept {
    const auto scope = fifo_.write(1);
    if (scope.blockSize1 > 0) {
        cmds_[scope.startIndex1] = Command{c, a, b, v};
    } else if (scope.blockSize2 > 0) {
        cmds_[scope.startIndex2] = Command{c, a, b, v};
    }
}

void LiftProcessor::post(uint8_t kind, int a, int b) noexcept {
    const auto w = midiFifo_.write(1);
    if (w.blockSize1 > 0) {
        midiEvents_[w.startIndex1] = MidiEvent{kind, a, b};
    } else if (w.blockSize2 > 0) {
        midiEvents_[w.startIndex2] = MidiEvent{kind, a, b};
    }
}

void LiftProcessor::pushScope(const float* x, int n) noexcept {
    const auto w = scopeFifo_.write(juce::jmin(n, scopeFifo_.getFreeSpace()));
    for (int k = 0; k < w.blockSize1; ++k) {
        scope_[w.startIndex1 + k] = x[k];
    }
    for (int k = 0; k < w.blockSize2; ++k) {
        scope_[w.startIndex2 + k] = x[w.blockSize1 + k];
    }
}

int LiftProcessor::readScope(float* dest, int maxSamples) noexcept {
    const auto r = scopeFifo_.read(juce::jmin(maxSamples, scopeFifo_.getNumReady()));
    for (int k = 0; k < r.blockSize1; ++k) {
        dest[k] = scope_[r.startIndex1 + k];
    }
    for (int k = 0; k < r.blockSize2; ++k) {
        dest[r.blockSize1 + k] = scope_[r.startIndex2 + k];
    }
    return r.blockSize1 + r.blockSize2;
}

void LiftProcessor::setTapeKnobs(const std::array<float, 4>& e) noexcept {
    speed.store(0.25f * std::pow(16.f, e[0]));
    bias.store(e[1]);
    drive.store(1.8f * std::pow(16.f, e[2] - 0.7f));
    for (int k = 0; k < 4; ++k) {
        knobs[kTapeScreen * 4 + k].store(juce::jlimit(0.f, 1.f, e[static_cast<size_t>(k)]), std::memory_order_relaxed);
    }
}

void LiftProcessor::setKnobValue(int screen, int k, float v) noexcept {
    if (screen < 0 || screen >= 5 || k < 0 || k >= 4) {
        return;
    }
    v = juce::jlimit(0.f, 1.f, v);
    knobs[screen * 4 + k].store(v, std::memory_order_relaxed);
    if (screen == kTapeScreen) {
        if (k == 0) {
            speed.store(0.25f * std::pow(16.f, v));
        } else if (k == 1) {
            bias.store(v);
        } else if (k == 2) {
            drive.store(1.8f * std::pow(16.f, v - 0.7f));
        }
    }
}

void LiftProcessor::setAllKnobs(const std::array<std::array<float, 4>, 5>& enc) noexcept {
    for (int sc = 0; sc < 5; ++sc) {
        for (int k = 0; k < 4; ++k) {
            setKnobValue(sc, k, enc[static_cast<size_t>(sc)][static_cast<size_t>(k)]);
        }
    }
}

// Per control-rate step (32 samples): smoothed knobs -> engine, voice, mix.
void LiftProcessor::stepKnobs(int n) noexcept {
    TapeRuntime& rt = *rt_;
    float v[20];
    for (int i = 0; i < 20; ++i) {
        sm_[i].setTarget(knobs[i].load(std::memory_order_relaxed));
        v[i] = sm_[i].advance(n);
    }
    // TAPE: BIAS and REC LVL into the record chain (tape and monitor) and the
    // playback gap loss; SPEED goes to the transport per block (the engine
    // glides it), SCRUB per block too.
    const float b = v[kTapeScreen * 4 + 1];
    const float d = 1.8f * std::pow(16.f, v[kTapeScreen * 4 + 2] - 0.7f);
    if (std::abs(b - lastBias_) > 1e-5f || std::abs(d - lastDrive_) > 1e-5f * d) {
        TapeParams p = rt.engine.p;
        p.bias = b;
        p.drive = d;
        tape_engine_set_params(rt.engine, p);
        TapeParams q = monitor_.p;
        q.bias = b;
        q.drive = d;
        tape_engine_set_params(monitor_, q);
        lastBias_ = b;
        lastDrive_ = d;
    }
    // SYNTH: the placeholder voice's macros
    for (int k = 0; k < 4; ++k) {
        voice_.macro[k] = v[0 * 4 + k];
    }
    // MIX: level / pan / low / high of the armed track
    const int arm = juce::jlimit(0, kTrackCount - 1, rt.arm);
    const float* mx = v + 3 * 4;
    if (arm != appliedArm_ || mx[0] != appliedMix_[0] || mx[1] != appliedMix_[1] || mx[2] != appliedMix_[2] ||
        mx[3] != appliedMix_[3]) {
        const float lv = mx[0] / 0.72f;  // unity at the default
        rt.fader[arm] = lv * lv;
        rt.panL[arm] = juce::jmin(1.f, 2.f * (1.f - mx[1]));
        rt.panR[arm] = juce::jmin(1.f, 2.f * mx[1]);
        rt.lowG[arm] = std::pow(10.f, (mx[2] - 0.55f) * 24.f / 20.f) - 1.f;   // +-12 dB around the default
        rt.highG[arm] = std::pow(10.f, (mx[3] - 0.45f) * 24.f / 20.f) - 1.f;
        if (std::abs(rt.lowG[arm]) < 1e-4f) {
            rt.lowG[arm] = 0.f;
        }
        if (std::abs(rt.highG[arm]) < 1e-4f) {
            rt.highG[arm] = 0.f;
        }
        for (int k = 0; k < 4; ++k) {
            appliedMix_[k] = mx[k];
        }
        appliedArm_ = arm;
    }
    // IN: GAIN on the record source and its monitor (unity at the default)
    const float g = v[4 * 4 + 2] / 0.6f;
    inGainPrev_ = inGain_;
    inGain_ = g * g;
}

// ------------------------------------------------------------------ commands

void LiftProcessor::apply(const Command& c) noexcept {
    TapeRuntime& rt = *rt_;
    switch (c.cmd) {
    case Cmd::Transport: {
        const bool play = c.a != 0;
        const bool rec = c.b != 0;
        if (play) {
            if (!rt.playing) {
                transport_tape_start(rt, 0.f);
            }
            rt.playing = true;
            rt.recording = rec;
        } else {
            rt.recording = false;
            transport_tape_stop(rt, kStopRampSeconds);
        }
        break;
    }
    case Cmd::Stop:
        rt.reverse = false;
        rt.recording = false;
        transport_tape_stop(rt, kStopRampSeconds);
        break;
    case Cmd::Lift:
        tape_lift(rt);
        break;
    case Cmd::Drop: {
        // keep what is about to be overdubbed so UNDO can put it back
        const int start = static_cast<int>(rt.pos);
        const int n = juce::jmin(rt.clipFrames, rt.frames - start);
        if (undo_[0] != nullptr && n > 0 && start >= 0) {
            for (int c = 0; c < 2; ++c) {
                const float* src = rt.ch[rt.arm][c] + start;
                float* dst = undo_[c].get();
                for (int i = 0; i < n; ++i) {
                    dst[i] = src[i];
                }
            }
            undoStart_ = start;
            undoFrames_ = n;
            undoTrack_ = rt.arm;
        }
        tape_drop(rt, true);
        break;
    }
    case Cmd::UndoDrop:
        if (undoStart_ >= 0) {
            for (int c = 0; c < 2; ++c) {
                float* dst = rt.ch[undoTrack_][c] + undoStart_;
                const float* src = undo_[c].get();
                for (int i = 0; i < undoFrames_; ++i) {
                    dst[i] = src[i];
                }
            }
            undoStart_ = -1;
        }
        break;
    case Cmd::LoopSet:
        rt.loopStart = juce::jlimit(0, juce::jmax(0, rt.frames - 1), c.a);
        rt.loopEnd = c.b > rt.loopStart ? juce::jmin(c.b, rt.frames) : 0;
        if (rt.loopEnd > rt.loopStart && (rt.pos < rt.loopStart || rt.pos >= rt.loopEnd)) {
            rt.pos = rt.loopStart;
        }
        break;
    case Cmd::HardStop:
        transport_stop(rt);
        rt.engine.ramp = 0.f;
        break;
    case Cmd::LiftAll: {
        int start = 0, end = rt.frames;
        if (rt.loopEnd > rt.loopStart) {
            start = rt.loopStart;
            end = juce::jmin(rt.loopEnd, rt.frames);
        }
        const int n = end - start;
        if (rt.clip[0] != nullptr && n > 0 && n <= rt.frames) {
            for (int c = 0; c < 2; ++c) {
                float* dst = rt.clip[c];
                for (int i = 0; i < n; ++i) {
                    float sum = 0.f;
                    for (int t = 0; t < kTrackCount; ++t) {
                        sum += rt.ch[t][c][start + i];
                    }
                    dst[i] = sum;
                }
            }
            rt.clipFrames = n;
        }
        break;
    }
    case Cmd::Clear:
        if (c.a >= 0 && c.a < kTrackCount) {
            clearTrack_ = c.a;
            clearPos_ = 0;
        }
        break;
    case Cmd::Jump:
        if (rt.frames > 0) {
            rt.pos = juce::jlimit(0.0, static_cast<double>(rt.frames - 1), c.v);
        }
        break;
    case Cmd::Character:
        transport_bind_character(rt, c.a);
        tape_engine_set_character(monitor_, character_row(rt.character));
        lastBias_ = -1.f;  // re-apply the BIAS and REC LVL knobs over the row
        break;
    case Cmd::RecSource:
        recSource_ = juce::jlimit(0, 2, c.a);
        break;
    case Cmd::Rev:
        rt.reverse = c.a != 0;
        break;
    case Cmd::Loop:
        rt.loopStart = 0;
        rt.loopEnd = c.a != 0 ? juce::jmin(kLoopSeconds * kSampleRate, rt.frames) : 0;
        break;
    case Cmd::Arm:
        if (c.a >= 0 && c.a < kTrackCount) {
            rt.arm = c.a;
        }
        break;
    case Cmd::Mute:
        if (c.a >= 0 && c.a < kTrackCount) {
            rt.mute[c.a] = c.b != 0;
        }
        break;
    case Cmd::Seek:
        if (!rt.playing && !rt.recording && rt.frames > 0) {
            rt.pos = juce::jlimit(0.0, static_cast<double>(rt.frames - 1), c.v);
        }
        break;
    case Cmd::NoteOn: {
        const int vel = c.b > 0 ? juce::jmin(127, c.b) : 100;
        voice_.noteOn(c.a, static_cast<float>(vel) / 127.f);
        if (outNoteCount_ < 32) {
            outNotes_[outNoteCount_++] = (c.a & 0x7f) | (vel << 8);
        }
        break;
    }
    case Cmd::NoteOff:
        voice_.noteOff(c.a);
        if (outNoteCount_ < 32) {
            outNotes_[outNoteCount_++] = c.a >= 0 ? -1 - (c.a & 0x7f) : -1000;
        }
        break;
    }
}

// ------------------------------------------------------------------ MIDI in

void LiftProcessor::handleMidiIn(const juce::MidiMessage& m, int samplePos) noexcept {
    TapeRuntime& rt = *rt_;
    if (m.isNoteOn()) {
        voice_.noteOn(m.getNoteNumber(), static_cast<float>(m.getVelocity()) / 127.f);
    } else if (m.isNoteOff()) {
        voice_.noteOff(m.getNoteNumber());
    } else if (m.isPitchWheel()) {
        voice_.bend = 2.f * static_cast<float>(m.getPitchWheelValue() - 8192) / 8192.f;
    } else if (m.isController()) {
        const int cc = m.getControllerNumber(), v = m.getControllerValue();
        if (cc == 1) {
            voice_.mod = static_cast<float>(v) / 127.f;
        } else if (cc == 64) {
            voice_.setSustain(v >= 64);
        } else if (cc == 0) {
            bankMsb_ = v;
        } else if (cc == 120 || cc == 123) {
            voice_.allOff();
        } else if (cc != 32) {
            post(0, cc, v);
        }
    } else if (m.isProgramChange()) {
        post(1, bankMsb_ * 128 + m.getProgramChangeNumber() + 1, 0);
    } else if (m.isMidiClock()) {
        const juce::int64 now = sampleCount_ + samplePos;
        if (lastClock_ >= 0) {
            const double dt = static_cast<double>(now - lastClock_) / sampleRate_;
            if (dt > 0.0 && dt < 0.25) {  // 10 .. 2500 BPM
                clockIntervals_[clockW_] = dt;
                clockW_ = (clockW_ + 1) % 24;
                clockN_ = juce::jmin(24, clockN_ + 1);
                double sum = 0.0;
                for (int k = 0; k < clockN_; ++k) {
                    sum += clockIntervals_[k];
                }
                tempoBpm.store(60.0 / (24.0 * sum / clockN_), std::memory_order_relaxed);
                clockSlaved.store(true, std::memory_order_relaxed);
            } else {
                clockN_ = clockW_ = 0;
            }
        }
        lastClock_ = now;
    } else if (m.isMidiStart() || m.isMidiContinue()) {
        if (m.isMidiStart()) {
            rt.pos = rt.loopEnd > rt.loopStart ? rt.loopStart : 0;
        }
        if (!rt.playing) {
            transport_tape_start(rt, 0.f);
        }
        rt.playing = true;
        rt.recording = false;
        rt.reverse = false;
        midiPlay.store(true, std::memory_order_relaxed);
        transportSerial.fetch_add(1, std::memory_order_release);
    } else if (m.isMidiStop()) {
        rt.recording = false;
        rt.reverse = false;
        transport_tape_stop(rt, kStopRampSeconds);
        midiPlay.store(false, std::memory_order_relaxed);
        transportSerial.fetch_add(1, std::memory_order_release);
    } else if (m.isSongPositionPointer()) {
        if (!rt.playing && rt.frames > 0) {
            // 1 beat = 4 positions; tape time at the current tempo
            const double beats = m.getSongPositionPointerMidiBeat() / 4.0;
            const double secs = beats * 60.0 / tempoBpm.load(std::memory_order_relaxed);
            rt.pos = juce::jlimit(0.0, static_cast<double>(rt.frames - 1), secs * kSampleRate);
        }
    }
}

// ------------------------------------------------------------------ render

void LiftProcessor::renderInternal(float* outL, float* outR, int m) noexcept {
    TapeRuntime& rt = *rt_;
    if (clearTrack_ >= 0) {
        // CLEAR in slices so no block does all of it
        const int end = juce::jmin(rt.frames, clearPos_ + 65536);
        for (int c = 0; c < 2; ++c) {
            float* x = rt.ch[clearTrack_][c];
            for (int i = clearPos_; i < end; ++i) {
                x[i] = 0.f;
            }
        }
        clearPos_ = end;
        if (clearPos_ >= rt.frames) {
            clearTrack_ = -1;
        }
    }
    constexpr int kSub = 32;  // knob control rate: 0.67 ms
    for (int off = 0; off < m; off += kSub) {
        const int n = juce::jmin(kSub, m - off);
        stepKnobs(n);
        renderSub(outL + off, outR + off, off, n);
    }
}

void LiftProcessor::renderSub(float* outL, float* outR, int off, int m) noexcept {
    TapeRuntime& rt = *rt_;
    float* s = synth_.get() + off;
    voice_.render(s, m);
    pushScope(s, m);
    // IN GAIN, ramped across the sub-block
    if (inGain_ != 1.f || inGainPrev_ != 1.f) {
        const float step = (inGain_ - inGainPrev_) / static_cast<float>(m);
        float gg = inGainPrev_;
        for (int i = 0; i < m; ++i) {
            gg += step;
            s[i] *= gg;
        }
    }
    // record source: the placeholder voice, the (not yet built) input, or the tape output
    const float* in = recSource_ == 0 ? s : recSource_ == 1 ? zero_.get() + off : resample_.get() + off;
    if (rt.playing && rt.frames > 0) {
        // Track meters: peak of what is on each track under the head.
        const int p0 = juce::jlimit(0, rt.frames - 1, static_cast<int>(rt.pos));
        const int p1 = juce::jmin(rt.frames, p0 + m);
        for (int t = 0; t < kTrackCount && t < 4; ++t) {
            float pk = 0.f;
            if (!rt.mute[t]) {
                const float* x = rt.ch[t][0];
                for (int i = p0; i < p1; ++i) {
                    pk = juce::jmax(pk, x[i] < 0.f ? -x[i] : x[i]);
                }
            }
            trackPeak_[t] = juce::jmax(trackPeak_[t], pk);
        }
    }
    process_block(rt, in, in, outL, outR, m);
    // SCRUB by hand while the transport is stopped: the heads read the tape
    // as it moves, louder the faster it goes, silent when it stands still
    if (!rt.playing && !rt.recording && rt.frames > 0 && (scrubPending_ != 0.0 || scrubGain_ > 0.f)) {
        const double a = 1.0 - std::exp(-1.0 / (0.06 * kSampleRate));  // reel inertia, 60 ms
        const float ga = 1.f - std::exp(-1.f / (0.004f * static_cast<float>(kSampleRate)));
        for (int i = 0; i < m; ++i) {
            double st = scrubPending_ * a;
            if (std::abs(scrubPending_) < 0.01) {
                st = scrubPending_;
            }
            scrubPending_ -= st;
            transport_scrub(rt, st);
            const float target = juce::jmin(1.f, static_cast<float>(std::abs(st)) * 3.f);
            scrubGain_ += ga * (target - scrubGain_);
            if (scrubGain_ < 1e-4f && target == 0.f) {
                scrubGain_ = 0.f;
            }
            float l = 0.f, r = 0.f;
            for (int t = 0; t < kTrackCount; ++t) {
                if (rt.mute[t] || rt.ch[t][0] == nullptr) {
                    continue;
                }
                const float gt = rt.fader[t] * scrubGain_;
                l += read_looped(rt.ch[t][0], rt.frames, rt.pos, rt.loopStart, rt.loopEnd, false) * gt * rt.panL[t];
                r += read_looped(rt.ch[t][1], rt.frames, rt.pos, rt.loopStart, rt.loopEnd, false) * gt * rt.panR[t];
            }
            outL[i] += l;
            outR[i] += r;
        }
    }
    for (int i = 0; i < m; ++i) {
        resample_[static_cast<size_t>(off + i)] = outL[i];
    }
    // input monitor through the record electronics (REC LVL, BIAS)
    float* ml = monL_.get();
    float* mr = monR_.get();
    tape_engine_record(monitor_, s, s, ml, mr, m);
    for (int i = 0; i < m; ++i) {
        outL[i] += ml[i];
        if (outR != outL) {
            outR[i] += mr[i];
        }
    }
}

void LiftProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    TapeRuntime& rt = *rt_;
    const int n = buffer.getNumSamples();
    outNoteCount_ = 0;
    {
        const auto scope = fifo_.read(fifo_.getNumReady());
        for (int k = 0; k < scope.blockSize1; ++k) {
            apply(cmds_[scope.startIndex1 + k]);
        }
        for (int k = 0; k < scope.blockSize2; ++k) {
            apply(cmds_[scope.startIndex2 + k]);
        }
    }
    for (const auto meta : midi) {
        handleMidiIn(meta.getMessage(), meta.samplePosition);
    }
    if (lastClock_ >= 0 && static_cast<double>(sampleCount_ + n - lastClock_) > 0.5 * sampleRate_) {
        clockSlaved.store(false, std::memory_order_relaxed);  // the clock went away
        lastClock_ = -1;
        clockN_ = clockW_ = 0;
    }

    // ---- MIDI out: panel notes, then clock while the tape runs (not when slaved)
    midi.clear();
    for (int k = 0; k < outNoteCount_; ++k) {
        const int e = outNotes_[k];
        if (e >= 0) {
            midi.addEvent(juce::MidiMessage::noteOn(1, e & 0x7f, static_cast<juce::uint8>(e >> 8)), 0);
        } else if (e == -1000) {
            midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        } else {
            midi.addEvent(juce::MidiMessage::noteOff(1, -1 - e), 0);
        }
    }
    const bool slaved = clockSlaved.load(std::memory_order_relaxed);
    if (rt.playing != wasPlaying_) {
        if (!slaved) {
            midi.addEvent(rt.playing ? juce::MidiMessage::midiStart() : juce::MidiMessage::midiStop(), 0);
        }
        wasPlaying_ = rt.playing;
        clockOutPhase_ = 0.0;
    }
    if (rt.playing && !slaved) {
        const double per = sampleRate_ * 60.0 / (24.0 * juce::jmax(20.0, tempoBpm.load(std::memory_order_relaxed)));
        while (clockOutPhase_ < n) {
            midi.addEvent(juce::MidiMessage::midiClock(), juce::jmax(0, static_cast<int>(clockOutPhase_)));
            clockOutPhase_ += per;
        }
        clockOutPhase_ -= n;
    }
    sampleCount_ += n;

    // SCRUB: knob movement since the last block becomes tape to move. Stopped,
    // the heads play it as the reels turn by hand (renderSub); playing, it
    // nudges the speed like a jog wheel.
    {
        const float k = knobs[kTapeScreen * 4 + 3].load(std::memory_order_relaxed);
        if (scrubPrev_ >= 0.f && k != scrubPrev_) {
            scrubPending_ += static_cast<double>(k - scrubPrev_) * kScrubSeconds * kSampleRate;
        }
        scrubPrev_ = k;
    }
    float vs = speed.load(std::memory_order_relaxed);
    if (rt.playing && scrubPending_ != 0.0) {
        const double inner = static_cast<double>(n) * ratio_;  // tape-rate samples this block
        const double take = scrubPending_ * (1.0 - std::exp(-inner / (0.06 * kSampleRate)));
        scrubPending_ -= take;
        if (std::abs(scrubPending_) < 0.5) {
            scrubPending_ = 0.0;
        }
        vs += static_cast<float>(take / juce::jmax(1.0, inner));
    }
    transport_set_varispeed(rt, vs);

    for (int t = 0; t < 4; ++t) {
        trackPeak_[t] = 0.f;
    }
    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : outL;
    if (!prepared_ || !tracksReady_) {
        buffer.clear();
    } else if (ratio_ == 1.0) {
        for (int start = 0; start < n; start += kChunk) {
            const int m = juce::jmin(kChunk, n - start);
            renderInternal(outL + start, outR + start, m);
        }
    } else {
        // 48 kHz chunks, interpolated to the device rate
        float* iL = intL_.get();
        float* iR = intR_.get();
        for (int i = 0; i < n; ++i) {
            while (frac_ >= 1.0) {
                if (intPos_ >= intLen_) {
                    renderInternal(iL, iR, kChunk);
                    intPos_ = 0;
                    intLen_ = kChunk;
                }
                for (int c = 0; c < 2; ++c) {
                    hist_[c][0] = hist_[c][1];
                    hist_[c][1] = hist_[c][2];
                    hist_[c][2] = hist_[c][3];
                }
                hist_[0][3] = iL[intPos_];
                hist_[1][3] = iR[intPos_];
                ++intPos_;
                frac_ -= 1.0;
            }
            const float t = static_cast<float>(frac_);
            outL[i] = catmull(hist_[0], t);
            if (outR != outL) {
                outR[i] = catmull(hist_[1], t);
            }
            frac_ += ratio_;
        }
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch) {
        buffer.clear(ch, 0, n);
    }
    for (int t = 0; t < 4; ++t) {
        uiTrackLevel[t].store(trackPeak_[t], std::memory_order_relaxed);
    }
    uiMasterLevel.store(buffer.getMagnitude(0, 0, n), std::memory_order_relaxed);
    uiSynthEnv.store(voice_.env, std::memory_order_relaxed);
    uiSynthNote.store(voice_.gate ? voice_.note : -1, std::memory_order_relaxed);
    uiWowPhase.store(static_cast<float>(rt.engine.wowPhase), std::memory_order_relaxed);
    uiWowDepth.store(rt.engine.p.wowDepth + rt.engine.p.flutterDepth, std::memory_order_relaxed);
    uiLoopStart.store(rt.loopStart, std::memory_order_relaxed);
    uiLoopEnd.store(rt.loopEnd, std::memory_order_relaxed);
    uiClipFrames.store(rt.clipFrames, std::memory_order_relaxed);
    uiPos.store(rt.pos, std::memory_order_relaxed);
    uiPlaying.store(rt.playing, std::memory_order_relaxed);
    uiRecording.store(rt.recording, std::memory_order_relaxed);
    float reel = 0.f;
    if (rt.recording) {
        reel = 1.f;
    } else if (rt.playing) {
        reel = rt.engine.speedNow * rt.engine.ramp * (rt.reverse ? -1.f : 1.f);
    }
    uiReelSpeed.store(reel, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ MIDI -> message thread

bool LiftProcessor::reservedCc(int cc) noexcept {
    return cc == 0 || cc == 1 || cc == 32 || cc == 64 || cc >= 120;
}

int LiftProcessor::fixedCcTarget(int cc, int screen) noexcept {
    if (cc >= 20 && cc <= 23) {
        return juce::jlimit(0, 4, screen) * 4 + (cc - 20);  // the screen on show
    }
    if (cc >= 24 && cc <= 27) {
        return 2 * 4 + (cc - 24);  // TAPE: SPEED, BIAS, REC LVL, SCRUB
    }
    if (cc >= 28 && cc <= 31) {
        return 0 * 4 + (cc - 28);  // SYNTH
    }
    if (cc >= 102 && cc <= 105) {
        return 1 * 4 + (cc - 102);  // DRUM
    }
    if (cc >= 106 && cc <= 109) {
        return 3 * 4 + (cc - 106);  // MIX
    }
    if (cc >= 110 && cc <= 113) {
        return 4 * 4 + (cc - 110);  // IN
    }
    return -1;
}

void LiftProcessor::setKnob(int target, float value) {
    if (target < 0 || target >= UiState::kKnobTargets) {
        return;
    }
    value = juce::jlimit(0.f, 1.f, value);
    if (listener_ != nullptr) {
        listener_->knobFromMidi(target, value);  // the panel sets it as if turned
        return;
    }
    const int screen = target / 4, k = target % 4;
    ui_.enc[static_cast<size_t>(screen)][static_cast<size_t>(k)] = value;
    setKnobValue(screen, k, value);
}

void LiftProcessor::timerCallback() {
    const int pending = pendingLoaded_.exchange(0);
    if (pending > 0 && listener_ != nullptr) {
        listener_->stateLoaded(pending - 1);
    }
    drainMidiEvents();
}

void LiftProcessor::drainMidiEvents() {
    const int ready = midiFifo_.getNumReady();
    if (ready == 0) {
        return;
    }
    MidiEvent ev[512];
    const auto r = midiFifo_.read(ready);
    int n = 0;
    for (int k = 0; k < r.blockSize1; ++k) {
        ev[n++] = midiEvents_[r.startIndex1 + k];
    }
    for (int k = 0; k < r.blockSize2; ++k) {
        ev[n++] = midiEvents_[r.startIndex2 + k];
    }
    for (int k = 0; k < n; ++k) {
        const MidiEvent& e = ev[k];
        if (e.kind == 1) {
            const int slot = e.a;
            const auto res = loadSlot(slot);
            if (listener_ != nullptr && res != LoadResult::Ok) {
                listener_->slotMessage(res == LoadResult::BadSlot ? "NO SLOT " + juce::String(slot)
                                                                  : "SLOT " + juce::String(slot).paddedLeft('0', 3) + " EMPTY");
            }
            continue;
        }
        const int cc = e.a;
        const int armed = learnTarget.load();
        if (armed >= 0 && !reservedCc(cc)) {
            learnTarget.store(-1);
            for (auto& t : ui_.learn) {
                if (t == armed) {
                    t = -1;  // one CC per knob
                }
            }
            ui_.learn[static_cast<size_t>(cc)] = armed;
            if (listener_ != nullptr) {
                listener_->learned(cc, armed);
            }
            continue;
        }
        int target = ui_.learn[static_cast<size_t>(cc)];
        if (target < 0) {
            target = fixedCcTarget(cc, ui_.mode);
        }
        setKnob(target, static_cast<float>(e.b) / 127.f);
    }
}

// ------------------------------------------------------------------ state

void LiftProcessor::setUiState(const UiState& s) {
    ui_ = s;
    setAllKnobs(s.enc);
}

juce::MemoryBlock LiftProcessor::saveState() {
    if (listener_ != nullptr) {
        listener_->flushState();
    }
    ensureTracks();
    juce::MemoryOutputStream out;
    out.write(kMagic, 8);
    out.writeInt(kStateVersion);
    const juce::String xml = ui_.toTree().toXmlString();
    out.writeInt(static_cast<int>(xml.getNumBytesAsUTF8()));
    out.write(xml.toRawUTF8(), xml.getNumBytesAsUTF8());
    suspendProcessing(true);
    const TapeRuntime& rt = *rt_;
    out.writeInt(kSampleRate);
    out.writeInt(rt.frames);
    out.writeDouble(rt.pos);
    out.writeInt(kTrackCount);
    int len[kTrackCount][2];
    size_t total = 0;
    for (int t = 0; t < kTrackCount; ++t) {
        for (int c = 0; c < 2; ++c) {
            len[t][c] = trimmedLength(rt.ch[t][c], rt.frames);
            total += static_cast<size_t>(len[t][c]) * 4 + 4;
        }
    }
    const int clipN = juce::jlimit(0, rt.frames, rt.clipFrames);
    out.preallocate(out.getDataSize() + total + static_cast<size_t>(clipN) * 8 + 64);
    for (int t = 0; t < kTrackCount; ++t) {
        for (int c = 0; c < 2; ++c) {
            out.writeInt(len[t][c]);
            writeFloats(out, rt.ch[t][c], len[t][c]);
        }
    }
    out.writeInt(clipN);  // the LIFT clipboard
    for (int c = 0; c < 2; ++c) {
        writeFloats(out, rt.clip[c], clipN);
    }
    suspendProcessing(false);
    return out.getMemoryBlock();
}

LiftProcessor::LoadResult LiftProcessor::loadState(const void* data, size_t size, int slot) {
    juce::MemoryInputStream in(data, size, false);
    char magic[8] = {};
    if (size < 16 || in.read(magic, 8) != 8 || std::memcmp(magic, kMagic, 8) != 0) {
        return LoadResult::BadData;
    }
    const int version = in.readInt();
    if (version > kStateVersion) {
        return LoadResult::NewerVersion;
    }
    if (version < 1) {
        return LoadResult::BadData;
    }
    const int xmlLen = in.readInt();
    if (xmlLen <= 0 || xmlLen > in.getNumBytesRemaining()) {
        return LoadResult::BadData;
    }
    juce::MemoryBlock xmlBytes;
    in.readIntoMemoryBlock(xmlBytes, xmlLen);
    const auto xml = juce::parseXML(xmlBytes.toString());
    if (xml == nullptr) {
        return LoadResult::BadData;
    }
    UiState s = UiState::fromTree(juce::ValueTree::fromXml(*xml));
    const int rate = in.readInt();
    const int frames = in.readInt();
    const double pos = in.readDouble();
    const int tracks = in.readInt();
    if (rate < 8000 || rate > 384000 || frames < 0 || tracks < 0 || tracks > kTrackCount) {
        return LoadResult::BadData;
    }
    std::vector<float> audio[kTrackCount][2];
    for (int t = 0; t < tracks; ++t) {
        for (int c = 0; c < 2; ++c) {
            const int n = in.readInt();
            if (n > frames || !readFloats(in, audio[t][c], n)) {
                return LoadResult::BadData;
            }
        }
    }
    std::vector<float> clip[2];
    const int clipN = in.readInt();
    if (clipN < 0 || clipN > frames || !readFloats(in, clip[0], clipN) || !readFloats(in, clip[1], clipN)) {
        return LoadResult::BadData;
    }

    // all parsed: now replace the running state
    ensureTracks();
    suspendProcessing(true);
    TapeRuntime& rt = *rt_;
    const double toTape = static_cast<double>(kSampleRate) / rate;
    for (int t = 0; t < kTrackCount; ++t) {
        for (int c = 0; c < 2; ++c) {
            putTape(audio[t][c], rate, rt.ch[t][c], rt.frames);
        }
    }
    for (int c = 0; c < 2; ++c) {
        putTape(clip[c], rate, rt.clip[c], rt.frames);
    }
    rt.clipFrames = juce::jmin(rt.frames, static_cast<int>(clipN * toTape));
    // positions in the state are 48 kHz tape frames already (the panel's clock)
    s.loopIn = juce::jlimit(0, juce::jmax(0, rt.frames - 1), s.loopIn);
    s.loopOut = juce::jlimit(0, rt.frames, s.loopOut);
    for (auto& mk : s.marks) {
        mk = juce::jlimit(0, juce::jmax(0, rt.frames - 1), mk);
    }
    transport_stop(rt);
    rt.engine.ramp = 0.f;
    rt.pos = juce::jlimit(0.0, static_cast<double>(juce::jmax(0, rt.frames - 1)), pos * toTape);
    rt.loopStart = s.loop ? s.loopIn : 0;
    rt.loopEnd = s.loop && s.loopOut > s.loopIn ? s.loopOut : 0;
    for (int t = 0; t < kTrackCount; ++t) {
        rt.mute[t] = s.mutes[static_cast<size_t>(t)];
    }
    rt.arm = s.arm;
    transport_bind_character(rt, s.character);
    tape_engine_set_character(monitor_, character_row(rt.character));
    lastBias_ = -1.f;
    appliedArm_ = -1;
    recSource_ = s.recSource;
    undoStart_ = -1;
    clearTrack_ = -1;
    voice_.allOff();
    fifo_.reset();  // drop panel commands that predate the load
    uiPos.store(rt.pos);
    uiPlaying.store(false);
    uiRecording.store(false);
    uiLoopStart.store(rt.loopStart);
    uiLoopEnd.store(rt.loopEnd);
    uiClipFrames.store(rt.clipFrames);
    suspendProcessing(false);

    ui_ = s;
    setAllKnobs(s.enc);
    scrubPrev_ = -1.f;
    seqStepDiv.store(s.seqDiv);
    drumStepDiv.store(s.drumDiv);
    drumLength.store(s.drumLen);
    drumSwing.store(s.swing);
    drumKit.store(s.sel[1]);
    synthEngine.store(s.sel[0]);
    patch.publish(s.cords);
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && !mm->isThisTheMessageThread()) {
        pendingLoaded_.store(slot + 1);  // a host restored state off the message thread: tell the panel from the timer
    } else if (listener_ != nullptr) {
        listener_->stateLoaded(slot);
    }
    return LoadResult::Ok;
}

bool LiftProcessor::saveSlot(int slot) {
    if (!SlotStore::valid(slot)) {
        return false;
    }
    const auto blob = saveState();
    if (!slots_.write(slot, blob)) {
        return false;
    }
    slots_.setLastSlot(slot);
    currentSlot_ = slot;
    return true;
}

void LiftProcessor::saveSlotAsync(int slot, std::function<void(bool)> done) {
    if (!SlotStore::valid(slot)) {
        if (done) {
            done(false);
        }
        return;
    }
    // the snapshot is taken here (the tape must not move under it); only the
    // disk write goes to the background
    auto blob = std::make_shared<juce::MemoryBlock>(saveState());
    currentSlot_ = slot;
    const SlotStore* store = &slots_;
    io_.addJob([store, slot, blob, done = std::move(done)] {
        const bool ok = store->write(slot, *blob) && store->setLastSlot(slot);
        juce::MessageManager::callAsync([done, ok] {
            if (done) {
                done(ok);
            }
        });
    });
}

LiftProcessor::LoadResult LiftProcessor::loadSlot(int slot) {
    if (!SlotStore::valid(slot)) {
        return LoadResult::BadSlot;
    }
    juce::MemoryBlock blob;
    if (!slots_.read(slot, blob)) {
        return LoadResult::Empty;
    }
    const auto r = loadState(blob.getData(), blob.getSize(), slot);
    if (r == LoadResult::Ok) {
        currentSlot_ = slot;
        slots_.setLastSlot(slot);
    }
    return r;
}

bool LiftProcessor::reopenLast() {
    const int s = slots_.lastSlot();
    return s != 0 && loadSlot(s) == LoadResult::Ok;
}

void LiftProcessor::getStateInformation(juce::MemoryBlock& dest) {
    dest.reset();
    if (wrapperType == wrapperType_Standalone) {
        return;  // no autosave in the standalone; it reopens the last saved slot
    }
    dest = saveState();
}

void LiftProcessor::setStateInformation(const void* data, int size) {
    if (data != nullptr && size > 0) {
        loadState(data, static_cast<size_t>(size), 0);
    }
}

juce::AudioProcessorEditor* LiftProcessor::createEditor() {
    return new LiftEditor(*this);
}

}  // namespace lift

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new lift::LiftProcessor();
}
