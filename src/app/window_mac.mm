// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/prepare.h"
#include "ui/panel.h"

#include <AudioToolbox/AudioToolbox.h>
#include <Cocoa/Cocoa.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kSlice = 4096;

struct Args {
    const char* project;
    const char* shot;
    int play;
    int bad;
};

struct HalPack {
    UInt32 count;
    AudioBuffer bufs[2];
};

struct Engine {
    AudioUnit unit;
    bool deviceOn;
    bool inputOn;
    bool inputInterleaved;
    std::atomic<int> run;
    std::thread worker;
    std::vector<float> inL;
    std::vector<float> inR;
    std::vector<float> outL;
    std::vector<float> outR;
    std::vector<float> scratch;
};

struct App {
    TapeRuntime rt;
    PanelUi ui;
    Engine engine;
    std::vector<uint8_t> pixels;
    double t0;
    bool tracks;
};

App g_storage;
App* g_live = nullptr;
NSTimer* g_timer = nil;

Args parse_args(int argc, char** argv) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i];
        if (std::strcmp(s, "--project") == 0) {
            if (i + 1 >= argc) {
                args.bad = 1;
                return args;
            }
            args.project = argv[++i];
        } else if (std::strcmp(s, "--shot") == 0) {
            if (i + 1 >= argc) {
                args.bad = 1;
                return args;
            }
            args.shot = argv[++i];
        } else if (std::strcmp(s, "--shot-play") == 0) {
            if (i + 1 >= argc) {
                args.bad = 1;
                return args;
            }
            args.shot = argv[++i];
            args.play = 1;
        } else {
            args.bad = 1;
            return args;
        }
    }
    return args;
}

AudioStreamBasicDescription client_format(bool interleaved) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = 48000.0;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket = 1;
    fmt.mBitsPerChannel = 32;
    fmt.mChannelsPerFrame = 2;
    if (interleaved) {
        fmt.mBytesPerFrame = 8;
        fmt.mBytesPerPacket = 8;
    } else {
        fmt.mFormatFlags |= kAudioFormatFlagIsNonInterleaved;
        fmt.mBytesPerFrame = 4;
        fmt.mBytesPerPacket = 4;
    }
    return fmt;
}

void copy_mono(AudioBuffer& buf, const float* src, UInt32 n, UInt32 frames) {
    if (buf.mData == nullptr) {
        return;
    }
    auto* dst = static_cast<float*>(buf.mData);
    const UInt32 have = buf.mDataByteSize / static_cast<UInt32>(sizeof(float));
    const UInt32 copy = n < have ? n : have;
    if (copy > 0) {
        std::memcpy(dst, src, static_cast<size_t>(copy) * sizeof(float));
    }
    const UInt32 tail = frames < have ? frames : have;
    for (UInt32 i = copy; i < tail; ++i) {
        dst[i] = 0.f;
    }
}

void write_output(AudioBufferList* io, const float* left, const float* right, UInt32 n, UInt32 frames) {
    if (io == nullptr || io->mNumberBuffers == 0) {
        return;
    }
    if (io->mNumberBuffers >= 2) {
        copy_mono(io->mBuffers[0], left, n, frames);
        copy_mono(io->mBuffers[1], right, n, frames);
        return;
    }
    auto* dst = static_cast<float*>(io->mBuffers[0].mData);
    if (dst == nullptr) {
        return;
    }
    const UInt32 ch = io->mBuffers[0].mNumberChannels < 1 ? 1 : io->mBuffers[0].mNumberChannels;
    for (UInt32 i = 0; i < frames; ++i) {
        const float lv = i < n ? left[i] : 0.f;
        const float rv = i < n ? right[i] : 0.f;
        if (ch == 1) {
            dst[i] = lv;
            continue;
        }
        dst[i * ch] = lv;
        dst[i * ch + 1] = rv;
        for (UInt32 c = 2; c < ch; ++c) {
            dst[i * ch + c] = lv;
        }
    }
}

void take_input(Engine* engine, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 n) {
    float* left = engine->inL.data();
    float* right = engine->inR.data();
    if (!engine->inputOn || engine->unit == nullptr) {
        std::memset(left, 0, static_cast<size_t>(n) * sizeof(float));
        std::memset(right, 0, static_cast<size_t>(n) * sizeof(float));
        return;
    }
    OSStatus status = noErr;
    if (engine->inputInterleaved) {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = n * 2u * static_cast<UInt32>(sizeof(float));
        list.mBuffers[0].mData = engine->scratch.data();
        status = AudioUnitRender(engine->unit, flags, ts, 1, n, &list);
        if (status == noErr) {
            const float* src = engine->scratch.data();
            for (UInt32 i = 0; i < n; ++i) {
                left[i] = src[i * 2u];
                right[i] = src[i * 2u + 1u];
            }
            return;
        }
    } else {
        HalPack pack{};
        pack.count = 2;
        pack.bufs[0].mNumberChannels = 1;
        pack.bufs[0].mDataByteSize = n * static_cast<UInt32>(sizeof(float));
        pack.bufs[0].mData = left;
        pack.bufs[1].mNumberChannels = 1;
        pack.bufs[1].mDataByteSize = n * static_cast<UInt32>(sizeof(float));
        pack.bufs[1].mData = right;
        auto* list = reinterpret_cast<AudioBufferList*>(&pack);
        status = AudioUnitRender(engine->unit, flags, ts, 1, n, list);
        if (status == noErr) {
            return;
        }
    }
    std::memset(left, 0, static_cast<size_t>(n) * sizeof(float));
    std::memset(right, 0, static_cast<size_t>(n) * sizeof(float));
}

OSStatus on_render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames,
                   AudioBufferList* io) {
    (void)bus;
    auto* engine = static_cast<Engine*>(ref);
    if (engine == nullptr || frames == 0) {
        return noErr;
    }
    const UInt32 n = frames > kSlice ? kSlice : frames;
    take_input(engine, flags, ts, n);
    panel_audio_block(engine->inL.data(), engine->inR.data(), engine->outL.data(), engine->outR.data(),
                       static_cast<int>(n));
    write_output(io, engine->outL.data(), engine->outR.data(), n, frames);
    return noErr;
}

void dispose_unit(Engine& engine) {
    if (engine.unit == nullptr) {
        return;
    }
    AudioComponentInstanceDispose(engine.unit);
    engine.unit = nullptr;
    engine.deviceOn = false;
    engine.inputOn = false;
}

bool set_output_format(AudioUnit unit, bool interleaved) {
    const AudioStreamBasicDescription fmt = client_format(interleaved);
    return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt) ==
           noErr;
}

bool start_device(Engine& engine) {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        return false;
    }
    if (AudioComponentInstanceNew(comp, &engine.unit) != noErr) {
        engine.unit = nullptr;
        return false;
    }
    UInt32 enable = 1;
    const bool inputArmed =
        AudioUnitSetProperty(engine.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enable,
                             sizeof enable) == noErr;
    bool interleaved = false;
    if (!set_output_format(engine.unit, false) && !set_output_format(engine.unit, true)) {
        dispose_unit(engine);
        return false;
    }
    AudioStreamBasicDescription got{};
    UInt32 size = sizeof got;
    if (AudioUnitGetProperty(engine.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &got, &size) ==
        noErr) {
        interleaved = (got.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    }
    engine.inputOn = false;
    engine.inputInterleaved = interleaved;
    if (inputArmed) {
        const AudioStreamBasicDescription inFmt = client_format(interleaved);
        if (AudioUnitSetProperty(engine.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inFmt,
                                 sizeof inFmt) == noErr) {
            engine.inputOn = true;
            engine.inputInterleaved = interleaved;
        } else {
            UInt32 disable = 0;
            AudioUnitSetProperty(engine.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable,
                                 sizeof disable);
        }
    }
    UInt32 cap = kSlice;
    AudioUnitSetProperty(engine.unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &cap,
                         sizeof cap);
    AURenderCallbackStruct cb{};
    cb.inputProc = on_render;
    cb.inputProcRefCon = &engine;
    if (AudioUnitSetProperty(engine.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                             sizeof cb) != noErr) {
        dispose_unit(engine);
        return false;
    }
    if (AudioUnitInitialize(engine.unit) != noErr) {
        dispose_unit(engine);
        return false;
    }
    if (AudioOutputUnitStart(engine.unit) != noErr) {
        AudioUnitUninitialize(engine.unit);
        dispose_unit(engine);
        return false;
    }
    engine.deviceOn = true;
    return true;
}

void start_silence(Engine& engine) {
    engine.run.store(1);
    engine.worker = std::thread([&engine] {
        while (engine.run.load() != 0) {
            std::memset(engine.inL.data(), 0, 480u * sizeof(float));
            std::memset(engine.inR.data(), 0, 480u * sizeof(float));
            panel_audio_block(engine.inL.data(), engine.inR.data(), engine.outL.data(), engine.outR.data(), 480);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void stop_engine(Engine& engine) {
    if (engine.deviceOn && engine.unit != nullptr) {
        AudioOutputUnitStop(engine.unit);
        engine.deviceOn = false;
        AudioUnitUninitialize(engine.unit);
    }
    engine.run.store(0);
    if (engine.worker.joinable()) {
        engine.worker.join();
    }
    dispose_unit(engine);
}

void fault_tracks(const TapeRuntime& rt) {
    if (rt.frames <= 0) {
        return;
    }
    const size_t bytes = static_cast<size_t>(rt.frames) * sizeof(float);
    for (int t = 0; t < kTrackCount; ++t) {
        std::memset(rt.ch[t][0], 0, bytes);
        std::memset(rt.ch[t][1], 0, bytes);
    }
    std::memset(rt.clip[0], 0, bytes);
    std::memset(rt.clip[1], 0, bytes);
}

void shutdown_live() {
    if (g_live == nullptr) {
        return;
    }
    if (g_timer != nil) {
        [g_timer invalidate];
        g_timer = nil;
    }
    stop_engine(g_live->engine);
    panel_audio_bind(nullptr);
    if (g_live->tracks) {
        release_tracks(g_live->rt);
        g_live->tracks = false;
    }
}

void refresh_view(NSView* view) {
    if (g_live == nullptr || view == nil) {
        return;
    }
    PanelMeters meters{};
    panel_audio_meters(meters);
    const double seconds = CFAbsoluteTimeGetCurrent() - g_live->t0;
    const PanelDrawIn in = panel_compose(g_live->ui, meters, seconds);
    panel_draw(in, g_live->pixels.data(), kPanelW, kPanelH);
    [view setNeedsDisplay:YES];
}

}  // namespace

@interface LiftView : NSView
@end

@implementation LiftView

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

- (void)mouseDown:(NSEvent*)event {
    if (g_live == nullptr) {
        return;
    }
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const int x = static_cast<int>(point.x);
    const int y = static_cast<int>(point.y);
    panel_ui_click(g_live->ui, panel_hit(x, y));
    refresh_view(self);
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (g_live == nullptr || g_live->pixels.size() < static_cast<size_t>(kPanelW * kPanelH * 4)) {
        return;
    }
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (ctx == nullptr) {
        return;
    }
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, g_live->pixels.data(), g_live->pixels.size(), nullptr);
    const CGBitmapInfo info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big) |
                              static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast);
    CGImageRef image = CGImageCreate(static_cast<size_t>(kPanelW), static_cast<size_t>(kPanelH), 8, 32,
                                     static_cast<size_t>(kPanelW * 4), space, info, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, kPanelH);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, kPanelW, kPanelH), image);
    CGContextRestoreGState(ctx);
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
}

@end

@interface LiftDelegate : NSObject <NSApplicationDelegate>
@end

@implementation LiftDelegate

- (void)applicationWillTerminate:(NSNotification*)note {
    (void)note;
    shutdown_live();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

@end

namespace {

int write_shot(const Args& args) {
    PanelUi ui;
    panel_ui_init(ui);
    ui.baseColor = 0;
    if (args.project != nullptr && panel_ui_load_pool(ui, args.project) < 0) {
        std::fprintf(stderr, "pool load failed\n");
        return 1;
    }
    PanelMeters meters{};
    if (args.play) {
        meters.playing = 1;
    }
    const PanelDrawIn in = panel_compose(ui, meters, args.play ? 0.5 : 0.0);
    std::vector<uint8_t> pixels(static_cast<size_t>(kPanelW * kPanelH * 4));
    panel_draw(in, pixels.data(), kPanelW, kPanelH);
    return panel_write_ppm(args.shot, pixels.data(), kPanelW, kPanelH);
}

int run_live(const Args& args) {
    g_live = &g_storage;
    transport_init(g_live->rt);
    try {
        prepare_tracks(g_live->rt, kDefaultFrames);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "tape allocation failed\n");
        g_live = nullptr;
        return 1;
    }
    g_live->tracks = true;
    fault_tracks(g_live->rt);
    panel_audio_bind(&g_live->rt);
    panel_ui_init(g_live->ui);
    g_live->ui.baseColor = 0;
    if (args.project != nullptr && panel_ui_load_pool(g_live->ui, args.project) < 0) {
        std::fprintf(stderr, "pool load failed\n");
    }
    g_live->pixels.assign(static_cast<size_t>(kPanelW * kPanelH * 4), 0);
    g_live->t0 = CFAbsoluteTimeGetCurrent();
    g_live->engine.inL.assign(kSlice, 0.f);
    g_live->engine.inR.assign(kSlice, 0.f);
    g_live->engine.outL.assign(kSlice, 0.f);
    g_live->engine.outR.assign(kSlice, 0.f);
    g_live->engine.scratch.assign(kSlice * 2u, 0.f);
    if (start_device(g_live->engine)) {
        std::fprintf(stderr, "audio: device\n");
    } else {
        start_silence(g_live->engine);
        std::fprintf(stderr, "audio: silence\n");
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    LiftDelegate* delegate = [[LiftDelegate alloc] init];
    [NSApp setDelegate:delegate];
    const NSRect rect = NSMakeRect(0, 0, kPanelW, kPanelH);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:rect
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    window.title = @"LIFT";
    LiftView* view = [[LiftView alloc] initWithFrame:rect];
    window.contentView = view;
    refresh_view(view);
    g_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                              repeats:YES
                                                block:^(NSTimer* timer) {
                                                  (void)timer;
                                                  refresh_view(view);
                                                }];
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    shutdown_live();
    g_live = nullptr;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (args.bad) {
        std::fprintf(stderr, "lift_window [--project folder] [--shot path | --shot-play path]\n");
        return 2;
    }
    if (args.shot != nullptr) {
        return write_shot(args);
    }
    @autoreleasepool {
        return run_live(args);
    }
}
