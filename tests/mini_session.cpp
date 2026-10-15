// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// lift_mini_session plays LIFT Mini from a scenario book, the way a person
// would: SHIFT, the ten buttons, 24 keys, four push encoders and the Pico's
// USB MIDI all go through mini::MiniUi::handle() (and mini::midiPanelEvent),
// exactly as the app wires them. Audio runs offline through the real
// LiftProcessor in 256-sample blocks. Time is fake, so SHIFT taps, double taps
// and toasts behave the same on every machine.
//
//   lift_mini_session                              (tests/mini_recipes/book.txt)
//   lift_mini_session BOOK --only "NAME" --list --out DIR
//   lift_mini_session --chaos-seeds 200            (extra random runs)
//
// The book format and how to extend it: tests/mini_recipes/README.md.

#include "MiniUi.h"
#include "LiftProcessor.h"
#include "tape/transport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using mini::Btn;
using mini::Event;

namespace {

constexpr int kBlock = 256;
constexpr double kRate = 48000.0;

#ifndef LIFT_SOURCE_DIR
#define LIFT_SOURCE_DIR "."
#endif

// ------------------------------------------------------------------ book

struct Step {
    int line = 0;
    std::vector<std::string> w;
};

struct Recipe {
    std::string name, known;
    int line = 0;
    std::vector<Step> steps;
};

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) {
        if (w[0] == '#') break;
        out.push_back(w);
    }
    return out;
}

bool readBook(const std::string& path, std::vector<Recipe>& book, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return false; }
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        const std::string t = trim(raw);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("recipe:", 0) == 0) { Recipe r; r.name = trim(t.substr(7)); r.line = line; book.push_back(r); continue; }
        if (book.empty()) { err = path + ":" + std::to_string(line) + ": a step before the first recipe:"; return false; }
        if (t.rfind("known:", 0) == 0) { book.back().known = trim(t.substr(6)); continue; }
        Step s;
        s.line = line;
        s.w = words(t);
        if (!s.w.empty()) book.back().steps.push_back(s);
    }
    return true;
}

// ------------------------------------------------------------------ names

const char* kModeNames[] = {"SYNTH", "DRUM", "TAPE", "MIX", "RADIO"};
const char* kSourceNames[] = {"INTERNAL", "RADIO", "COMPUTER", "MASTER"};
const char* kColours[] = {"YELLOW", "BLUE", "BLACK", "RED"};

int indexOf(const char* const* names, int n, const std::string& w) {
    const std::string u = upper(w);
    for (int i = 0; i < n; ++i) if (u == names[i]) return i;
    return -1;
}

// "yellow" / "1" -> 0
int encoderOf(const std::string& w) {
    const int c = indexOf(kColours, 4, w);
    if (c >= 0) return c;
    const int n = std::atoi(w.c_str());
    return n >= 1 && n <= 4 ? n - 1 : -1;
}

// Key spec -> chromatic index 0..23 (C3 = 0). "7" = 7th chromatic key,
// "w3" = 3rd white key (1..14), "b2" = 2nd black key (1..10), as the sheet counts them.
int keyOf(const std::string& w) {
    static const int white[7] = {0, 2, 4, 5, 7, 9, 11}, black[5] = {1, 3, 6, 8, 10};
    if (w.size() >= 2 && (w[0] == 'w' || w[0] == 'W')) {
        const int n = std::atoi(w.c_str() + 1) - 1;
        return n >= 0 && n < 14 ? 12 * (n / 7) + white[n % 7] : -1;
    }
    if (w.size() >= 2 && (w[0] == 'b' || w[0] == 'B')) {
        const int n = std::atoi(w.c_str() + 1) - 1;
        return n >= 0 && n < 10 ? 12 * (n / 5) + black[n % 5] : -1;
    }
    const int n = std::atoi(w.c_str()) - 1;
    return n >= 0 && n < 24 ? n : -1;
}

// ------------------------------------------------------------------ session

class Session {
public:
    Session(const Recipe& r, const fs::path& work, const fs::path& out) : r_(r), out_(out) {
        static int n = 0;   // each recipe gets its own slot folder: a fresh Mini every time
        slots_ = juce::File(work.string()).getChildFile("slots-" + std::to_string(++n));
        slots_.deleteRecursively();
        start();
    }

    // power on, as MiniApp does: reopen the last project, else a new one
    void start() {
        proc_ = std::make_unique<lift::LiftProcessor>();
        proc_->clearDrumPatterns();
        proc_->setSlotFolder(slots_);
        proc_->prepareToPlay(kRate, kBlock);
        ui_ = std::make_unique<mini::MiniUi>(*proc_, 30);
        ui_->setClock([this] { return clockMs_; });
        ui_->setSize(mini::kW, mini::kH);
        ui_->boot();
    }

    ~Session() {
        ui_.reset();
    }

    std::vector<std::string> run() {
        for (const Step& s : r_.steps) {
            line_ = s.line;
            if (!step(s.w)) break;
            if (failures_.size() > 30) { failures_.push_back("(stopping: too many failures)"); break; }
        }
        return failures_;
    }

    // ---- chaos (also used by --chaos-seeds) ----
    void chaos(uint32_t seed, int moves) {
        uint32_t s = seed * 2654435761u + 7u;
        auto rnd = [&](uint32_t n) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return n ? s % n : 0u; };
        input_ = Input::Noise;
        amp_ = 0.3f;
        for (int i = 0; i < moves && failures_.empty(); ++i) {
            switch (rnd(12)) {
                case 0: case 1: { const Btn b = Btn(rnd(uint32_t(Btn::Count))); press(b, true); if (rnd(4)) press(b, false); break; }
                case 2: case 3: { const int k = int(rnd(24)); keyDown(k, !held_.count(k)); break; }
                case 4: case 5: send({Event::Turn, int(rnd(4)), int(rnd(11)) - 5}); break;
                case 6: { const int e = int(rnd(4)); send({Event::Push, e, 1}); send({Event::Push, e, 0}); break; }
                case 7: press(Btn::Shift, true); advance(double(rnd(120))); press(Btn::Shift, false); break;
                case 8: midi(juce::MidiMessage::controllerEvent(16, 20 + int(rnd(4)), 64 + int(rnd(9)) - 4)); break;
                case 9: midi(juce::MidiMessage::noteOn(1, 48 + int(rnd(24)), juce::uint8(100))); break;
                default: break;
            }
            advance(double(rnd(250)));
            invariants();
            if (!failures_.empty()) failures_.back() += "  (chaos seed " + std::to_string(seed) + ", move " + std::to_string(i) + ")";
        }
        // let go of everything a person could still be holding
        for (int k : std::vector<int>(held_.begin(), held_.end())) keyDown(k, false);
        for (int b = 0; b < int(Btn::Count); ++b) if (heldBtn_[b]) press(Btn(b), false);
        midi(juce::MidiMessage::allNotesOff(1));
        paint();
    }

private:
    const Recipe& r_;
    fs::path out_;
    std::unique_ptr<lift::LiftProcessor> proc_;
    juce::File slots_;
    std::unique_ptr<mini::MiniUi> ui_;
    double clockMs_ = 1000.0;
    int line_ = 0;
    std::vector<std::string> failures_;
    std::set<int> held_;                 // keys held through the UI
    bool heldBtn_[int(Btn::Count)] = {};
    juce::MidiBuffer pendingMidi_;
    enum class Input { Silence, Sine, Noise } input_ = Input::Silence;
    float amp_ = 0.f;
    double freq_ = 440.0, phase_ = 0.0;
    uint32_t noise_ = 99u;
    std::vector<float> outL_, outR_;
    double worstMs_ = 0.0;
    double markPos_ = 0.0;
    std::map<std::string, std::vector<float>> snaps_;

    void fail(const std::string& what) { failures_.push_back("line " + std::to_string(line_) + ": " + what); }

    double num(const std::vector<std::string>& w, size_t i, double fallback = 0.0) {
        if (i >= w.size()) return fallback;
        char* end = nullptr;
        const double v = std::strtod(w[i].c_str(), &end);
        if (end == w[i].c_str()) { fail("expected a number, got '" + w[i] + "'"); return fallback; }
        return v;
    }

    // ---- input to the processor ----
    void inputSample(float& l, float& r) {
        switch (input_) {
            case Input::Silence: l = r = 0.f; return;
            case Input::Sine:
                l = r = amp_ * float(std::sin(phase_));
                phase_ += 2.0 * juce::MathConstants<double>::pi * freq_ / kRate;
                if (phase_ > 2.0 * juce::MathConstants<double>::pi) phase_ -= 2.0 * juce::MathConstants<double>::pi;
                return;
            case Input::Noise:
                noise_ = noise_ * 1664525u + 1013904223u; l = amp_ * (float(noise_ >> 8) / 8388608.f - 1.f);
                noise_ = noise_ * 1664525u + 1013904223u; r = amp_ * (float(noise_ >> 8) / 8388608.f - 1.f);
                return;
        }
    }

    // ---- time: audio runs as the clock moves ----
    void advance(double ms) {
        long frames = long(std::llround(ms * kRate / 1000.0));
        juce::AudioBuffer<float> buf(2, kBlock);
        while (frames > 0) {
            const int n = int(std::min<long>(kBlock, frames));
            buf.setSize(2, n, false, false, true);
            for (int i = 0; i < n; ++i) { float l, r; inputSample(l, r); buf.setSample(0, i, l); buf.setSample(1, i, r); }
            proc_->flushBacklog();          // the processor's 30 Hz timer, which no message loop drives here
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc_->processBlock(buf, pendingMidi_);
            worstMs_ = std::max(worstMs_, juce::Time::getMillisecondCounterHiRes() - t0);
            pendingMidi_.clear();
            proc_->drainMidiEvents();
            for (int i = 0; i < n; ++i) {
                const float l = buf.getSample(0, i), r = buf.getSample(1, i);
                if (!std::isfinite(l) || !std::isfinite(r)) { fail("output went NaN or inf"); return; }
                outL_.push_back(l);
                outR_.push_back(r);
            }
            clockMs_ += 1000.0 * n / kRate;
            frames -= n;
        }
    }

    // ---- controls, through the same paths as the app ----
    void send(const Event& e) { ui_->handle(e); }
    void press(Btn b, bool down) { heldBtn_[int(b)] = down; send({Event::Button, int(b), down ? 1 : 0}); }
    void keyDown(int k, bool down) {
        if (down) held_.insert(k); else held_.erase(k);
        send({Event::Key, k, down ? 1 : 0});
    }
    void midi(const juce::MidiMessage& m) {
        // MiniApp's MidiSplit: panel messages go to the UI, the rest to the engine
        if (!mini::midiPanelEvent(m, [this](const Event& e) { send(e); })) pendingMidi_.addEvent(m, 0);
    }
    void paint() {
        juce::Image img(juce::Image::RGB, mini::kW, mini::kH, true);
        juce::Graphics g(img);
        ui_->paintEntireComponent(g, true);
    }

    // after every chaos move
    void invariants() {
        const auto v = ui_->view();
        if (v.page < 0 || v.page > 3) fail("page out of range: " + std::to_string(v.page));
        if (v.octave < -3 || v.octave > 3) fail("octave out of range: " + std::to_string(v.octave));
        if (v.track < 0 || v.track > 3) fail("track out of range: " + std::to_string(v.track));
        const double pos = proc_->uiPos.load();
        if (pos < 0.0 || pos > double(proc_->runtime().frames)) fail("playhead left the tape: " + std::to_string(pos));
        if (!std::isfinite(proc_->signedSpeed.load())) fail("speed went NaN");
    }

    // ---- metrics ----
    static float rmsOf(const std::vector<float>& x) {
        if (x.empty()) return 0.f;
        double s = 0; for (float v : x) s += double(v) * v;
        return float(std::sqrt(s / double(x.size())));
    }
    static float peakOf(const std::vector<float>& x) {
        float p = 0; for (float v : x) p = std::max(p, std::fabs(v));
        return p;
    }
    static float clicksOf(const std::vector<float>& x) {
        float m = 0; for (size_t i = 1; i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
        return m;
    }
    std::vector<float> snapshot() {
        auto& rt = proc_->runtime();
        std::vector<float> s;
        for (int t = 0; t < 4; ++t) for (int c = 0; c < 2; ++c) s.insert(s.end(), rt.ch[t][c], rt.ch[t][c] + rt.frames);
        return s;
    }

    // Returns false (and records why) if the metric is unknown.
    bool value(const std::vector<std::string>& m, double& have) {
        const auto v = ui_->view();
        auto& rt = proc_->runtime();
        const std::string k = m.empty() ? "" : m[0];
        if (k == "mode") have = v.mode;
        else if (k == "page") have = v.page + 1;
        else if (k == "octave") have = v.octave;
        else if (k == "track") have = v.track + 1;
        else if (k == "source") have = v.source;
        else if (k == "source_sel") have = v.sourceSel;
        else if (k == "sources_open") have = v.sourcesOpen;
        else if (k == "shift") have = v.shift;
        else if (k == "shift_latched") have = v.shiftLatched;
        else if (k == "clip") have = v.clip ? v.clip->frames() / kRate : 0.0;
        else if (k == "clip_peak" || k == "clip_rms") {
            if (!v.clip) { have = 0; return true; }
            std::vector<float> both(v.clip->l.begin(), v.clip->l.end());
            both.insert(both.end(), v.clip->r.begin(), v.clip->r.end());
            for (float x : both) if (!std::isfinite(x)) { have = 1e9; return true; }
            have = k == "clip_peak" ? peakOf(both) : rmsOf(both);
        }
        else if (k == "mute" && m.size() > 1) { const int t = int(num(m, 1)) - 1; have = t >= 0 && t < 4 ? v.mute[t] : -1; }
        else if (k == "knob" && m.size() > 1) { const int e = encoderOf(m[1]); if (e < 0) { fail("knob: yellow, blue, black or red"); return false; } have = v.knob[e]; }
        else if (k == "sknob" && m.size() > 1) { const int e = encoderOf(m[1]); if (e < 0) { fail("sknob: yellow, blue, black or red"); return false; } have = v.knobS[e]; }
        else if (k == "tempo") have = proc_->tempoBpm.load();
        else if (k == "engine") have = proc_->synthEngine.load();
        else if (k == "pages") have = v.pages;
        else if (k == "passes") have = proc_->uiPassCount.load();
        else if (k == "loop") have = (proc_->uiLoopEnd.load() - proc_->uiLoopStart.load()) / kRate;
        else if (k == "character") have = rt.character;
        else if (k == "fader" && m.size() > 1) have = rt.fader[std::clamp(std::atoi(m[1].c_str()) - 1, 0, 3)];
        else if (k == "pan" && m.size() > 1) { const int t = std::clamp(std::atoi(m[1].c_str()) - 1, 0, 3); have = rt.panR[t] - rt.panL[t]; }
        else if (k == "speed") have = proc_->signedSpeed.load();
        else if (k == "playing") have = proc_->uiPlaying.load();
        else if (k == "recording") have = proc_->uiRecording.load();
        else if (k == "arm") have = rt.arm + 1;
        else if (k == "pos") have = proc_->uiPos.load() / kRate;
        else if (k == "moved") have = (proc_->uiPos.load() - markPos_) / kRate;
        else if (k == "note") have = proc_->uiSynthNote.load();
        else if (k == "voices") { int n = 0; for (auto& e : proc_->uiVoiceEnv) n += e.load() > 1e-3f; have = n; }
        else if (k == "drums") { int n = 0; for (auto& e : proc_->uiDrumEnv) n += e.load() > 1e-3f; have = n; }
        else if (k == "out" && m.size() > 1) {
            if (m[1] == "rms") have = std::max(rmsOf(outL_), rmsOf(outR_));
            else if (m[1] == "peak") have = std::max(peakOf(outL_), peakOf(outR_));
            else if (m[1] == "clicks") have = std::max(clicksOf(outL_), clicksOf(outR_));
            else { fail("out: rms, peak or clicks"); return false; }
        }
        else if (k == "track_rms" && m.size() > 1) {
            const int t = int(num(m, 1)) - 1;
            if (t < 0 || t > 3) { fail("track_rms: track 1 to 4"); return false; }
            int a = 0, b = rt.frames;
            if (m.size() > 3) { a = std::clamp(int(num(m, 2) * kRate), 0, rt.frames); b = std::clamp(int(num(m, 3) * kRate), 0, rt.frames); }
            std::vector<float> x(rt.ch[t][0] + a, rt.ch[t][0] + b);
            have = rmsOf(x);
        }
        else if (k == "overflows") have = proc_->queueOverflows.load();
        else if (k == "job_busy") have = proc_->jobBusy();
        else if (k == "pool") have = double(proc_->pool().size());
        else if (k == "slice_kit") have = proc_->sliceKitLoaded();
        else if (k == "block_ms") have = worstMs_;
        else if (k == "changed" && m.size() > 1) {
            auto it = snaps_.find(m[1]);
            if (it == snaps_.end()) { fail("no snapshot called " + m[1]); return false; }
            const auto now = snapshot();
            double d = 0;
            for (size_t i = 0; i < now.size(); ++i) d = std::max(d, double(std::fabs(now[i] - it->second[i])));
            have = d;
        }
        else { fail("unknown metric '" + k + "'"); return false; }
        return true;
    }

    // expect METRIC... OP VALUE   |   expect toast has|lacks WORDS...
    void expect(const std::vector<std::string>& w) {
        if (w.size() >= 3 && (w[1] == "toast" || w[1] == "page_label") && (w[2] == "has" || w[2] == "lacks")) {
            std::string want;
            for (size_t i = 3; i < w.size(); ++i) want += (want.empty() ? "" : " ") + w[i];
            const auto vw = ui_->view();
            const std::string toast = (w[1] == "toast" ? vw.toast : vw.pageLabel).toStdString();
            auto squash = [](std::string x) {   // case and runs of spaces don't matter
                std::string o;
                for (char ch : upper(x)) if (!(ch == ' ' && !o.empty() && o.back() == ' ')) o += ch;
                return o;
            };
            const bool has = squash(toast).find(squash(want)) != std::string::npos;
            if (has != (w[2] == "has")) fail("expected " + w[1] + " " + w[2] + " \"" + want + "\", toast is \"" + toast + "\"");
            return;
        }
        if (w.size() < 4) { fail("expect needs a metric, an operator and a value"); return; }
        const std::string op = w[w.size() - 2], vs = w.back();
        const std::vector<std::string> m(w.begin() + 1, w.end() - 2);
        double want;
        if (m[0] == "mode" && indexOf(kModeNames, 5, vs) >= 0) want = indexOf(kModeNames, 5, vs);
        else if ((m[0] == "source" || m[0] == "source_sel") && indexOf(kSourceNames, 4, vs) >= 0) want = indexOf(kSourceNames, 4, vs);
        else want = num(w, w.size() - 1);
        double have = 0;
        if (!value(m, have)) return;
        bool ok;
        if (op == "<") ok = have < want;
        else if (op == "<=") ok = have <= want;
        else if (op == ">") ok = have > want;
        else if (op == ">=") ok = have >= want;
        else if (op == "==") ok = std::fabs(have - want) < 1e-6;
        else if (op == "!=") ok = std::fabs(have - want) >= 1e-6;
        else { fail("unknown operator '" + op + "'"); return; }
        if (!ok) {
            std::string metric;
            for (auto& x : m) metric += (metric.empty() ? "" : " ") + x;
            std::string shown = std::to_string(have);
            if (m[0] == "mode" && have >= 0 && have < 5) shown = kModeNames[int(have)];
            if ((m[0] == "source" || m[0] == "source_sel") && have >= 0 && have < 4) shown = kSourceNames[int(have)];
            fail("expected " + metric + " " + op + " " + vs + ", got " + shown);
        }
    }

    // ---- steps ----
    bool step(const std::vector<std::string>& w) {
        const std::string& c = w[0];
        auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
        if (c == "button") {
            const Btn b = mini::btnFromName(juce::String(upper(arg(1))));
            if (b == Btn::Count) { fail("no button called '" + arg(1) + "' (SYNTH DRUM TAPE MIX OCT- OCT+ LIFT DROP REC PLAY SHIFT)"); return false; }
            const std::string how = arg(2).empty() ? "tap" : arg(2);
            if (how == "down") press(b, true);
            else if (how == "up") press(b, false);
            else if (how == "tap") { press(b, true); advance(40); press(b, false); advance(40); }
            else { fail("button NAME down|up|tap"); return false; }
        } else if (c == "key") {
            const int k = keyOf(arg(1));
            if (k < 0) { fail("key: 1-24, w1-w14 (white) or b1-b10 (black)"); return false; }
            const std::string how = arg(2).empty() ? "tap" : arg(2);
            if (how == "down") keyDown(k, true);
            else if (how == "up") keyDown(k, false);
            else if (how == "tap") { keyDown(k, true); advance(150); keyDown(k, false); }
            else { fail("key SPEC down|up|tap"); return false; }
        } else if (c == "turn") {
            const int e = encoderOf(arg(1));
            if (e < 0) { fail("turn yellow|blue|black|red DETENTS"); return false; }
            const int d = int(num(w, 2, 1));
            // "turn red 10 each": ten single detents, 10 ms apart (a quick hand);
            // "turn red 10 each 200": 200 ms apart (a slow hand)
            const bool one = arg(3) == "each";
            const double gap = arg(4).empty() ? 10.0 : num(w, 4, 10);
            if (one) for (int i = 0; i < std::abs(d); ++i) { send({Event::Turn, e, d > 0 ? 1 : -1}); advance(gap); }
            else send({Event::Turn, e, d});
        } else if (c == "restart") {
            // quit (the app saves the project) and power on again
            for (int k : std::vector<int>(held_.begin(), held_.end())) keyDown(k, false);
            ui_->saveNow();
            ui_.reset();
            proc_.reset();
            start();
            advance(20);
        } else if (c == "push") {
            const int e = encoderOf(arg(1));
            if (e < 0) { fail("push yellow|blue|black|red [down|up]"); return false; }
            const std::string how = arg(2).empty() ? "tap" : arg(2);
            if (how != "up") send({Event::Push, e, 1});
            if (how == "tap") advance(40);
            if (how != "down") send({Event::Push, e, 0});
        } else if (c == "midi") {
            // midi CH noteon N [VEL] | noteoff N | cc N V
            const int ch = int(num(w, 1, 1));
            const std::string kind = arg(2);
            if (kind == "noteon") midi(juce::MidiMessage::noteOn(ch, int(num(w, 3)), juce::uint8(num(w, 4, 100))));
            else if (kind == "noteoff") midi(juce::MidiMessage::noteOff(ch, int(num(w, 3))));
            else if (kind == "cc") midi(juce::MidiMessage::controllerEvent(ch, int(num(w, 3)), int(num(w, 4))));
            else { fail("midi CH noteon|noteoff|cc ..."); return false; }
        } else if (c == "wait") {
            advance(num(w, 1));
        } else if (c == "run") {
            advance(1000.0 * num(w, 1));
        } else if (c == "mark") {
            outL_.clear(); outR_.clear(); worstMs_ = 0; markPos_ = proc_->uiPos.load();
        } else if (c == "input") {
            const std::string kind = arg(1);
            if (kind == "silence") input_ = Input::Silence;
            else if (kind == "sine") { input_ = Input::Sine; freq_ = num(w, 2, 440); amp_ = float(num(w, 3, 0.5)); }
            else if (kind == "noise") { input_ = Input::Noise; amp_ = float(num(w, 2, 0.3)); }
            else { fail("input silence | sine HZ AMP | noise AMP"); return false; }
        } else if (c == "fill") {
            // fill track N sine HZ AMP: write straight onto the tape (setup only)
            auto& rt = proc_->runtime();
            const int t = int(num(w, 2)) - 1;
            if (t < 0 || t > 3) { fail("fill track 1-4 sine HZ AMP"); return false; }
            const double f = num(w, 4, 220), a = num(w, 5, 0.3);
            for (int i = 0; i < rt.frames; ++i) rt.ch[t][0][i] = rt.ch[t][1][i] = float(a * std::sin(2.0 * juce::MathConstants<double>::pi * f * i / kRate));
        } else if (c == "snapshot") {
            snaps_[arg(1).empty() ? "a" : arg(1)] = snapshot();
        } else if (c == "shot") {
            fs::create_directories(out_);
            juce::Image img(juce::Image::RGB, mini::kW, mini::kH, true);
            { juce::Graphics g(img); ui_->paintEntireComponent(g, true); }
            const juce::File f(fs::path(out_ / ((arg(1).empty() ? "mini" : arg(1)) + ".png")).string());
            f.deleteFile();
            juce::FileOutputStream os(f);
            juce::PNGImageFormat png;
            if (!os.openedOk() || !png.writeImageToStream(img, os)) fail("could not write " + f.getFullPathName().toStdString());
        } else if (c == "paint") {
            paint();
        } else if (c == "chaos") {
            chaos(uint32_t(num(w, 1, 1)), int(num(w, 2, 300)));
        } else if (c == "expect") {
            expect(w);
        } else {
            fail("unknown step '" + c + "'");
            return false;
        }
        return true;
    }
};

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    // no network radio, no system capture in tests (the processor reads this)
    setenv("LIFT_RADIO_OFF", "1", 1);
    std::string book = std::string(LIFT_SOURCE_DIR) + "/tests/mini_recipes/book.txt", only;
    fs::path out = fs::current_path() / "mini-recipes-out";
    bool list = false;
    int chaosSeeds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--list") list = true;
        else if (a == "--chaos-seeds" && i + 1 < argc) chaosSeeds = std::atoi(argv[++i]);
        else book = a;
    }
    std::vector<Recipe> recipes;
    std::string err;
    if (!readBook(book, recipes, err)) { std::cerr << err << "\n"; return 2; }
    if (chaosSeeds > 0) {
        for (int s = 1; s <= chaosSeeds; ++s) {
            Recipe r; r.name = "chaos seed " + std::to_string(s);
            r.steps.push_back({0, {"chaos", std::to_string(1000 + s), "300"}});
            recipes.push_back(r);
        }
    }
    if (list) {
        for (auto& r : recipes) std::cout << r.name << (r.known.empty() ? "" : "  [known]") << "\n";
        return 0;
    }
    const fs::path work = fs::temp_directory_path() / ("lift-mini-session-" + std::to_string(juce::Time::getHighResolutionTicks()));
    int passed = 0, failed = 0, known = 0, fixed = 0;
    for (const Recipe& r : recipes) {
        if (!only.empty() && r.name != only) continue;
        fs::remove_all(work);
        fs::create_directories(work);
        std::vector<std::string> f;
        {
            Session s(r, work, out);
            f = s.run();
        }
        if (r.known.empty()) {
            if (f.empty()) { ++passed; std::cout << "PASS   " << r.name << "\n"; }
            else { ++failed; std::cout << "FAIL   " << r.name << "\n"; for (auto& x : f) std::cout << "         " << x << "\n"; }
        } else if (f.empty()) {
            ++fixed; std::cout << "FIXED  " << r.name << "  (remove its known: line)\n";
        } else {
            ++known; std::cout << "KNOWN  " << r.name << "  (" << r.known << ")\n";
            for (auto& x : f) std::cout << "         " << x << "\n";
        }
    }
    fs::remove_all(work);
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << known << " known, " << fixed << " fixed\n";
    if (passed + failed + known + fixed == 0) { std::cerr << "no recipe matched\n"; return 2; }
    return failed == 0 && fixed == 0 ? 0 : 1;
}
