// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
// COMPUTER AUDIO backends for Linux (PulseAudio / PipeWire monitor) and
// Windows (WASAPI loopback). macOS is SystemCaptureMac.mm.
#include "SystemAudioSource.h"

#if JUCE_LINUX || JUCE_BSD
#include <dlfcn.h>
#endif

#include <algorithm>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace lift {

#if JUCE_LINUX || JUCE_BSD
namespace {
// The few libpulse-simple symbols we need, loaded at run time (LGPL library,
// dynamically loaded; nothing is linked at build time). PipeWire provides
// the same API through pipewire-pulse.
struct PaSampleSpec { int format; uint32_t rate; uint8_t channels; };
using PaNew = void* (*)(const char*, const char*, int, const char*, const char*, const PaSampleSpec*, const void*,
                        const void*, int*);
using PaRead = int (*)(void*, void*, size_t, int*);
using PaFree = void (*)(void*);
constexpr int kPaStreamRecord = 2, kPaFloat32LE = 5;

class PulseMonitorBackend final : public SystemCaptureBackend {
public:
    ~PulseMonitorBackend() override {
        close();
        if (lib_ != nullptr) dlclose(lib_);
    }
    static bool available() {
        void* h = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        if (h != nullptr) dlclose(h);
        return h != nullptr;
    }
    bool open(juce::String& why) override {
        if (lib_ == nullptr) lib_ = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        if (lib_ == nullptr) { why = "no PulseAudio/PipeWire"; return false; }
        pnew_ = reinterpret_cast<PaNew>(dlsym(lib_, "pa_simple_new"));
        pread_ = reinterpret_cast<PaRead>(dlsym(lib_, "pa_simple_read"));
        pfree_ = reinterpret_cast<PaFree>(dlsym(lib_, "pa_simple_free"));
        if (pnew_ == nullptr || pread_ == nullptr || pfree_ == nullptr) { why = "libpulse-simple is incomplete"; return false; }
        const PaSampleSpec ss{kPaFloat32LE, 48000u, 2};
        // 20 ms fragments: reads return about every 20 ms
        struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } attr{~0u, ~0u, ~0u, ~0u, 48000u * 8u / 50u};
        int err = 0;
        s_ = pnew_(nullptr, "LIFT", kPaStreamRecord, "@DEFAULT_MONITOR@", "Computer audio", &ss, nullptr, &attr, &err);
        if (s_ == nullptr) { why = "no monitor source (PulseAudio/PipeWire not running?)"; return false; }
        return true;
    }
    double sampleRate() const override { return 48000.0; }
    int read(float* l, float* r, int maxFrames) override {
        const int n = std::min(maxFrames, 960);  // 20 ms
        buf_.resize(size_t(n) * 2);
        int err = 0;
        if (pread_(s_, buf_.data(), buf_.size() * sizeof(float), &err) < 0) return -1;
        for (int i = 0; i < n; ++i) { l[i] = buf_[size_t(2 * i)]; r[i] = buf_[size_t(2 * i + 1)]; }
        return n;
    }
    void close() override {
        if (s_ != nullptr) pfree_(s_);
        s_ = nullptr;
    }
    bool includesOwnOutput() const override { return true; }  // the monitor hears every stream, LIFT's too
    juce::String name() const override { return "PulseAudio/PipeWire monitor"; }

private:
    void* lib_ = nullptr;
    void* s_ = nullptr;
    PaNew pnew_ = nullptr;
    PaRead pread_ = nullptr;
    PaFree pfree_ = nullptr;
    std::vector<float> buf_;
};
}  // namespace

std::unique_ptr<SystemCaptureBackend> makePlatformCaptureBackend() {
    if (!PulseMonitorBackend::available()) return nullptr;  // unsupported: no PulseAudio/PipeWire
    return std::make_unique<PulseMonitorBackend>();
}
#endif

#if JUCE_WINDOWS
namespace {
template <class T> void releaseCom(T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

class WasapiLoopbackBackend final : public SystemCaptureBackend {
public:
    ~WasapiLoopbackBackend() override { close(); }
    bool open(juce::String& why) override {
        if (!comInit_) comInit_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        IMMDeviceEnumerator* en = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&en));
        if (FAILED(hr)) { why = "no audio devices"; return false; }
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev_);
        releaseCom(en);
        if (FAILED(hr)) { why = "no default output device"; return false; }
        hr = dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
        if (FAILED(hr)) { why = "cannot open the output device"; close(); return false; }
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(client_->GetMixFormat(&wf))) { why = "no mix format"; close(); return false; }
        rate_ = wf->nSamplesPerSec;
        ch_ = wf->nChannels;
        isFloat_ = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        bits_ = wf->wBitsPerSample;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000 /* 200 ms */, 0, wf, nullptr);
        CoTaskMemFree(wf);
        if (FAILED(hr)) { why = "loopback refused"; close(); return false; }
        hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap_));
        if (FAILED(hr) || FAILED(client_->Start())) { why = "loopback did not start"; close(); return false; }
        if ((!isFloat_ && bits_ != 16 && bits_ != 32) || ch_ < 1) { why = "unsupported mix format"; close(); return false; }
        return true;
    }
    double sampleRate() const override { return double(rate_); }
    int read(float* l, float* r, int maxFrames) override {
        int got = 0;
        for (int spins = 0; spins < 5 && got == 0; ++spins) {
            UINT32 packet = 0;
            if (FAILED(cap_->GetNextPacketSize(&packet))) return -1;  // device invalidated
            if (packet == 0) { Sleep(10); continue; }
            while (packet != 0 && got < maxFrames) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return -1;
                const UINT32 take = std::min<UINT32>(frames, UINT32(maxFrames - got));
                for (UINT32 i = 0; i < take; ++i) {
                    float a = 0.f, b = 0.f;
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                        if (isFloat_) {
                            const float* f = reinterpret_cast<const float*>(data) + size_t(i) * ch_;
                            a = f[0]; b = ch_ > 1 ? f[1] : f[0];
                        } else if (bits_ == 16) {
                            const int16_t* s = reinterpret_cast<const int16_t*>(data) + size_t(i) * ch_;
                            a = s[0] / 32768.f; b = (ch_ > 1 ? s[1] : s[0]) / 32768.f;
                        } else {
                            const int32_t* s = reinterpret_cast<const int32_t*>(data) + size_t(i) * ch_;
                            a = float(s[0] / 2147483648.0); b = float((ch_ > 1 ? s[1] : s[0]) / 2147483648.0);
                        }
                    }
                    l[got + int(i)] = a;
                    r[got + int(i)] = b;
                }
                cap_->ReleaseBuffer(frames);
                got += int(take);
                if (FAILED(cap_->GetNextPacketSize(&packet))) return -1;
            }
        }
        return got;
    }
    void close() override {
        if (client_ != nullptr) client_->Stop();
        releaseCom(cap_);
        releaseCom(client_);
        releaseCom(dev_);
    }
    bool includesOwnOutput() const override { return true; }  // default-device loopback hears LIFT too
    juce::String name() const override { return "WASAPI loopback"; }

private:
    bool comInit_ = false;
    IMMDevice* dev_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* cap_ = nullptr;
    DWORD rate_ = 48000;
    int ch_ = 2, bits_ = 32;
    bool isFloat_ = true;
};
}  // namespace

std::unique_ptr<SystemCaptureBackend> makePlatformCaptureBackend() { return std::make_unique<WasapiLoopbackBackend>(); }
#endif

}  // namespace lift
