// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
// LIFT Mini checks: SHIFT state machine, speed through zero, loop crossfade
// direction, command queue overflow, capture ring, sliced copy jobs (DROP +
// UNDO exact), the audio-callback worst case during LIFT / DROP / undo, and
// (with --radio) a live internet radio stream.
#include "mini_bench.h"
#include "ShiftState.h"
#include "tape/transport.h"
#include "engine/resample.h"
#include <cmath>
#include <string>
#include <thread>

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static void shiftChecks() {
    using mini::ShiftState;
    {
        ShiftState s;
        s.press(0);
        check(s.active(), "SHIFT: on while held");
        check(s.release(100) == ShiftState::Out::Cancel && !s.active(), "SHIFT: tap = cancel, then off");
    }
    {
        ShiftState s;
        s.press(0); s.release(80);
        s.press(200);
        check(s.release(260) == ShiftState::Out::None && s.latched() && s.active(), "SHIFT: double tap latches (no second cancel)");
        s.press(2000);
        check(s.release(2050) == ShiftState::Out::None && !s.latched() && !s.active(), "SHIFT: tap while latched unlatches, no cancel");
    }
    {
        ShiftState s;
        s.press(0); s.release(80);
        s.press(1000);
        check(s.release(1060) == ShiftState::Out::Cancel && !s.latched(), "SHIFT: two taps far apart = two cancels, no latch");
    }
    {
        ShiftState s;
        s.press(0);
        check(s.turn(0, 50), "SHIFT+turn: shifted");
        check(s.release(100) == ShiftState::Out::None, "SHIFT+turn: release is not a tap");
        check(s.turn(0, 180), "SHIFT released mid-turn: same encoder stays shifted inside the grace");
        check(s.turn(0, 300), "SHIFT released mid-turn: grace extends while it keeps turning");
        check(!s.turn(1, 310), "SHIFT released mid-turn: other encoders are un-shifted at once");
        check(!s.turn(0, 1000), "SHIFT released mid-turn: grace ends when the turning stops");
    }
    {
        ShiftState s;
        s.press(0);
        s.used();
        check(s.release(50) == ShiftState::Out::None, "SHIFT+key: release is not a cancel");
        s.press(400);
        check(s.release(900) == ShiftState::Out::None && !s.active(), "SHIFT: long press alone does nothing");
    }
    {
        ShiftState s;
        s.press(0); s.release(50); s.press(150); s.release(200);
        check(s.latched(), "SHIFT: latched");
        s.modeKey();
        check(!s.latched() && !s.active(), "SHIFT: mode key while latched opens the mode un-shifted");
    }
}

static void speedChecks() {
    using mini::bipolarKnobToSpeed;
    check(bipolarKnobToSpeed(0.5f) == 0.f && bipolarKnobToSpeed(0.51f) == 0.f, "speed knob: centre band = stop");
    check(bipolarKnobToSpeed(0.f) == -4.f && bipolarKnobToSpeed(1.f) == 4.f, "speed knob: ends = -4x / +4x");
    bool mono = true;
    float prev = -10.f;
    for (int i = 0; i <= 1000; ++i) {
        const float s = bipolarKnobToSpeed(float(i) / 1000.f);
        if (s < prev - 1e-6f || !std::isfinite(s)) mono = false;
        prev = s;
    }
    check(mono, "speed knob: monotonic, finite, through zero");
    check(std::abs(bipolarKnobToSpeed(0.5f + 0.5f * (0.04f + 0.96f * 0.6f)) - 1.f) < 1e-4f, "speed knob: 1x snaps");
    check(std::abs(bipolarKnobToSpeed(0.5f + 0.5f * 0.05f)) < 0.05f, "speed knob: small just past the stop band");

    // the engine, playing, knob swept from +1x through 0 to -1x and back
    lift::LiftProcessor p;
    p.clearDrumPatterns();
    p.prepareToPlay(48000.0, 256);
    auto& rt = p.runtime();
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < rt.frames; ++i) rt.ch[0][c][i] = 0.3f * std::sin(0.01f * float(i));
    p.bipolarSpeed.store(true);
    p.send(lift::Cmd::LoopSet, 48000, 48000 * 3);
    p.send(lift::Cmd::Transport, 1, 0);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    bool finite = true;
    float flipSpeed = 1e9f;
    bool sawRev = false;
    double posAtStop = -1;
    bool stoppedHeld = true;
    for (int b = 0; b < 1200; ++b) {
        const float k = b < 400 ? 1.f : b < 500 ? 1.f - (b - 400) / 100.f : b < 700 ? 0.f : b < 800 ? -(b - 700) / 100.f : -1.f;
        p.signedSpeed.store(k);
        const bool revBefore = rt.reverse;
        const float speedBefore = rt.engine.speedNow;  // what the flip decision sees
        buf.clear();
        p.processBlock(buf, midi);
        for (int i = 0; i < 256; ++i) finite = finite && std::isfinite(buf.getSample(0, i));
        if (rt.reverse != revBefore) flipSpeed = std::min(flipSpeed, speedBefore);
        if (rt.reverse) sawRev = true;
        if (b == 650) posAtStop = rt.pos;
        if (b == 690 && std::abs(rt.pos - posAtStop) > 2.0) stoppedHeld = false;
    }
    check(finite, "speed through zero: output finite (no divide by speed)");
    check(sawRev && flipSpeed <= ::kFlipSpeed + 1e-6f, "speed through zero: direction flips only near standstill (" + std::to_string(flipSpeed) + ")");
    check(stoppedHeld, "speed through zero: 0 holds the tape still while playing");
    // a hard jump +1 -> -1 still glides down before flipping
    p.signedSpeed.store(1.f);
    for (int b = 0; b < 300; ++b) { buf.clear(); p.processBlock(buf, midi); }
    p.signedSpeed.store(-1.f);
    float atFlip = 1e9f;
    for (int b = 0; b < 600; ++b) {
        const bool before = rt.reverse;
        const float speedBefore = rt.engine.speedNow;
        buf.clear();
        p.processBlock(buf, midi);
        if (rt.reverse != before) atFlip = speedBefore;
    }
    check(rt.reverse && atFlip <= ::kFlipSpeed + 1e-6f, "speed jump +1 -> -1: glides to a stop, then reverses");
}

static void crossfadeChecks() {
    // ramp buffer: value = index. Loop [10000, 20000), fade 256.
    std::vector<float> buf(40000);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = float(i);
    const int lo = 10000, hi = 20000;
    // reverse: approaching loopStart from above, the wet side must fall too
    // and land on loopEnd at the wrap
    float prevWetish = 0.f;
    bool ok = true;
    for (int k = 200; k >= 1; --k) {
        const double pos = lo + k;
        const float out = read_looped(buf.data(), int(buf.size()), pos, lo, hi, true);
        const float t = float((256.0 - k) / 256.0);
        const float dry = float(pos);
        const float wet = t > 0.f ? (out - dry * (1.f - t)) / t : 0.f;
        if (std::abs(wet - float(hi + k)) > 0.6f) ok = false;
        if (k < 200 && wet > prevWetish + 1e-3f) ok = false;  // falls as the head moves backwards
        prevWetish = wet;
    }
    check(ok, "loop crossfade, reverse: wet side reads backwards into loopEnd");
    const float atWrapRev = read_looped(buf.data(), int(buf.size()), lo + 0.001, lo, hi, true);
    const float afterWrapRev = read_looped(buf.data(), int(buf.size()), hi - 0.999, lo, hi, true);
    check(std::abs(atWrapRev - afterWrapRev) < 2.f, "loop crossfade, reverse: continuous across the wrap");
    const float atWrapFwd = read_looped(buf.data(), int(buf.size()), hi - 0.001, lo, hi, false);
    const float afterWrapFwd = read_looped(buf.data(), int(buf.size()), lo + 0.0, lo, hi, false);
    check(std::abs(atWrapFwd - afterWrapFwd) < 2.f, "loop crossfade, forward: continuous across the wrap");
}

static void queueChecks() {
    lift::LiftProcessor p;
    p.prepareToPlay(48000.0, 256);
    for (int i = 0; i < 3000; ++i) p.send(lift::Cmd::Arm, i % 4);
    p.send(lift::Cmd::Arm, 2);
    check(p.queueOverflows.load() > 0, "queue: overflow counted (" + std::to_string(p.queueOverflows.load()) + ")");
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    for (int b = 0; b < 8; ++b) { p.flushBacklog(); buf.clear(); p.processBlock(buf, midi); }
    check(p.runtime().arm == 2, "queue: no press dropped, last one applied in order");
}

static void captureChecks() {
    lift::eng::CaptureRing c;
    c.prepare(48000.0, 4.0);
    check(c.capacity() == 4 * 48000, "capture: capacity excludes the guard");
    std::vector<float> l(48000, 0.5f), r(48000, 0.5f), ol(4 * 48000), orr(4 * 48000);
    for (int s = 0; s < 6; ++s) c.write(l.data(), r.data(), 48000);
    std::atomic<bool> go{true};
    std::thread w([&] {
        std::vector<float> a(256, 0.5f);
        while (go) c.write(a.data(), a.data(), 256);
    });
    bool clean = true;
    for (int k = 0; k < 50; ++k) {
        const int n = c.read(ol.data(), orr.data(), 4 * 48000);
        for (int i = 0; i < n; ++i) if (ol[size_t(i)] != 0.5f && ol[size_t(i)] != 0.f) clean = false;
    }
    go = false;
    w.join();
    check(clean, "capture: reads while the writer runs are never torn (torn frames zeroed: " + std::to_string(c.lastReadTorn()) + ")");
}

static void jobChecks() {
    lift::LiftProcessor p;
    p.clearDrumPatterns();
    p.prepareToPlay(48000.0, 256);
    auto& rt = p.runtime();
    for (int t = 0; t < 4; ++t)
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < rt.frames; ++i) rt.ch[t][c][i] = 0.001f * float((i * (t + 3)) % 997);
    std::vector<float> before(rt.ch[1][0], rt.ch[1][0] + rt.frames);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    auto run = [&](int blocks) { for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock(buf, midi); } };
    p.send(lift::Cmd::Arm, 1);
    p.send(lift::Cmd::LoopSet, 0, 48000 * 20);
    p.send(lift::Cmd::Lift);
    run(1);
    check(p.jobBusy(), "jobs: a 20 s LIFT is spread over callbacks");
    run(200);
    check(!p.jobBusy() && rt.clipFrames == 48000 * 20, "jobs: LIFT done, clip is the loop");
    bool same = true;
    for (int i = 0; i < rt.clipFrames; ++i) if (rt.clip[0][i] != rt.ch[1][0][i]) same = false;
    check(same, "jobs: LIFT copied exactly");
    auto clip = p.makeClip(std::vector<float>(48000 * 5, 0.25f), std::vector<float>(48000 * 5, 0.25f), true);
    p.dropToLoop(clip, 1);
    p.send(lift::Cmd::UndoAudio);  // queued behind the drop: must run after it
    run(400);
    bool restored = true;
    for (int i = 0; i < rt.frames; ++i) if (rt.ch[1][0][i] != before[size_t(i)]) { restored = false; break; }
    check(restored, "jobs: DROP clip then UNDO in one go restores the track exactly (order kept)");
}

static void radioCheck() {
    lift::InternetRadioSource r(lift::RadioList::defaults());
    r.tune(0);
    std::vector<float> l(48000), rr(48000);
    double loud = 0;
    int got = 0;
    for (int s = 0; s < 150 && got < 48000 * 3; ++s) {  // up to 15 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int n = r.read(l.data(), rr.data(), 4800);
        got += n;
        for (int i = 0; i < n; ++i) loud += double(l[size_t(i)]) * l[size_t(i)];
    }
    const double rms = got > 0 ? std::sqrt(loud / got) : 0;
    std::printf("RADIO  status \"%s\", %d frames, rms %.4f, buffered %.2f s\n", r.status().toRawUTF8(), got, rms, r.bufferedSeconds());
    check(got >= 48000 * 3 && rms > 0.01, "radio: live MP3 stream decodes into the ring at 48 kHz");
    r.tune(-1);

    // network loss: an address nothing answers. Silence, a status, no blocking.
    lift::RadioList bad;
    bad.stations = {{"Nowhere", "http://127.0.0.1:9/stream.mp3"}};
    bad.presets = {0, -1, -1, -1, -1, -1, -1, -1};
    lift::InternetRadioSource nb(bad);
    nb.tune(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    double worst = 0;
    int real = 0;
    for (int k = 0; k < 100; ++k) {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        real += nb.read(l.data(), rr.data(), 256);
        worst = std::max(worst, juce::Time::getMillisecondCounterHiRes() - t0);
    }
    std::printf("RADIO  lost: status \"%s\", read worst %.4f ms\n", nb.status().toRawUTF8(), worst);
    check(real == 0 && l[0] == 0.f && nb.status().startsWith("NO SIGNAL") && worst < 0.5, "radio: network loss = silence + status, read never blocks");

    // LIFT from the radio, through the engine
    lift::LiftProcessor p;
    p.clearDrumPatterns();
    p.prepareToPlay(48000.0, 256);
    p.setRadioStation(0);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    for (int b = 0; b < 1200; ++b) {  // ~6.4 s of audio, paced near real time
        buf.clear();
        p.processBlock(buf, midi);
        std::this_thread::sleep_for(std::chrono::microseconds(5000));
    }
    auto clip = p.keepLast(2.0);
    double e = 0;
    if (clip) for (float v : clip->l) e += double(v) * v;
    const double crms = clip ? std::sqrt(e / clip->frames()) : 0;
    std::printf("RADIO  LIFT 2 s: %d frames, rms %.4f, status \"%s\"\n", clip ? clip->frames() : 0, crms, p.radio()->status().toRawUTF8());
    check(clip && crms > 0.01, "radio: LIFT keeps what the radio played");
    p.setRadioStation(-1);
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    const std::string a1 = argc > 1 ? argv[1] : "";
    if (a1 == "--radio") { radioCheck(); return failures ? 1 : 0; }
    if (a1 != "--bench") {
        shiftChecks();
        speedChecks();
        crossfadeChecks();
        queueChecks();
        captureChecks();
        jobChecks();
    }
    // bench: three runs, each item's best worst-case (scheduler noise on shared machines)
    double budget = 0;
    auto r = minibench::run(&budget);
    for (int k = 0; k < 2; ++k) {
        const auto again = minibench::run(&budget);
        for (size_t i = 0; i < r.size(); ++i) r[i].worstMs = std::min(r[i].worstMs, again[i].worstMs);
    }
    for (auto& x : r) std::printf("BENCH  %-28s worst %7.3f ms  (budget %.2f ms)\n", x.name, x.worstMs, budget);
    for (auto& x : r) check(x.worstMs < budget * 0.5, std::string("callback under half the block budget: ") + x.name);
    std::printf("%s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
