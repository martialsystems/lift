// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
// COMPUTER AUDIO on macOS: a Core Audio process tap (macOS 14.2+) of every
// process except LIFT, read through a private aggregate device. The IO block
// runs on Core Audio's real-time thread and only copies into a lock-free
// staging ring; the capture thread (SystemAudioSource) drains it.
#include "SystemAudioSource.h"

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#if __has_include(<CoreAudio/CATapDescription.h>)
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#define LIFT_HAS_TAPS 1
#else
#define LIFT_HAS_TAPS 0
#endif

namespace lift {

#if LIFT_HAS_TAPS
namespace {

class API_AVAILABLE(macos(14.2)) MacTapBackend final : public SystemCaptureBackend {
public:
    ~MacTapBackend() override { close(); }

    bool open(juce::String& why) override {
        // LIFT's own process object, so the tap leaves it out (no feedback)
        AudioObjectID self = kAudioObjectUnknown;
        pid_t pid = getpid();
        AudioObjectPropertyAddress a{kAudioHardwarePropertyTranslatePIDToProcessObject, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
        UInt32 sz = sizeof(self);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(pid), &pid, &sz, &self);
        NSArray<NSNumber*>* exclude = self != kAudioObjectUnknown ? @[ @(self) ] : @[];
        CATapDescription* desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:exclude];
        desc.name = @"LIFT computer audio";
        desc.privateTap = YES;
        desc.muteBehavior = CATapUnmuted;
        OSStatus st = AudioHardwareCreateProcessTap(desc, &tap_);
        if (st != noErr || tap_ == kAudioObjectUnknown) {
            why = st == kAudioDevicePermissionsError || st == kAudioHardwareIllegalOperationError ? "permission denied (System Settings > Privacy > Audio Recording)"
                                                                            : "could not create the tap (" + juce::String(int(st)) + ")";
            tap_ = kAudioObjectUnknown;
            return false;
        }
        // the tap's format (sample rate)
        AudioStreamBasicDescription fmt{};
        a = {kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        sz = sizeof(fmt);
        if (AudioObjectGetPropertyData(tap_, &a, 0, nullptr, &sz, &fmt) == noErr && fmt.mSampleRate > 0) rate_ = fmt.mSampleRate;
        // private aggregate device holding just the tap
        NSString* tapUID = desc.UUID.UUIDString;
        NSString* aggUID = [NSString stringWithFormat:@"com.martialsystems.lift.tap.%d", int(pid)];
        NSDictionary* agg = @{
            @kAudioAggregateDeviceNameKey : @"LIFT computer audio",
            @kAudioAggregateDeviceUIDKey : aggUID,
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceTapAutoStartKey : @YES,
            @kAudioAggregateDeviceTapListKey : @[ @{@kAudioSubTapUIDKey : tapUID, @kAudioSubTapDriftCompensationKey : @YES} ],
        };
        st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef) agg, &agg_);
        if (st != noErr) {
            why = "could not create the capture device (" + juce::String(int(st)) + ")";
            close();
            return false;
        }
        fifo_.reset();
        auto* fifo = &fifo_;
        auto* L = ringL_.data();
        auto* R = ringR_.data();
        st = AudioDeviceCreateIOProcIDWithBlock(&proc_, agg_, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*) {
                // real-time thread: copy only (interleaved or two planar buffers)
                if (in == nullptr || in->mNumberBuffers == 0) return;
                const AudioBuffer& b0 = in->mBuffers[0];
                const bool planar = in->mNumberBuffers > 1;
                const UInt32 ch = planar ? 1 : (b0.mNumberChannels > 0 ? b0.mNumberChannels : 1);
                const int n = int(b0.mDataByteSize / (sizeof(float) * ch));
                const float* p0 = static_cast<const float*>(b0.mData);
                const float* p1 = planar ? static_cast<const float*>(in->mBuffers[1].mData) : nullptr;
                const int k = std::min(n, fifo->getFreeSpace());
                const auto sc = fifo->write(k);
                auto put = [&](int dst, int i) {
                    L[dst] = planar ? p0[i] : p0[size_t(i) * ch];
                    R[dst] = planar ? p1[i] : p0[size_t(i) * ch + (ch > 1 ? 1 : 0)];
                };
                for (int i = 0; i < sc.blockSize1; ++i) put(sc.startIndex1 + i, i);
                for (int i = 0; i < sc.blockSize2; ++i) put(sc.startIndex2 + i, sc.blockSize1 + i);
            });
        if (st != noErr || AudioDeviceStart(agg_, proc_) != noErr) {
            why = "could not start the capture device";
            close();
            return false;
        }
        return true;
    }

    double sampleRate() const override { return rate_; }

    int read(float* l, float* r, int maxFrames) override {
        for (int spins = 0; spins < 5; ++spins) {
            const int k = std::min(maxFrames, fifo_.getNumReady());
            if (k > 0) {
                const auto sc = fifo_.read(k);
                std::copy_n(ringL_.data() + sc.startIndex1, sc.blockSize1, l);
                std::copy_n(ringR_.data() + sc.startIndex1, sc.blockSize1, r);
                std::copy_n(ringL_.data() + sc.startIndex2, sc.blockSize2, l + sc.blockSize1);
                std::copy_n(ringR_.data() + sc.startIndex2, sc.blockSize2, r + sc.blockSize1);
                return k;
            }
            juce::Thread::sleep(10);
        }
        return 0;
    }

    void close() override {
        if (agg_ != kAudioObjectUnknown && proc_ != nullptr) {
            AudioDeviceStop(agg_, proc_);
            AudioDeviceDestroyIOProcID(agg_, proc_);
        }
        proc_ = nullptr;
        if (agg_ != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(agg_);
        agg_ = kAudioObjectUnknown;
        if (tap_ != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(tap_);
        tap_ = kAudioObjectUnknown;
    }

    bool includesOwnOutput() const override { return false; }  // LIFT is excluded from the tap
    juce::String name() const override { return "Core Audio tap"; }

private:
    AudioObjectID tap_ = kAudioObjectUnknown, agg_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_ = nullptr;
    double rate_ = 48000.0;
    juce::AbstractFifo fifo_{48000};
    std::vector<float> ringL_ = std::vector<float>(48000), ringR_ = std::vector<float>(48000);
};

// macOS older than 14.2: say so instead of failing quietly
class MacTooOldBackend final : public SystemCaptureBackend {
public:
    bool open(juce::String& why) override { why = "needs macOS 14.2 or later"; return false; }
    double sampleRate() const override { return 48000.0; }
    int read(float*, float*, int) override { return 0; }
    void close() override {}
    bool includesOwnOutput() const override { return false; }
    juce::String name() const override { return "Core Audio tap"; }
};
}  // namespace

std::unique_ptr<SystemCaptureBackend> makePlatformCaptureBackend() {
    if (@available(macOS 14.2, *)) return std::make_unique<MacTapBackend>();
    return std::make_unique<MacTooOldBackend>();
}
#else
namespace {
class MacNoSdkBackend final : public SystemCaptureBackend {
public:
    bool open(juce::String& why) override { why = "this build has no Core Audio taps (SDK older than 14.2)"; return false; }
    double sampleRate() const override { return 48000.0; }
    int read(float*, float*, int) override { return 0; }
    void close() override {}
    bool includesOwnOutput() const override { return false; }
    juce::String name() const override { return "Core Audio tap"; }
};
}  // namespace
std::unique_ptr<SystemCaptureBackend> makePlatformCaptureBackend() { return std::make_unique<MacNoSdkBackend>(); }
#endif

}  // namespace lift
