// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#include "SystemAudioSource.h"

#include "engine/resample.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace lift {
using namespace juce;

SystemAudioSource::SystemAudioSource(std::unique_ptr<SystemCaptureBackend> b)
    : Thread("LIFT computer audio"), backend_(b != nullptr ? std::move(b) : makePlatformCaptureBackend()) {
    if (backend_ == nullptr) status_ = "COMPUTER AUDIO: NOT SUPPORTED HERE";
}

SystemAudioSource::~SystemAudioSource() {
    on_.store(false);
    signalThreadShouldExit();
    notify();
    stopThread(4000);
}

void SystemAudioSource::setOn(bool on) {
    if (backend_ == nullptr) return;
    on_.store(on);
    flush_.store(true);
    if (on && !isThreadRunning()) startThread(Priority::high);
    notify();
}

String SystemAudioSource::status() const {
    const ScopedLock l(statusLock_);
    return status_;
}

void SystemAudioSource::setStatus(const String& s) {
    const ScopedLock l(statusLock_);
    status_ = s;
}

int SystemAudioSource::read(float* l, float* r, int n) noexcept {
    if (flush_.exchange(false)) {
        fifo_.read(fifo_.getNumReady());
        playing_ = false;
    }
    const int ready = fifo_.getNumReady();
    if (!playing_ && ready >= kPreroll) playing_ = true;
    int got = 0;
    if (playing_) {
        got = std::min(n, ready);
        const auto sc = fifo_.read(got);
        std::copy_n(ringL_.data() + sc.startIndex1, sc.blockSize1, l);
        std::copy_n(ringR_.data() + sc.startIndex1, sc.blockSize1, r);
        std::copy_n(ringL_.data() + sc.startIndex2, sc.blockSize2, l + sc.blockSize1);
        std::copy_n(ringR_.data() + sc.startIndex2, sc.blockSize2, r + sc.blockSize1);
        if (got < n) playing_ = false;
    }
    for (int i = got; i < n; ++i) l[i] = r[i] = 0.f;
    const float lv = level.load(std::memory_order_relaxed), target = got > 0 ? 1.f : 0.f;
    for (int i = 0; i < n; ++i) {
        fade_ += (target - fade_) * 0.002f;
        l[i] *= lv * fade_;
        r[i] *= lv * fade_;
    }
    return got;
}

void SystemAudioSource::push(const float* l, const float* r, int n) {
    // level: slow RMS toward the keep target, but never boost silence-ish
    // material more than +12 dB, never cut more than -12 dB
    const double a = 1.0 - std::exp(-1.0 / (3.0 * 48000.0));
    std::vector<float> gl(static_cast<size_t>(n)), gr(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        ms_ += a * (0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]) - ms_);
        if ((i & 255) == 0 && ms_ > 1e-8) {
            const double db = 10.0 * std::log10(ms_);
            const float want = static_cast<float>(std::pow(10.0, jlimit(-12.0, 12.0, eng::kTargetLufs - db) / 20.0));
            gain_ += 0.05f * (want - gain_);
        }
        gl[size_t(i)] = jlimit(-1.f, 1.f, l[i] * gain_);
        gr[size_t(i)] = jlimit(-1.f, 1.f, r[i] * gain_);
    }
    // never wait here: a full ring (nobody reading) drops the oldest-in-time
    // new material instead of stalling the capture device
    const int k = std::min(n, fifo_.getFreeSpace());
    const auto sc = fifo_.write(k);
    std::copy_n(gl.data(), sc.blockSize1, ringL_.data() + sc.startIndex1);
    std::copy_n(gr.data(), sc.blockSize1, ringR_.data() + sc.startIndex1);
    std::copy_n(gl.data() + sc.blockSize1, sc.blockSize2, ringL_.data() + sc.startIndex2);
    std::copy_n(gr.data() + sc.blockSize1, sc.blockSize2, ringR_.data() + sc.startIndex2);
}

void SystemAudioSource::run() {
    constexpr int kMax = 4096;
    std::vector<float> l(kMax), r(kMax);
    std::vector<float> ol(kMax * 4 + 64), orr(kMax * 4 + 64);
    LagrangeInterpolator interp[2];
    while (!threadShouldExit()) {
        if (!on_.load()) {
            setStatus("OFF");
            wait(-1);
            continue;
        }
        String why;
        if (!backend_->open(why)) {
            setStatus("COMPUTER AUDIO: " + why.toUpperCase());
            wait(3000);  // permission may be granted meanwhile: try again
            continue;
        }
        ownOut_.store(backend_->includesOwnOutput());
        setStatus("COMPUTER AUDIO  " + backend_->name().toUpperCase());
        const double ratio = backend_->sampleRate() / 48000.0;
        interp[0].reset();
        interp[1].reset();
        ms_ = 0.0;
        double heard = 0.0, silentSince = Time::getMillisecondCounterHiRes();
        bool saidSilent = false;
        while (!threadShouldExit() && on_.load()) {
            const int n = backend_->read(l.data(), r.data(), kMax);
            for (int i = 0; i < std::max(0, n); ++i) heard = std::max(heard, double(std::abs(l[size_t(i)])));
            if (heard > 1e-6) {
                heard = 0.0;
                silentSince = Time::getMillisecondCounterHiRes();
                if (saidSilent) setStatus("COMPUTER AUDIO  " + backend_->name().toUpperCase());
                saidSilent = false;
            } else if (!saidSilent && Time::getMillisecondCounterHiRes() - silentSince > 8000.0) {
                // nothing playing, or (macOS) the Audio Recording permission was refused: taps then deliver silence
                setStatus("COMPUTER AUDIO: SILENT (NOTHING PLAYING, OR NO AUDIO RECORDING PERMISSION)");
                saidSilent = true;
            }
            if (n < 0) {
                setStatus("COMPUTER AUDIO: DEVICE CHANGED, REOPENING");
                break;
            }
            if (n == 0) continue;
            if (std::abs(ratio - 1.0) < 1e-6) {
                push(l.data(), r.data(), n);
            } else {
                const int made = static_cast<int>(n / ratio);
                interp[0].process(ratio, l.data(), ol.data(), made);
                interp[1].process(ratio, r.data(), orr.data(), made);
                push(ol.data(), orr.data(), made);
            }
        }
        backend_->close();
    }
}

}  // namespace lift
