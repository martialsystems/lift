// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Headless check of the JUCE panel. Renders each screen to PNG (prototype
// size, 1432 x 996) and drives the panel's actions, the same handlers the mouse
// and keyboard call, against the real tape engine: REC + PLAY with a keyboard
// note, STOP, LIFT, DROP, PLAY. Exit code 0 means every check passed.

#include "LiftPanel.h"
#include "LiftProcessor.h"
#include "tape/transport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <cstdio>
#include <vector>

namespace {

int g_fails = 0;

void check(bool ok, const juce::String& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (!ok) {
        ++g_fails;
    }
}

void snap(lift::LiftPanel& panel, const juce::File& dir, const juce::String& name) {
    const juce::Image img = panel.createComponentSnapshot(panel.getLocalBounds(), true, 1.0f);
    const juce::File f = dir.getChildFile(name);
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat png;
    png.writeImageToStream(img, os);
    std::printf("  wrote %s\n", f.getFullPathName().toRawUTF8());
}

double toneAmp(const float* x, int from, int to, double hz) {
    double re = 0.0, im = 0.0;
    for (int i = from; i < to; ++i) {
        const double ph = 2.0 * juce::MathConstants<double>::pi * hz * i / 48000.0;
        re += x[i] * std::cos(ph);
        im += x[i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (to - from);
}

double rms(const float* x, int from, int to) {
    double s = 0.0;
    for (int i = from; i < to; ++i) {
        s += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(s / juce::jmax(1, to - from));
}

// Drives one panel + processor pair in lock step: audio for dt, then the
// screen clock for dt. Frames come from the screen area only (the same fast
// path the animation timer repaints).
struct Rig {
    lift::LiftProcessor proc;
    std::unique_ptr<lift::LiftPanel> panel;
    double carry = 0.0;
    double paintMs = 0.0, paintMax = 0.0;
    int painted = 0;
    Rig() {
        proc.prepareToPlay(48000.0, 512);
        panel = std::make_unique<lift::LiftPanel>(proc);
    }
    void step(double dt) {
        carry += dt * 48000.0;
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        while (carry >= 512.0) {
            buf.clear();
            proc.processBlock(buf, midi);
            carry -= 512.0;
        }
        panel->advance(dt);
    }
    juce::Image frame() {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        juce::Image img = panel->createComponentSnapshot(lift::LiftPanel::screenArea(), true, 1.0f);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        paintMs += ms;
        paintMax = juce::jmax(paintMax, ms);
        ++painted;
        return img;
    }
};

bool sameImage(const juce::Image& a, const juce::Image& b) {
    if (a.getBounds() != b.getBounds()) {
        return false;
    }
    const juce::Image::BitmapData da(a, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData db(b, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < a.getHeight(); ++y) {
        if (std::memcmp(da.getLinePointer(y), db.getLinePointer(y), static_cast<size_t>(a.getWidth() * da.pixelStride)) != 0) {
            return false;
        }
    }
    return true;
}

struct Event {
    double t;
    std::function<void(lift::LiftPanel&)> f;
};

// Renders one screen's frame sequence (30 fps) into dir/<name>/f###.png.
int renderSequence(const juce::File& dir, const juce::String& name, double seconds, std::vector<Event> events,
                   const std::function<void(Rig&)>& setup, double& msSum, double& msMax, int& frames) {
    Rig rig;
    if (setup) {
        setup(rig);
    }
    const juce::File sub = dir.getChildFile(name);
    sub.deleteRecursively();
    sub.createDirectory();
    const double dt = 1.0 / 30.0;
    const int n = static_cast<int>(seconds / dt);
    size_t next = 0;
    int distinct = 0;
    juce::Image last;
    for (int f = 0; f < n; ++f) {
        const double t = f * dt;
        while (next < events.size() && events[next].t <= t + 1e-9) {
            events[next].f(*rig.panel);
            ++next;
        }
        rig.step(dt);
        juce::Image img = rig.frame();
        if (last.isValid() && !sameImage(img, last)) {
            ++distinct;
        }
        last = img;
        juce::FileOutputStream os(sub.getChildFile(juce::String::formatted("f%03d.png", f)));
        juce::PNGImageFormat().writeImageToStream(img, os);
    }
    msSum += rig.paintMs;
    msMax = juce::jmax(msMax, rig.paintMax);
    frames += rig.painted;
    std::printf("  %-6s %d frames, %d changed from the previous frame, paint %.2f ms avg\n", name.toRawUTF8(), n, distinct, rig.paintMs / juce::jmax(1, rig.painted));
    return distinct;
}

void ramp(std::vector<Event>& ev, double t0, double t1, int enc, float v0, float v1) {
    for (double t = t0; t <= t1 + 1e-9; t += 1.0 / 30.0) {
        const float u = static_cast<float>((t - t0) / (t1 - t0));
        ev.push_back({t, [enc, v = v0 + (v1 - v0) * u](lift::LiftPanel& p) { p.setEnc(enc, v); }});
    }
}

void sortEvents(std::vector<Event>& ev) {
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
}

int runAnim(const juce::File& base) {
    const juce::File dir = base.getChildFile("anim");
    dir.createDirectory();
    double msSum = 0.0, msMax = 0.0;
    int frames = 0;
    int ok = 0;
    auto act = [](const char* a) { return [s = juce::String(a)](lift::LiftPanel& p) { p.act(s); }; };
    {  // TAPE: record a note, speed up, tape-stop, LIFT, scrub back, DROP, play
        std::vector<Event> ev = {{0.1, act("rec")},
                                 {0.1, act("play")},
                                 {0.1, [](lift::LiftPanel& p) { p.noteOn(9); }},
                                 {1.3, [](lift::LiftPanel& p) { p.noteOff(); }},
                                 {2.6, act("stop")},
                                 {3.2, act("lift")},
                                 {4.4, act("drop")},
                                 {5.0, act("play")}};
        ramp(ev, 1.4, 1.8, 0, 0.5f, 0.85f);
        ramp(ev, 3.6, 4.1, 3, 0.05f, 0.f);
        ramp(ev, 5.6, 6.0, 0, 0.85f, 0.5f);
        sortEvents(ev);
        ok += renderSequence(dir, "tape", 6.6, ev, nullptr, msSum, msMax, frames) > 20;
    }
    {  // SYNTH: slide in, notes (live scope), engine switches
        std::vector<Event> ev = {{0.1, act("mode:synth")},
                                 {0.6, [](lift::LiftPanel& p) { p.noteOn(9); }},
                                 {1.4, [](lift::LiftPanel& p) { p.noteOff(); }},
                                 {1.8, act("num:1")},
                                 {2.3, [](lift::LiftPanel& p) { p.noteOn(4); }},
                                 {3.0, [](lift::LiftPanel& p) { p.noteOff(); }},
                                 {3.4, act("num:3")},
                                 {3.9, act("num:0")}};
        ramp(ev, 2.4, 2.9, 0, 0.35f, 0.9f);
        sortEvents(ev);
        ok += renderSequence(dir, "synth", 4.6, ev, nullptr, msSum, msMax, frames) > 20;
    }
    {  // DRUM: playhead on the tape clock
        std::vector<Event> ev = {{0.1, act("mode:drum")}, {0.5, act("play")}, {3.6, act("stop")}};
        ok += renderSequence(dir, "drum", 4.2, ev, nullptr, msSum, msMax, frames) > 20;
    }
    {  // MIX: meters from recorded tracks, fader and EQ springs, mute squash
        auto setup = [](Rig& r) {
            auto& p = *r.panel;
            p.act("rec");
            p.act("play");
            p.noteOn(9);
            for (int i = 0; i < 60; ++i) {
                r.step(1.0 / 30.0);
            }
            p.noteOff();
            p.act("stop");
            for (int i = 0; i < 20; ++i) {
                r.step(1.0 / 30.0);
            }
            p.setEnc(3, 0.f);
            p.act("lift");
            p.act("arm:1");
            p.act("drop");
            p.act("arm:0");
            r.step(1.0 / 30.0);
        };
        std::vector<Event> ev = {{0.1, act("mode:mix")},
                                 {0.4, act("play")},
                                 {1.0, [](lift::LiftPanel& p) { p.setEnc(0, 0.3f); }},
                                 {1.6, act("mute:1")},
                                 {2.0, [](lift::LiftPanel& p) { p.setEnc(2, 0.95f); }},
                                 {2.4, [](lift::LiftPanel& p) { p.setEnc(3, 0.1f); }},
                                 {2.9, act("mute:1")},
                                 {3.2, [](lift::LiftPanel& p) { p.setEnc(0, 0.71f); }}};
        ok += renderSequence(dir, "mix", 4.0, ev, setup, msSum, msMax, frames) > 20;
    }
    {  // IN: radio needle with overshoot, meters, threshold pulse
        std::vector<Event> ev = {{0.1, act("radio")},
                                 {0.6, [](lift::LiftPanel& p) { p.setEnc(0, 0.72f); }},
                                 {1.6, [](lift::LiftPanel& p) { p.setEnc(0, 0.1f); }},
                                 {2.4, [](lift::LiftPanel& p) { p.setEnc(3, 0.42f); }},
                                 {3.0, [](lift::LiftPanel& p) { p.setEnc(0, 0.3f); }}};
        ok += renderSequence(dir, "in", 3.8, ev, nullptr, msSum, msMax, frames) > 20;
    }
    {  // BAY: rows deal in, dots travel, feedback tag pulses
        std::vector<Event> ev = {{0.1, act("bay")}};
        ok += renderSequence(dir, "bay", 3.0, ev, nullptr, msSum, msMax, frames) > 20;
    }
    std::printf("  screen paint: %.2f ms average, %.2f ms worst over %d frames (1x, software renderer)\n",
                msSum / juce::jmax(1, frames), msMax, frames);
    check(ok == 6, "every screen animates (frames change over time)");
    check(msSum / juce::jmax(1, frames) < 8.0, "average screen frame paints well inside a 60 fps budget");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String base = juce::SystemStats::getEnvironmentVariable("LIFT_RENDER_DIR", "lift-renders");
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile(base).getChildFile("ui");
    dir.createDirectory();

    const bool anim = argc > 1 && juce::String(argv[1]) == "--anim";
    if (argc > 1 && juce::String(argv[1]) == "--bench") {
        // Screen repaint cost per animation frame (TAPE, tape running).
        Rig r;
        r.panel->act(argc > 2 ? juce::String(argv[2]) : juce::String("play"));
        r.step(0.1);
        r.frame();
        r.paintMs = r.paintMax = 0.0;
        r.painted = 0;
        for (int i = 0; i < 600; ++i) {
            r.step(1.0 / 60.0);
            r.frame();
        }
        std::printf("bench: %.2f ms avg, %.2f ms worst\n", r.paintMs / r.painted, r.paintMax);
        return 0;
    }

    lift::LiftProcessor proc;
    proc.prepareToPlay(48000.0, 512);
    lift::LiftPanel panel(proc);

    std::vector<float> outL;
    auto run = [&](double seconds) {
        const int total = static_cast<int>(seconds * 48000.0);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        for (int done = 0; done < total; done += 512) {
            buf.clear();
            proc.processBlock(buf, midi);
            outL.insert(outL.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
            panel.tick();
        }
    };

    std::printf("-- screens\n");
    snap(panel, dir, "juce_tape.png");
    for (const char* m : {"synth", "drum", "mix", "in"}) {
        panel.act(juce::String("mode:") + m);
        panel.advance(0.0);
        panel.advance(1.5);  // let the slide and the springs settle
        snap(panel, dir, juce::String("juce_") + m + ".png");
    }
    panel.act("mode:tape");
    panel.act("bay");
    panel.advance(0.0);
        panel.advance(1.5);
    snap(panel, dir, "juce_bay.png");
    panel.act("mode:tape");

    std::printf("-- UI actions against the tape engine\n");
    TapeRuntime& rt = proc.runtime();
    check(panel.mode() == lift::LiftPanel::Tape && rt.arm == 0, "TAPE screen, T1 armed");
    // REC, PLAY, then hold A (220 Hz at octave 0) for one second.
    panel.act("rec");
    panel.act("play");
    panel.noteOn(9);
    run(0.05);
    check(proc.uiRecording.load() && proc.uiReelSpeed.load() > 0.f, "REC + PLAY records and the reels turn");
    run(0.95);
    snap(panel, dir, "juce_tape_recording.png");
    panel.noteOff();
    run(0.3);
    panel.act("stop");
    run(0.6);
    check(!proc.uiPlaying.load(), "STOP ramps the tape down and stops the transport");
    const float* t1 = rt.ch[0][0];
    const double recRms = rms(t1, 4800, 43200);
    const double a220 = toneAmp(t1, 4800, 43200, 220.0);
    std::printf("  T1 printed rms %.4f, 220 Hz amp %.4f\n", recRms, a220);
    check(recRms > 0.02 && a220 > 0.02, "the keyboard note was printed onto T1");

    // SCRUB to the top while stopped, LIFT (loop on: first 8 s of T1), arm T2, DROP.
    panel.setEnc(3, 0.f);
    run(0.02);
    check(proc.uiPos.load() < 1.0, "SCRUB returns the playhead to the top while stopped");
    panel.act("lift");
    panel.act("arm:1");
    panel.act("drop");
    run(0.02);
    const double t2Rms = rms(rt.ch[1][0], 4800, 43200);
    std::printf("  T2 after DROP rms %.4f\n", t2Rms);
    check(rt.arm == 1 && std::abs(t2Rms - recRms) < recRms * 0.05, "LIFT + DROP overdubs the take onto T2");

    // PLAY back: T1 + T2 through the playback chain.
    const size_t from = outL.size();
    panel.act("play");
    run(1.0);
    const float* o = outL.data() + from;
    const int n = static_cast<int>(outL.size() - from);
    bool finite = true;
    for (int i = 0; i < n; ++i) {
        finite = finite && std::isfinite(o[i]);
    }
    const double playRms = rms(o, 4800, juce::jmin(n, 43200));
    const double p220 = toneAmp(o, 4800, juce::jmin(n, 43200), 220.0);
    const double p300 = toneAmp(o, 4800, juce::jmin(n, 43200), 300.0);
    std::printf("  playback rms %.4f, 220 Hz %.4f, 300 Hz %.4f, pos %.0f\n", playRms, p220, p300, proc.uiPos.load());
    check(finite && playRms > 0.03 && p220 > 5.0 * p300, "PLAY plays the takes back (220 Hz, finite)");
    check(proc.uiPos.load() > 0.9 * 48000.0, "tape counter follows the transport");
    panel.act("stop");
    run(0.5);
    check(panel.cords().size() == 6, "default patch: six cables in the model");

    {
        // The screen animates while the tape runs and is still at rest.
        std::printf("-- screen motion\n");
        Rig r;
        r.step(0.1);
        const juce::Image a = r.frame();
        r.step(0.1);
        check(sameImage(a, r.frame()), "TAPE screen at rest is still");
        r.panel->act("play");
        r.step(0.2);
        const juce::Image b = r.frame();
        r.step(1.0 / 60.0);
        check(!sameImage(b, r.frame()), "TAPE screen moves while the tape runs");
        r.panel->act("stop");
        r.step(1.0);
        r.panel->setReducedMotion(true);
        r.panel->act("mode:synth");
        r.step(1.0 / 60.0);
        const juce::Image c = r.frame();
        r.step(0.25);
        check(sameImage(c, r.frame()), "reduced motion: idle SYNTH screen is still");
    }
    if (anim) {
        std::printf("-- animation frames\n");
        runAnim(dir.getParentDirectory());
    }

    std::printf("%s (%d failed)\n", g_fails == 0 ? "ALL PANEL CHECKS PASSED" : "PANEL CHECKS FAILED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
