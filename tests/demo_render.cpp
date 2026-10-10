// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Demo renders through the plugin processor (LIFT's own tape, the patch bay,
// SHOGUN drums, the synth engines, the spring): `lift_panel_check --demo DIR`.
// Also the real-tape feedback check (HEAD 2 -> SPEED, no NaN, bounded).

#include "LiftProcessor.h"
#include "tape/transport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

struct Note {
    double t, len;
    int note, vel;
};

struct Take {
    std::vector<float> l, r;
};

struct Demo {
    std::unique_ptr<lift::LiftProcessor> p = std::make_unique<lift::LiftProcessor>();
    Demo() { p->prepareToPlay(48000.0, 512); }
    void state(const std::function<void(lift::UiState&)>& f) {
        lift::UiState s = p->uiState();
        f(s);
        p->setUiState(s);
        p->patch.publish(s.cords);
    }
    // secs of audio with timed notes (MIDI, channel 1)
    Take run(double secs, const std::vector<Note>& notes = {}) {
        Take a;
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        const int blocks = static_cast<int>(secs * 48000.0 / 512.0);
        for (int b = 0; b < blocks; ++b) {
            buf.clear();
            midi.clear();
            const double t0 = b * 512.0 / 48000.0, t1 = (b + 1) * 512.0 / 48000.0;
            for (const Note& n : notes) {
                if (n.t >= t0 && n.t < t1) {
                    midi.addEvent(juce::MidiMessage::noteOn(1, n.note, static_cast<juce::uint8>(n.vel)),
                                  static_cast<int>((n.t - t0) * 48000.0));
                }
                const double off = n.t + n.len;
                if (off >= t0 && off < t1) {
                    midi.addEvent(juce::MidiMessage::noteOff(1, n.note), static_cast<int>((off - t0) * 48000.0));
                }
            }
            p->processBlock(buf, midi);
            a.l.insert(a.l.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
            a.r.insert(a.r.end(), buf.getReadPointer(1), buf.getReadPointer(1) + 512);
        }
        return a;
    }
};

bool writeWav(const juce::File& f, const Take& t) {
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000.0, 2, 24, {}, 0));
    if (w == nullptr) return false;
    os.release();
    const float* ch[2] = {t.l.data(), t.r.data()};
    return w->writeFromFloatArrays(ch, 2, static_cast<int>(t.l.size()));
}

double peak(const Take& t) {
    double p = 0.0;
    for (size_t i = 0; i < t.l.size(); ++i) p = std::max(p, static_cast<double>(std::max(std::abs(t.l[i]), std::abs(t.r[i]))));
    return p;
}
bool finite(const Take& t) {
    for (size_t i = 0; i < t.l.size(); ++i)
        if (!std::isfinite(t.l[i]) || !std::isfinite(t.r[i])) return false;
    return true;
}

// a minor-key line in 16ths, one 8 s tape loop long
std::vector<Note> line(double bpm, int root) {
    const double q = 60.0 / bpm;
    const int seq[16] = {0, 12, 7, 10, 0, 15, 14, 10, 5, 12, 8, 7, 3, 7, 10, 12};
    std::vector<Note> v;
    for (int bar = 0; bar < 4; ++bar) {
        for (int k = 0; k < 16; ++k) {
            const double t = (bar * 16 + k) * q / 4.0;  // sixteenths
            if (t > 7.8) break;
            v.push_back({0.05 + t, q * 0.2, root + seq[k] + (bar == 3 ? -2 : 0), 96 + (k % 4 == 0 ? 20 : 0)});
        }
    }
    return v;
}

}  // namespace

int runDemoRenders(const juce::String& dirName, const std::function<void(bool, const juce::String&)>& check) {
    const juce::File dir(dirName);
    dir.createDirectory();
    std::printf("-- demo renders to %s\n", dir.getFullPathName().toRawUTF8());
    auto save = [&](const char* name, const Take& t) {
        const bool ok = finite(t) && peak(t) > 0.05 && writeWav(dir.getChildFile(name), t);
        std::printf("  %s: %.1f s, peak %.2f\n", name, t.l.size() / 48000.0, peak(t));
        check(ok, juce::String("demo ") + name + " rendered (finite, audible)");
    };
    // 1-2: the kits on their own patterns, through the tape's monitor electronics, a touch of spring
    for (int kit = 0; kit < 2; ++kit) {
        Demo d;
        d.p->tempoBpm.store(kit == 0 ? 96.0 : 126.0);
        d.state([&](lift::UiState& s) {
            s.sel[1] = kit;
            s.fx = true;
            s.fxType = lift::eng::FX_SPRING;
            s.fxKnobs[0] = {0.5f, 0.35f, 0.5f, 0.18f};
        });
        d.p->send(lift::Cmd::Transport, 1, 0);
        save(kit == 0 ? "808-beat.wav" : "909-beat.wav", d.run(10.0));
    }
    // 3: a synth line (LOOM) recorded onto T1, then played back off tape with the 808
    Demo d;
    d.p->tempoBpm.store(110.0);
    d.state([&](lift::UiState& s) {
        for (auto& k : s.drumPat) k.fill(0u);  // record the synth alone
        s.sel[0] = 0;
        s.enc[0] = {0.4f, 0.45f, 0.78f, 0.45f};
        s.enc[2][2] = 0.85f;  // REC LVL hot: tape saturation
        s.fx = true;
        s.fxType = lift::eng::FX_SPRING;
        s.fxKnobs[0] = {0.5f, 0.45f, 0.45f, 0.25f};
    });
    d.p->send(lift::Cmd::Transport, 1, 1);  // REC + PLAY
    d.run(8.0, line(110.0, 45));
    d.p->send(lift::Cmd::HardStop);
    d.run(0.2);
    d.p->send(lift::Cmd::Seek, 0, 0, 0.0);
    d.run(0.05);
    d.p->send(lift::Cmd::Transport, 1, 0);
    save("synth-line-through-tape.wav", d.run(16.0));
    // 4: self-patched tape wobble: LFO -> SPEED on the recorded line
    d.p->send(lift::Cmd::HardStop);
    d.run(0.1);
    d.state([](lift::UiState& s) { s.cords = {{lift::eng::O_LFO, lift::eng::I_SPEED, 2, false}}; });
    d.p->send(lift::Cmd::Seek, 0, 0, 0.0);
    d.run(0.05);
    d.p->send(lift::Cmd::Transport, 1, 0);
    save("tape-speed-wobble-lfo-to-speed.wav", d.run(16.0));
    // 5: feedback: HEAD 2 -> SPEED, HEAD 1 -> FX MAC, with the 808 back on
    d.p->send(lift::Cmd::HardStop);
    d.run(0.1);
    d.state([](lift::UiState& s) {
        s.cords = {{lift::eng::O_HEAD2, lift::eng::I_SPEED, 1, false}, {lift::eng::O_HEAD1, lift::eng::I_FXMAC, 3, false}};
        for (int k = 0; k < lift::eng::kKits; ++k)
            for (int v = 0; v < lift::eng::kDrumVoices; ++v)
                s.drumPat[static_cast<size_t>(k)][static_cast<size_t>(v)] = lift::eng::kitInfo(k).pattern[v];
    });
    d.p->send(lift::Cmd::Seek, 0, 0, 0.0);
    d.run(0.05);
    d.p->send(lift::Cmd::Transport, 1, 0);
    const Take fb = d.run(30.0);
    {
        Take last;
        last.l.assign(fb.l.end() - 48000 * 5, fb.l.end());
        last.r.assign(fb.r.end() - 48000 * 5, fb.r.end());
        std::printf("  real tape HEAD 2 -> SPEED loop: 30 s, peak %.2f, last 5 s peak %.2f\n", peak(fb), peak(last));
        check(finite(fb) && peak(fb) < 4.0, "real tape: HEAD 2 -> SPEED feedback stays finite and bounded for 30 s");
    }
    Take fb12;
    fb12.l.assign(fb.l.begin(), fb.l.begin() + 48000 * 14);
    fb12.r.assign(fb.r.begin(), fb.r.begin() + 48000 * 14);
    save("feedback-head2-to-speed.wav", fb12);
    return 0;
}
