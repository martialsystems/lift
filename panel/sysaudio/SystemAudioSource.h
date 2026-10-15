// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// COMPUTER AUDIO: what the computer is playing (system loopback), as a
// source you can LIFT. Same shape as RadioSource: a capture thread pulls from
// a platform backend, resamples to 48 kHz, levels it, and hands it to the
// audio thread through a lock-free ring. Standalone only.
//
// Backends (SystemCaptureBackend):
//   macOS 14.2+  Core Audio process tap of everything except LIFT itself
//   Windows      WASAPI loopback of the default render device
//   Linux / Pi   PulseAudio / PipeWire "@DEFAULT_MONITOR@" (libpulse-simple,
//                loaded at run time; no build dependency)
// Feedback: the source is never sent to LIFT's output (you already hear the
// computer). It goes to the listen buffer (and the IN path when REC source is
// IN). Backends that hear LIFT itself (includesOwnOutput) replace LIFT's own
// output in the listen buffer instead of adding to it, so nothing is doubled.
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

namespace lift {

class SystemCaptureBackend {
public:
    virtual ~SystemCaptureBackend() = default;
    // Capture thread. open(): false + a short reason ("needs macOS 14.2",
    // "permission denied", "no PulseAudio/PipeWire") if it cannot run.
    virtual bool open(juce::String& why) = 0;
    virtual double sampleRate() const = 0;
    // Blocks up to ~50 ms. Returns frames written (stereo, deinterleaved),
    // 0 = nothing yet, < 0 = the device went away (reopen).
    virtual int read(float* l, float* r, int maxFrames) = 0;
    virtual void close() = 0;
    virtual bool includesOwnOutput() const = 0;  // the loopback also hears LIFT
    virtual juce::String name() const = 0;
};

std::unique_ptr<SystemCaptureBackend> makePlatformCaptureBackend();  // nullptr: unsupported here

class SystemAudioSource final : private juce::Thread {
public:
    // backend: nullptr = the platform's (tests pass a fake)
    explicit SystemAudioSource(std::unique_ptr<SystemCaptureBackend> backend = nullptr);
    ~SystemAudioSource() override;
    // message thread
    void setOn(bool on);
    bool isOn() const noexcept { return on_.load(); }
    bool supported() const noexcept { return backend_ != nullptr; }
    juce::String status() const;
    bool includesOwnOutput() const noexcept { return ownOut_.load(); }
    std::atomic<float> level{1.f};
    // audio thread: lock-free, never blocks. Returns frames of real audio.
    int read(float* l, float* r, int n) noexcept;
    double bufferedSeconds() const noexcept { return fifo_.getNumReady() / 48000.0; }

private:
    void run() override;
    void setStatus(const juce::String& s);
    void push(const float* l, const float* r, int n);

    static constexpr int kRing = 48000 * 2;      // 2 s
    static constexpr int kPreroll = 48000 / 20;  // 50 ms: loopback is local, keep it close
    std::unique_ptr<SystemCaptureBackend> backend_;
    std::atomic<bool> on_{false}, ownOut_{false}, flush_{false};
    juce::AbstractFifo fifo_{kRing};
    std::vector<float> ringL_ = std::vector<float>(kRing), ringR_ = std::vector<float>(kRing);
    bool playing_ = false;
    float fade_ = 0.f;
    mutable juce::CriticalSection statusLock_;
    juce::String status_ = "OFF";
    double ms_ = 0.0;
    float gain_ = 1.f;
};

}  // namespace lift
