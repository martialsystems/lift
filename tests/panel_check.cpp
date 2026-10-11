// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Headless check of the JUCE panel. Renders each screen to PNG (prototype
// size, 1432 x 996) and drives the panel's actions, the same handlers the mouse
// and keyboard call, against the real tape engine: REC + PLAY with a keyboard
// note, STOP, LIFT, DROP, PLAY. Exit code 0 means every check passed.

#include "LiftPanel.h"
#include "LiftProcessor.h"
#include "PanelData.h"
#include "tape/transport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <cstdio>
#include <vector>

int runUiBench(float backingScale);
void runStateChecks(const std::function<void(bool, const juce::String&)>& check);
void runKnobChecks(const std::function<void(bool, const juce::String&)>& check);
int runDemoRenders(const juce::String& dir, const std::function<void(bool, const juce::String&)>& check);

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
        proc.clearDrumPatterns();  // the tape alone (the drum sequencer follows the transport)
        panel = std::make_unique<lift::LiftPanel>(proc);
    }
    void step(double dt) {
        carry += dt * 48000.0;
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        while (carry >= 512.0) {
            midi.clear();
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

    if (argc > 2 && juce::String(argv[1]) == "--demo") {
        int bad = 0;
        runDemoRenders(argv[2], [&](bool ok, const juce::String& what) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
            bad += ok ? 0 : 1;
        });
        return bad == 0 ? 0 : 1;
    }
    if (argc > 2 && juce::String(argv[1]) == "--panel-shot") {
        // the full panel at 2x, default state (TAPE screen, the default patch)
        Rig r;
        r.step(0.5);
        r.panel->advance(1.0);
        const juce::Image img = r.panel->createComponentSnapshot(r.panel->getLocalBounds(), true, 2.0f);
        const juce::File f(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
        f.deleteFile();
        juce::FileOutputStream os(f);
        juce::PNGImageFormat().writeImageToStream(img, os);
        std::printf("wrote %s (%d x %d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "--ui-bench") {
        return runUiBench(argc > 2 ? juce::String(argv[2]).getFloatValue() : 2.f);
    }
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
            midi.clear();
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
    // the real voice: two detuned oscillators through the spring and the tape's
    // wow, so look for the fundamental within a few Hz of 220
    double a220 = 0.0;
    for (double hz = 214.0; hz <= 226.0; hz += 0.5) {
        a220 = std::max(a220, toneAmp(t1, 4800, 43200, hz));
    }
    std::printf("  T1 printed rms %.4f, 214-226 Hz amp %.4f\n", recRms, a220);
    check(recRms > 0.02 && a220 > 0.01, "the keyboard note was printed onto T1");

    // SCRUB to the top while stopped, LIFT (loop on: first 8 s of T1), arm T2, DROP.
    // (SCRUB is a jog now: it moves the tape by hand, so seek to the top directly)
    proc.send(lift::Cmd::Seek, 0, 0, 0.0);
    run(0.02);
    check(proc.uiPos.load() < 1.0, "seek returns the playhead to the top while stopped");
    panel.act("lift");
    panel.act("arm:1");
    panel.act("drop");
    run(0.02);
    const double t2Rms = rms(rt.ch[1][0], 4800, 43200);
    std::printf("  T2 after DROP rms %.4f\n", t2Rms);
    // LIFT keeps what was just heard (the capture), trimmed to the keep target
    const auto kept = panel.keptClip();
    const float keptLu = kept ? lift::eng::integratedLufs(kept->l.data(), kept->r.data(), kept->frames(), 48000.0) : -200.f;
    std::printf("  kept %.2f s at %.2f LUFS (in %.2f, trim %+.2f dB)\n", kept ? kept->frames() / 48000.0 : 0.0, keptLu,
                kept ? kept->lufsIn : 0.f, kept ? kept->gainDb : 0.f);
    check(kept != nullptr && std::abs(keptLu - lift::eng::kTargetLufs) < 1.f, "LIFT keeps what was heard, trimmed to the target");
    check(rt.arm == 1 && t2Rms > 0.02, "LIFT + DROP overdubs the kept sound onto T2");

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
    check(panel.cords().size() == 3, "default patch: three cables in the model");

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
    {
        std::printf("-- shift layer\n");
        Rig r;
        auto& p = *r.panel;
        TapeRuntime& rt2 = r.proc.runtime();
        const double fr = rt2.frames;
        for (int i = 0; i < 48000 * 6; ++i) {  // known material on T1 and T2
            rt2.ch[0][0][i] = rt2.ch[0][1][i] = 0.5f * std::sin(2.0 * 3.14159265 * 220.0 * i / 48000.0);
            rt2.ch[1][0][i] = rt2.ch[1][1][i] = 0.25f * std::sin(2.0 * 3.14159265 * 330.0 * i / 48000.0);
        }
        p.pressMem(lift::ui::kShiftSlot);
        r.step(0.2);
        check(p.shiftActive(), "keypad slot above STOP is SHIFT; a tap latches it");
        p.pressMem(2);  // top row, middle: DROP; SHIFT + DROP = the fast commit (keep + place)
        check(p.keptClip() == nullptr && p.shiftActive(), "SHIFT + DROP with nothing heard keeps nothing (SHIFT stays latched)");
        r.proc.send(lift::Cmd::Seek, 0, 0, 48000.0);  // playhead (SCRUB is a jog now)  // SCRUB to 1 s
        r.step(0.05);
        p.pressKey(0);  // white 1: LOOP IN
        r.proc.send(lift::Cmd::Seek, 0, 0, 144000.0);  // playhead (SCRUB is a jog now)
        r.step(0.05);
        p.pressKey(2);  // white 2: LOOP OUT
        r.step(0.05);
        std::printf("  loop %d..%d\n", rt2.loopStart, rt2.loopEnd);
        check(std::abs(rt2.loopStart - 48000) <= 2 && std::abs(rt2.loopEnd - 144000) <= 2,
              "SHIFT + white 1/2 set loop in/out at the playhead (engine loop 1 s .. 3 s)");
        r.proc.send(lift::Cmd::Seek, 0, 0, 96000.0);  // playhead (SCRUB is a jog now)
        r.step(0.05);
        p.pressMem(1);  // SHIFT + LOOP
        r.step(0.05);
        check(std::abs(rt2.loopEnd - 96000) <= 2 && std::abs(rt2.loopStart - 48000) <= 2,
              "SHIFT + LOOP ends the loop at the playhead (1 s .. 2 s)");
        p.act("play");
        p.pressKey(6);  // black 3: 2x
        r.step(1.5);
        check(std::abs(r.proc.speed.load() - 2.f) < 1e-3f && std::abs(rt2.engine.speedNow - 2.f) < 0.05f,
              "SHIFT + black 3 sets 2x speed in the engine");
        check(rt2.pos >= 48000.0 && rt2.pos < 96000.0, "playback stays inside the shifted loop");
        p.pressKey(1);  // black 1: 0.5x
        r.step(0.6);
        check(std::abs(rt2.engine.speedNow - 0.5f) < 0.05f, "SHIFT + black 1 sets 0.5x speed");
        p.pressMem(9);  // SHIFT + STOP
        r.step(1.0 / 60.0);
        check(!rt2.playing && rt2.engine.ramp == 0.f, "SHIFT + STOP stops at once (no tape-stop ramp)");
        p.pressMem(0);  // SHIFT + LIFT: all tracks, loop region
        r.step(0.05);
        {
            const auto k = p.keptClip();
            const float want = (0.5f * std::sin(2.0 * 3.14159265 * 220.0 * 48100 / 48000.0) +
                                0.25f * std::sin(2.0 * 3.14159265 * 330.0 * 48100 / 48000.0)) * std::pow(10.f, (k ? k->gainDb : 0.f) / 20.f);
            check(k != nullptr && k->frames() == 48000 && std::abs(k->l[100] - want) < 0.02f,
                  "SHIFT + LIFT keeps the sum of all tracks (loop region, trimmed)");
        }
        p.pressKey(7);  // white 5 (G): TRACK 2
        r.step(0.02);
        check(rt2.arm == 1, "SHIFT + white 5 arms track 2");
        p.pressKey(13);  // C#4 = black 6: CLEAR (first press only asks)
        r.step(0.1);
        check(rt2.ch[rt2.arm][0][1000] != 0.f, "CLEAR asks before clearing");
        r.proc.send(lift::Cmd::Seek, 0, 0, 48000.0);  // playhead (SCRUB is a jog now)
        r.step(0.05);
        const float before = rt2.ch[1][0][53000];
        p.act("drop");  // DROP (SHIFT + DROP is save): overdub the lifted sum onto T2
        r.step(0.05);
        const float dropped = rt2.ch[1][0][53000];
        p.pressKey(10);  // black 5: UNDO DROP
        r.step(0.05);
        std::printf("  T2 at 1 s: %.3f -> drop %.3f -> undo %.3f\n", before, dropped, rt2.ch[1][0][53000]);
        check(std::abs(dropped - before) > 1e-3f && rt2.ch[1][0][53000] == before, "DROP overdubs the kept sum; UNDO restores the track");
        p.pressKey(13);
        r.step(0.6);
        check(rt2.ch[1][0][1000] == 0.f && rt2.ch[1][0][48000 * 5] == 0.f && std::abs(rt2.ch[0][0][1000] - 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265 * 220.0 * 1000 / 48000.0))) < 1e-6f,
              "CLEAR on the second press clears only the armed track");
        p.pressMem(lift::ui::kShiftSlot);
        check(!p.shiftActive(), "tap SHIFT again releases the latch");
        p.setShiftKey(true);
        check(p.shiftActive(), "holding the computer Shift key shifts");
        p.setShiftKey(false);
        check(!p.shiftActive(), "releasing it unshifts");
        p.act("mode:synth");
        p.setShiftKey(true);
        p.pressKey(9);  // white 6 on SYNTH: OCT +2
        p.setShiftKey(false);
        p.noteOn(9);
        r.step(0.1);
        check(r.proc.uiSynthNote.load() == 48 + 24 + 9, "SHIFT + white 6 on SYNTH sets octave +2 for the keyboard");
        p.noteOff();
    }
    {
        // shift off vs on, full panel (pixel diffs are checked in the render script)
        lift::LiftProcessor pr;
        pr.prepareToPlay(48000.0, 512);
        lift::LiftPanel pn(pr);
        pn.advance(0.0);
        snap(pn, dir, "juce_shift_off.png");
        pn.setShiftKey(true);
        pn.advance(0.0);
        pn.advance(1.0);
        snap(pn, dir, "juce_shift_on.png");
        pn.setShiftKey(false);
        pn.openPicker(lift::LiftPanel::Picker::Save);
        pn.pickStep(41);
        pn.advance(0.0);
        pn.advance(1.0);
        snap(pn, dir, "juce_slot_picker.png");
        pn.pickCancel();
    }
    {
        std::printf("-- resample: keep, fast path, SELECT, one undo history\n");
        Rig r;
        auto& p = *r.panel;
        TapeRuntime& rt = r.proc.runtime();
        auto trackRms = [&](int t, int a, int b) { return rms(rt.ch[t][0], a, b); };
        p.act("mode:synth");
        p.noteOn(9);
        r.step(2.0);
        p.noteOff();
        r.step(0.3);
        const int h0 = p.historySize();
        p.setPin(0, 0, 1);
        check(p.historySize() == h0 + 1, "a pin joins the history");
        p.act("lift");
        const auto kept = p.keptClip();
        const float lu = kept ? lift::eng::integratedLufs(kept->l.data(), kept->r.data(), kept->frames(), 48000.0) : -200.f;
        std::printf("  LIFT kept %.2f s, %.2f LUFS in -> %.2f LUFS\n", kept ? kept->frames() / 48000.0 : 0.0,
                    kept ? kept->lufsIn : 0.f, lu);
        check(kept != nullptr && std::abs(lu - lift::eng::kTargetLufs) < 1.f, "LIFT keeps the last seconds heard, trimmed to target");
        const lift::eng::Selection def = p.selection();
        check(def.length > 0 && def.snap == lift::eng::SNAP_ZERO && !p.selectIsOpen(),
              "a keep comes with its default selection (snap ZERO); SELECT stays shut");
        r.proc.send(lift::Cmd::Seek, 0, 0, 0.0);
        r.step(0.05);
        p.act("drop");
        r.step(0.05);
        const double t1 = trackRms(0, 0, 48000);
        check(!p.selectIsOpen() && t1 > 0.01 && p.lastDest() == 0, "DROP without SELECT places the default selection on T1");
        p.setEnc(3, p.enc(0, 3) + 0.05f);  // red, right after the keep: opens SELECT and moves the start
        r.step(0.05);
        check(p.selectIsOpen() && !(p.selection() == def), "a knob turned after a keep opens SELECT (red moves the start)");
        snap(p, dir, "juce_select.png");
        p.undo();
        check(p.selectIsOpen() && p.selection() == def, "UNDO with SELECT open reverts the SELECT edit first");
        p.undo();
        r.step(0.05);
        check(trackRms(0, 0, 48000) < 1e-6, "the next UNDO takes the DROP back off T1");
        p.undo();
        check(p.keptClip() == nullptr && !p.selectIsOpen(), "then the KEEP");
        p.undo();
        check(p.pin(0, 0) == 0, "then the pin (one history for cables, pins, keeps, drops)");
        // overdubs: pass 1 finished, pass 2 undone while it records
        p.act("arm:2");
        r.step(0.05);
        p.act("play");
        p.act("rec");
        p.noteOn(4);
        r.step(1.0);
        p.noteOff();
        p.act("rec");  // REC off, tape keeps playing: pass 1 done (and kept as a clip)
        r.step(0.3);
        std::vector<float> after1(rt.ch[2][0], rt.ch[2][0] + 48000 * 3);
        const double pass1 = rms(after1.data(), 0, 48000 * 3);
        check(pass1 > 0.01 && p.keptClip() != nullptr, "an overdub pass prints on T3 and REC keeps what was heard");
        p.act("rec");  // pass 2
        p.noteOn(7);
        r.step(0.6);
        p.undo();  // mid-pass
        p.noteOff();
        r.step(0.3);
        double diff = 0.0;
        for (int i = 0; i < 48000 * 3; ++i) diff = std::max(diff, static_cast<double>(std::abs(rt.ch[2][0][i] - after1[static_cast<size_t>(i)])));
        std::printf("  pass 1 rms %.4f; after UNDO mid pass 2: max diff %.6f\n", pass1, diff);
        check(!rt.recording && diff < 1e-6, "UNDO mid-overdub discards the current pass only");
        p.undo();  // the REC keep
        p.undo();  // pass 1
        r.step(0.3);
        check(rms(rt.ch[2][0], 0, 48000 * 3) < 1e-6, "UNDO again: the REC keep, then pass 1 comes off T3");
        // SHIFT + DROP: keep and place in one go, into the last place used (T1)
        p.act("stop");
        r.step(1.0);
        p.noteOn(9);
        r.step(1.0);
        p.noteOff();
        r.step(0.2);
        const int h1 = p.historySize();
        p.act("dropnow");
        r.step(0.05);
        check(p.historySize() == h1 + 2 && p.lastDest() == 0 && trackRms(0, 0, 48000 * 8) > 0.01 && !p.selectIsOpen(),
              "SHIFT + DROP keeps with the default selection and places on the last place (T1), no SELECT");
        // hold DROP + a key: the key plays the sound as a one-shot
        p.act("droppick");
        p.noteOn(0);
        check(p.lastDest() == 5, "hold DROP + a key puts the sound on that key");
        // export
        const juce::File ex = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lift-export-check");
        ex.deleteRecursively();
        p.exportTo(ex);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rd(wav.createReaderFor(ex.getChildFile("LIFT-T1.wav").createInputStream().release(), true));
        check(rd != nullptr && rd->bitsPerSample == 24 && rd->sampleRate == 48000.0 && ex.getChildFile("LIFT-master.wav").existsAsFile() &&
                  ex.getChildFile("LIFT-clip.wav").existsAsFile(),
              "export writes the clip, T1-T4 and the master as 24-bit 48 kHz WAV");
        // P4: the REC jack / column set to press LIFT; PULSE pinned to REC
        r.proc.recJackLifts.store(true);
        const int lifts0 = r.proc.uiLiftPresses.load();
        const auto before = p.keptClip();
        p.setPin(11, 11, 1);  // PULSE -> REC
        p.act("play");
        r.step(1.5);
        std::printf("  PULSE -> REC (presses LIFT): %d presses\n", r.proc.uiLiftPresses.load() - lifts0);
        check(r.proc.uiLiftPresses.load() > lifts0 && p.keptClip() != before && !rt.recording,
              "PULSE pinned to REC, REC set to press LIFT: the pulses keep (LIFT), never record");
        p.act("stop");
        r.step(0.5);
    }
    {
        std::printf("-- GRAIN FX: resampling keeps the live cloud\n");
        Rig r;
        r.proc.send(lift::Cmd::FxType, lift::eng::FX_GRAIN);
        r.proc.fxOn.store(true);
        r.proc.fxKnobs[0].store(0.f);   // POS: the newest audio
        r.proc.fxKnobs[1].store(0.35f);
        r.proc.fxKnobs[2].store(0.7f);
        r.proc.fxKnobs[3].store(1.f);   // all cloud, no dry
        r.proc.send(lift::Cmd::NoteOn, 57, 100);  // A3 220 Hz
        r.step(1.5);
        r.proc.send(lift::Cmd::NoteOff, 57);
        r.proc.send(lift::Cmd::NoteOn, 69, 100);  // A4 440 Hz
        r.step(1.5);
        r.proc.send(lift::Cmd::NoteOff, 69);
        const auto c = r.proc.keepLast(3.0);
        bool ok = c != nullptr;
        double a1 = 0, b1 = 0, a2 = 0, b2 = 0;
        if (ok) {
            const int h = c->frames() / 2;
            a1 = toneAmp(c->l.data(), 4800, h - 4800, 220.0);
            b1 = toneAmp(c->l.data(), 4800, h - 4800, 440.0);
            a2 = toneAmp(c->l.data(), h + 9600, c->frames() - 2400, 220.0);
            b2 = toneAmp(c->l.data(), h + 9600, c->frames() - 2400, 440.0);
        }
        std::printf("  kept cloud: first half 220 %.4f / 440 %.4f, second half 220 %.4f / 440 %.4f\n", a1, b1, a2, b2);
        check(ok && a1 > 5.0 * a2 && b2 > 2.0 * b1, "a keep of the GRAIN effect is the cloud as it evolved (220 Hz, then 440 Hz), not a frozen window");
    }
    runStateChecks([](bool ok, const juce::String& what) { check(ok, what); });
    runKnobChecks([](bool ok, const juce::String& what) { check(ok, what); });
    if (anim) {
        std::printf("-- shift frames\n");
        const juce::File sdir = dir.getParentDirectory().getChildFile("shift");
        sdir.deleteRecursively();
        sdir.createDirectory();
        Rig r;
        auto& p = *r.panel;
        const juce::Rectangle<int> crop =
            juce::Rectangle<float>(lift::ui::kDevX + lift::ui::kMainDX + 40.f, lift::ui::kDevY + lift::ui::kMainDY + 236.f, 644.f, 470.f).getSmallestIntegerContainer();
        std::vector<Event> ev = {{0.1, [](lift::LiftPanel& q) { q.act("play"); }},
                                 {0.5, [](lift::LiftPanel& q) { q.setShiftKey(true); }},
                                 {1.1, [](lift::LiftPanel& q) { q.pressKey(6); }},
                                 {1.6, [](lift::LiftPanel& q) { q.pressKey(0); }},
                                 {2.1, [](lift::LiftPanel& q) { q.setShiftKey(false); }},
                                 {2.7, [](lift::LiftPanel& q) { q.pressMem(lift::ui::kShiftSlot); }},
                                 {3.2, [](lift::LiftPanel& q) { q.pressMem(1); }},
                                 {3.8, [](lift::LiftPanel& q) { q.pressMem(9); }},
                                 {4.4, [](lift::LiftPanel& q) { q.pressMem(lift::ui::kShiftSlot); }}};
        size_t next = 0;
        const int n = static_cast<int>(5.0 * 30.0);
        for (int f = 0; f < n; ++f) {
            while (next < ev.size() && ev[next].t <= f / 30.0 + 1e-9) {
                ev[next].f(p);
                ++next;
            }
            r.step(1.0 / 30.0);
            const juce::Image img = p.createComponentSnapshot(crop, true, 1.0f);
            juce::FileOutputStream os(sdir.getChildFile(juce::String::formatted("f%03d.png", f)));
            juce::PNGImageFormat().writeImageToStream(img, os);
        }
        std::printf("  %d frames in %s\n", n, sdir.getFullPathName().toRawUTF8());
    }
    if (anim) {
        std::printf("-- animation frames\n");
        runAnim(dir.getParentDirectory());
    }

    std::printf("%s (%d failed)\n", g_fails == 0 ? "ALL PANEL CHECKS PASSED" : "PANEL CHECKS FAILED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
