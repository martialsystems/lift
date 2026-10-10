// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Knob audibility: every knob that drives sound is turned through the panel
// component itself (a mouse drag on the knob, the same events a user makes)
// and the change in the audio is measured against the same passage with the
// knob left alone. Also checks that a fast sweep is free of zipper steps.

#include "LiftPanel.h"
#include "LiftProcessor.h"
#include "PanelData.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

using Check = std::function<void(bool, const juce::String&)>;

struct Audio {
    std::vector<float> l, r;
};

struct Rig {
    lift::LiftProcessor p;
    std::unique_ptr<lift::LiftPanel> panel;
    Rig() {
        p.prepareToPlay(48000.0, 512);
        panel = std::make_unique<lift::LiftPanel>(p);
        panel->setSize(lift::LiftPanel::kW, lift::LiftPanel::kH);
    }
    // seconds of audio; the panel's frame clock steps once per block
    // (`during` runs before each block, e.g. to move the mouse)
    Audio run(double secs, const std::function<void(double)>& during = {}) {
        Audio a;
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        const int blocks = static_cast<int>(secs * 48000.0 / 512.0);
        for (int b = 0; b < blocks; ++b) {
            if (during) {
                during(static_cast<double>(b) / juce::jmax(1, blocks - 1));
            }
            buf.clear();
            midi.clear();
            p.processBlock(buf, midi);
            a.l.insert(a.l.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
            a.r.insert(a.r.end(), buf.getReadPointer(1), buf.getReadPointer(1) + 512);
            panel->tick();
        }
        return a;
    }
    juce::MouseEvent ev(juce::Point<float> pos, juce::Point<float> down, bool drag, bool button) {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto mods = button ? juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys();
        return juce::MouseEvent(src, pos, mods, 1.f, 0.f, 0.f, 0.f, 0.f, panel.get(), panel.get(), juce::Time::getCurrentTime(),
                                down, juce::Time::getCurrentTime(), 1, drag);
    }
    juce::Point<float> knob(int k) const {
        const auto c = lift::ui::knobCentre(k);
        return {lift::ui::kDevX + c.x, lift::ui::kDevY + c.y};
    }
    // press on knob k, drag vertically by dy px (canvas = component px at 100 %)
    // spread over `secs` of audio, release. Returns the audio during the drag.
    Audio turn(int k, float dy, double secs) {
        const auto k0 = knob(k);
        panel->mouseDown(ev(k0, k0, false, true));
        auto a = run(secs, [&](double t) { panel->mouseDrag(ev(k0.translated(0.f, -dy * static_cast<float>(t)), k0, true, true)); });
        panel->mouseDrag(ev(k0.translated(0.f, -dy), k0, true, true));
        panel->mouseUp(ev(k0.translated(0.f, -dy), k0, false, false));
        return a;
    }
    float value(int screen, int k) const { return p.uiState().enc[static_cast<size_t>(screen)][static_cast<size_t>(k)]; }
};

double rms(const std::vector<float>& x, size_t a = 0, size_t b = 0) {
    b = b == 0 ? x.size() : b;
    double s = 0.0;
    for (size_t i = a; i < b; ++i) {
        s += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(s / static_cast<double>(juce::jmax<size_t>(1, b - a)));
}
double db(double a, double b) { return 20.0 * std::log10((a + 1e-9) / (b + 1e-9)); }

// average magnitude spectrum (Hann, 4096)
std::vector<double> spectrum(const std::vector<float>& x) {
    constexpr int order = 12, n = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<double> acc(n / 2, 0.0);
    std::vector<float> buf(2 * n);
    int frames = 0;
    for (size_t s = 0; s + n <= x.size(); s += n / 2) {
        for (int i = 0; i < n; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.f * juce::MathConstants<float>::pi * static_cast<float>(i) / n);
            buf[static_cast<size_t>(i)] = x[s + static_cast<size_t>(i)] * w;
        }
        std::fill(buf.begin() + n, buf.end(), 0.f);
        fft.performFrequencyOnlyForwardTransform(buf.data());
        for (int i = 0; i < n / 2; ++i) {
            acc[static_cast<size_t>(i)] += buf[static_cast<size_t>(i)];
        }
        ++frames;
    }
    for (auto& v : acc) {
        v /= juce::jmax(1, frames);
    }
    return acc;
}
// spectral centroid (Hz)
double centroid(const std::vector<double>& m) {
    double num = 0.0, den = 0.0;
    for (size_t i = 1; i < m.size(); ++i) {
        num += m[i] * static_cast<double>(i) * 48000.0 / 4096.0;
        den += m[i];
    }
    return den > 0.0 ? num / den : 0.0;
}
// relative spectral distance ||A - B|| / ||A||
double specDist(const std::vector<double>& a, const std::vector<double>& b) {
    double d = 0.0, n = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        d += (a[i] - b[i]) * (a[i] - b[i]);
        n += a[i] * a[i];
    }
    return std::sqrt(d / (n + 1e-18));
}
// strongest partial (Hz), parabolic peak
double peakHz(const std::vector<double>& m) {
    size_t k = 2;
    for (size_t i = 2; i + 1 < m.size(); ++i) {
        if (m[i] > m[k]) {
            k = i;
        }
    }
    const double a = m[k - 1], b = m[k], c = m[k + 1];
    const double d = 0.5 * (a - c) / (a - 2.0 * b + c + 1e-18);
    return (static_cast<double>(k) + d) * 48000.0 / 4096.0;
}
// largest sample-to-sample jump of the second difference: a zipper step in
// the gain or the filter shows as a spike, a smooth change does not
double maxD2(const std::vector<float>& x, size_t a, size_t b) {
    double m = 0.0;
    for (size_t i = a + 2; i < b; ++i) {
        m = juce::jmax(m, std::abs(static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2]));
    }
    return m;
}

}  // namespace

void runKnobChecks(const Check& check) {
    std::printf("-- knobs (turned with the mouse on the panel, audio measured)\n");
    const auto mouseCheck = [&](Rig& r, int screen, int k, float dy, float expect) {
        return std::abs(r.value(screen, k) - expect) < 0.02f;
    };

    // ---- TAPE: put a held A3 on T1 first
    Rig r;
    r.panel->act("mode:tape");
    r.p.send(lift::Cmd::Seek, 0, 0, 0.0);
    r.panel->act("rec");
    r.panel->act("play");
    r.p.send(lift::Cmd::NoteOn, 57, 100);
    r.run(3.0);
    r.p.send(lift::Cmd::NoteOff, 57);
    r.panel->act("stop");
    r.run(0.5);
    auto playFrom0 = [&](double secs) {
        r.panel->act("stop");
        r.run(0.8);  // the stop ramp ends; a seek only lands on stopped tape
        r.p.send(lift::Cmd::Seek, 0, 0, 0.3 * 48000.0);
        r.panel->act("play");
        r.run(0.4);  // speed glide / transport settle
        auto a = r.run(secs);
        r.panel->act("stop");
        r.run(0.3);
        return a;
    };
    {
        const auto base = playFrom0(1.0);
        const float v0 = r.value(2, 0);
        r.turn(0, -110.f, 0.3);  // SPEED down by half a turn
        const auto lo = playFrom0(1.0);
        const double f0 = peakHz(spectrum(base.l)), f1 = peakHz(spectrum(lo.l));
        std::printf("  TAPE SPEED %.2f -> %.2f: tape pitch %.1f Hz -> %.1f Hz (x%.3f)\n", v0, r.value(2, 0), f0, f1, f1 / f0);
        check(mouseCheck(r, 2, 0, -110.f, juce::jmax(0.f, v0 - 0.5f)) && f1 < 0.8 * f0, "TAPE SPEED (mouse) changes the tape pitch");
        r.turn(0, 110.f, 0.1);
    }
    {
        // BIAS is a record setting: heard on the live monitor (the record
        // electronics) and on what gets recorded
        r.p.send(lift::Cmd::NoteOn, 57, 100);
        r.run(0.3);
        const auto base = r.run(1.0);
        const float v0 = r.value(2, 1);
        r.turn(1, -99.f, 0.3);  // BIAS down to 0 (under-biased)
        const auto lo = r.run(1.0);
        r.p.send(lift::Cmd::NoteOff, 57);
        r.run(0.5);
        const auto sa = spectrum(base.l), sb = spectrum(lo.l);
        std::printf("  TAPE BIAS %.2f -> %.2f: centroid %.0f -> %.0f Hz, level %+.1f dB, spectral change %.2f\n", v0, r.value(2, 1),
                    centroid(sa), centroid(sb), db(rms(lo.l), rms(base.l)), specDist(sa, sb));
        check(specDist(sa, sb) > 0.2 || std::abs(std::log(centroid(sb) / centroid(sa))) > 0.15,
              "TAPE BIAS (mouse) audibly changes the tape sound (brightness)");
        r.turn(1, 99.f * 0.45f / 0.45f, 0.1);
    }
    {
        // REC LVL: the live monitor runs through the record electronics
        r.p.send(lift::Cmd::NoteOn, 57, 100);
        r.run(0.3);
        const auto base = r.run(1.0);
        const float v0 = r.value(2, 2);
        r.turn(2, 66.f, 0.3);  // up to 1.0
        const auto hot = r.run(1.0);
        r.p.send(lift::Cmd::NoteOff, 57);
        r.run(0.5);
        const auto sa = spectrum(base.l), sb = spectrum(hot.l);
        std::printf("  TAPE REC LVL %.2f -> %.2f: level %+.1f dB, centroid %.0f -> %.0f Hz, spectral change %.2f\n", v0,
                    r.value(2, 2), db(rms(hot.l), rms(base.l)), centroid(sa), centroid(sb), specDist(sa, sb));
        check(specDist(sa, sb) > 0.2, "TAPE REC LVL (mouse) audibly drives the record chain");
    }
    {
        // SCRUB while stopped: turning moves the tape under the heads and it sounds
        r.p.send(lift::Cmd::Seek, 0, 0, 48000.0);
        r.run(0.3);
        const double p0 = r.p.uiPos.load();
        const auto quiet = r.run(0.3);
        const auto scrub = r.turn(3, 120.f, 0.6);
        r.run(0.3);
        const double p1 = r.p.uiPos.load();
        std::printf("  TAPE SCRUB (stopped): playhead %.3f s -> %.3f s, level while scrubbing %.1f dB over still\n", p0 / 48000.0,
                    p1 / 48000.0, db(rms(scrub.l), rms(quiet.l)));
        check(p1 > p0 + 0.5 * 48000.0 && rms(scrub.l) > 0.01, "TAPE SCRUB (mouse) jogs the tape and the heads play it");
    }
    {
        // SCRUB while playing: a speed offset (jog), back to normal when it stops
        r.p.send(lift::Cmd::Seek, 0, 0, 0.3 * 48000.0);
        r.panel->act("play");
        r.run(0.4);
        const auto base = r.run(0.5);
        const auto jog = r.turn(3, 150.f, 0.5);
        r.panel->act("stop");
        r.run(0.3);
        const double f0 = peakHz(spectrum(base.l)), f1 = peakHz(spectrum(jog.l));
        std::printf("  TAPE SCRUB (playing): pitch %.1f Hz -> %.1f Hz while turning\n", f0, f1);
        check(std::abs(f1 / f0 - 1.0) > 0.05, "TAPE SCRUB (mouse) while playing pushes the tape speed");
    }

    // ---- MIX: armed T1, the tape playing
    r.panel->act("mode:mix");
    const auto mixPass = [&](double secs) { return playFrom0(secs); };
    {
        const auto base = mixPass(1.0);
        r.turn(0, -100.f, 0.3);  // LEVEL down
        const auto lo = mixPass(1.0);
        std::printf("  MIX LEVEL -> %.2f: %+.1f dB\n", r.value(3, 0), db(rms(lo.l), rms(base.l)));
        check(db(rms(lo.l), rms(base.l)) < -6.0, "MIX LEVEL (mouse) turns the armed track down");
        r.turn(0, 100.f, 0.1);
    }
    {
        const auto base = mixPass(1.0);
        r.turn(1, -110.f, 0.3);  // PAN hard left
        const auto left = mixPass(1.0);
        std::printf("  MIX PAN -> %.2f: L/R %+.1f dB -> %+.1f dB\n", r.value(3, 1), db(rms(base.l), rms(base.r)),
                    db(rms(left.l), rms(left.r)));
        check(db(rms(left.l), rms(left.r)) > 12.0, "MIX PAN (mouse) moves the armed track left");
        r.turn(1, 110.f, 0.1);
    }
    for (int k = 2; k < 4; ++k) {
        const auto base = mixPass(1.0);
        const float v0 = r.value(3, k);
        r.turn(k, 110.f, 0.3);  // boost
        const auto up = mixPass(1.0);
        const auto sa = spectrum(base.l), sb = spectrum(up.l);
        std::printf("  MIX %s %.2f -> %.2f: level %+.1f dB, centroid %.0f -> %.0f Hz\n", k == 2 ? "LOW" : "HIGH", v0,
                    r.value(3, k), db(rms(up.l), rms(base.l)), centroid(sa), centroid(sb));
        check(specDist(sa, sb) > 0.2, juce::String("MIX ") + (k == 2 ? "LOW" : "HIGH") + " (mouse) changes the tone");
        r.turn(k, -110.f, 0.1);
    }
    {
        // zipper: sweep LEVEL fast over a steady tone; the sweep must be no
        // rougher than the steady tone itself
        r.p.send(lift::Cmd::Seek, 0, 0, 0.3 * 48000.0);
        r.panel->act("play");
        r.run(0.4);
        const auto steady = r.run(0.3);
        const auto sweep = r.turn(0, -150.f, 0.25);
        r.panel->act("stop");
        r.run(0.2);
        const double a = maxD2(steady.l, 0, steady.l.size()), b = maxD2(sweep.l, 0, sweep.l.size());
        std::printf("  MIX LEVEL fast sweep: max 2nd difference %.5f (steady %.5f)\n", b, a);
        check(b <= a * 1.05 + 1e-6, "MIX LEVEL sweep has no zipper steps");
        r.turn(0, 150.f, 0.1);
    }

    // ---- SYNTH: the four macros on a played note (attack, hold, release)
    r.panel->act("mode:synth");
    auto note = [&] {
        r.p.send(lift::Cmd::NoteOn, 57, 100);
        auto a = r.run(0.5);
        r.p.send(lift::Cmd::NoteOff, 57);
        auto b = r.run(0.8);
        a.l.insert(a.l.end(), b.l.begin(), b.l.end());
        return a;
    };
    const char* names[4] = {"DETUNE", "CUTOFF", "ENV AMT", "DECAY"};
    for (int k = 0; k < 4; ++k) {
        const auto base = note();
        const float v0 = r.value(0, k);
        const float dy = v0 < 0.5f ? 110.f : -110.f;
        r.turn(k, dy, 0.2);
        const auto moved = note();
        const auto sa = spectrum(base.l), sb = spectrum(moved.l);
        const size_t rel = static_cast<size_t>(0.6 * 48000.0);
        std::printf("  SYNTH %-7s %.2f -> %.2f: spectral change %.2f, centroid %.0f -> %.0f Hz, release %+.1f dB\n", names[k], v0,
                    r.value(0, k), specDist(sa, sb), centroid(sa), centroid(sb),
                    db(rms(moved.l, rel, moved.l.size()), rms(base.l, rel, base.l.size())));
        const bool changed = specDist(sa, sb) > 0.2 || std::abs(db(rms(moved.l, rel, moved.l.size()), rms(base.l, rel, base.l.size()))) > 3.0;
        check(changed, juce::String("SYNTH ") + names[k] + " (mouse) audibly changes the voice");
        r.turn(k, -dy, 0.1);
    }

    // ---- IN: GAIN on the input (the voice is the record source)
    r.panel->act("mode:in");
    {
        r.p.send(lift::Cmd::NoteOn, 57, 100);
        r.run(0.3);
        const auto base = r.run(0.6);
        r.turn(2, -132.f, 0.3);  // GAIN down to 0
        const auto lo = r.run(0.6);
        r.p.send(lift::Cmd::NoteOff, 57);
        r.run(0.5);
        std::printf("  IN GAIN -> %.2f: %+.1f dB\n", r.value(4, 2), db(rms(lo.l), rms(base.l)));
        check(db(rms(lo.l), rms(base.l)) < -20.0, "IN GAIN (mouse) sets the input level");
    }
}
