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

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String base = juce::SystemStats::getEnvironmentVariable("LIFT_RENDER_DIR", "lift-renders");
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile(base).getChildFile("ui");
    dir.createDirectory();

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
        snap(panel, dir, juce::String("juce_") + m + ".png");
    }
    panel.act("mode:tape");
    panel.act("bay");
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

    std::printf("%s (%d failed)\n", g_fails == 0 ? "ALL PANEL CHECKS PASSED" : "PANEL CHECKS FAILED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
