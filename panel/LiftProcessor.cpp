// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "LiftProcessor.h"

#include "LiftEditor.h"

#include "audio/prepare.h"
#include "audio/process_block.h"
#include "tape/edit.h"
#include "tape/transport.h"

#include <cmath>

namespace lift {

namespace {
constexpr float kStopRampSeconds = 0.35f;
constexpr int kChunk = 512;
}  // namespace

void PlaceholderVoice::render(float* out, int n, double sampleRate) noexcept {
    const float att = 1.f - std::exp(-1.f / (0.004f * static_cast<float>(sampleRate)));
    const float rel = 1.f - std::exp(-1.f / (0.18f * static_cast<float>(sampleRate)));
    const float lpA = 1.f - std::exp(-2.f * 3.14159265f * 2400.f / static_cast<float>(sampleRate));
    for (int i = 0; i < n; ++i) {
        env += (gate ? att : rel) * ((gate ? 1.f : 0.f) - env);
        if (env < 1e-5f && !gate) {
            env = 0.f;
            out[i] = 0.f;
            continue;
        }
        phase += inc;
        if (phase >= 1.0) {
            phase -= 1.0;
        }
        phase2 += inc * 1.004;
        if (phase2 >= 1.0) {
            phase2 -= 1.0;
        }
        const float saw = static_cast<float>(2.0 * phase - 1.0);
        const float sq = phase2 < 0.5 ? 0.6f : -0.6f;
        lp += lpA * ((saw + sq) * 0.5f - lp);
        out[i] = lp * env * 0.5f;
    }
}

LiftProcessor::LiftProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      rt_(std::make_unique<TapeRuntime>()) {
    transport_init(*rt_);
    rt_->mute[2] = true;  // the panel opens with M3 lit
    rt_->loopStart = 0;
    rt_->loopEnd = kLoopSeconds * kSampleRate;
}

LiftProcessor::~LiftProcessor() {
    release_tracks(*rt_);
}

bool LiftProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LiftProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // The tape is clocked at kSampleRate (48 kHz); other device rates play
    // the tape off-pitch until the engine resamples.
    sampleRate_ = sampleRate;
    if (!prepared_) {
        prepare_tracks(*rt_, kTapeSeconds * kSampleRate);
        if (rt_->loopEnd > rt_->frames) {
            rt_->loopEnd = rt_->frames;
        }
        prepared_ = true;
    }
    const int need = juce::jmax(samplesPerBlock, kChunk);
    if (need > synthSize_) {
        synth_.allocate(static_cast<size_t>(need), true);
        synthSize_ = need;
    }
    uiFrames.store(rt_->frames);
}

void LiftProcessor::send(Cmd c, int a, int b, double v) noexcept {
    const auto scope = fifo_.write(1);
    if (scope.blockSize1 > 0) {
        cmds_[scope.startIndex1] = Command{c, a, b, v};
    } else if (scope.blockSize2 > 0) {
        cmds_[scope.startIndex2] = Command{c, a, b, v};
    }
}

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
    case Cmd::Drop:
        tape_drop(rt, true);
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
    case Cmd::NoteOn:
        voice_.note = c.a;
        voice_.inc = 440.0 * std::pow(2.0, (c.a - 69) / 12.0) / sampleRate_;
        voice_.gate = true;
        break;
    case Cmd::NoteOff:
        if (c.a < 0 || c.a == voice_.note) {
            voice_.gate = false;
        }
        break;
    }
}

void LiftProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    TapeRuntime& rt = *rt_;
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
        const auto m = meta.getMessage();
        if (m.isNoteOn()) {
            apply(Command{Cmd::NoteOn, m.getNoteNumber(), 0, 0.0});
        } else if (m.isNoteOff()) {
            apply(Command{Cmd::NoteOff, m.getNoteNumber(), 0, 0.0});
        }
    }
    transport_set_varispeed(rt, speed.load(std::memory_order_relaxed));
    const float b = bias.load(std::memory_order_relaxed);
    const float d = drive.load(std::memory_order_relaxed);
    if (b != lastBias_ || d != lastDrive_) {
        TapeParams p = rt.engine.p;
        p.bias = b;
        p.drive = d;
        tape_engine_set_params(rt.engine, p);
        lastBias_ = b;
        lastDrive_ = d;
    }

    const int n = buffer.getNumSamples();
    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : outL;
    for (int start = 0; start < n; start += kChunk) {
        const int m = juce::jmin(kChunk, n - start, synthSize_);
        if (m <= 0 || !prepared_) {
            break;
        }
        float* s = synth_.get();
        voice_.render(s, m, sampleRate_);
        process_block(rt, s, s, outL + start, outR + start, m);
        for (int i = 0; i < m; ++i) {
            outL[start + i] += s[i];  // input monitor
            if (outR != outL) {
                outR[start + i] += s[i];
            }
        }
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch) {
        buffer.clear(ch, 0, n);
    }
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

juce::AudioProcessorEditor* LiftProcessor::createEditor() {
    return new LiftEditor(*this);
}

}  // namespace lift

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new lift::LiftProcessor();
}
