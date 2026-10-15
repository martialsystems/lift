// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// LIFT Mini screen: the 4.3 in 480 x 272 display, drawn at that logical size
// and scaled to whatever the window is (800 x 480 on the Waveshare DSI panel).
// Plain 2D drawing, a capped frame rate, no OpenGL.
#pragma once

#include "LiftProcessor.h"
#include "MiniControls.h"
#include "MiniLayout.h"
#include "ShiftState.h"
#include "MiniParams.h"

#include <functional>

namespace mini {

inline constexpr int kW = kScreenW, kH = kScreenH;

class MiniUi : public juce::Component, private juce::Timer {
public:
    MiniUi(lift::LiftProcessor& p, int fps);
    ~MiniUi() override;
    void paint(juce::Graphics&) override;
    void handle(const Event& e);              // every control lands here
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isDown) override;
    void setStatus(const juce::String& s) { status_ = s; }

    enum Mode { Synth, Drum, Tape, Mix, Radio };

    // ---- test access (tests/mini_session.cpp) ----
    // A test clock in ms; unset = the real clock. SHIFT timing and toasts use it.
    void setClock(std::function<double()> c) { clock_ = std::move(c); }
    struct View {
        Mode mode;
        int page, octave, track, source, sourceSel;
        bool sourcesOpen, shift, shiftLatched;
        bool mute[4];
        float knob[4];            // the four knobs of the page on show, plain side (0..1)
        float knobS[4];           // the same knobs' SHIFT side
        juce::String toast;       // the message strip text right now ("" = none)
        lift::LiftProcessor::ClipPtr clip;
    };
    View view() const;

private:
    void timerCallback() override;
    void button(Btn b, bool down);
    void key(int k, bool down);
    void releaseHeldNotes();
    void shiftKey(int k);
    void turn(int enc, int detents);
    void push(int enc, bool down);
    void paintTape(juce::Graphics&, juce::Rectangle<float>);
    void paintSynth(juce::Graphics&, juce::Rectangle<float>);
    void paintDrum(juce::Graphics&, juce::Rectangle<float>);
    void paintMix(juce::Graphics&, juce::Rectangle<float>);
    void paintFoot(juce::Graphics&, juce::Rectangle<float>);
    void paintRadio(juce::Graphics&, juce::Rectangle<float>);
    void paintSources(juce::Graphics&, juce::Rectangle<float>);
    void say(juce::String s);
    double now() const { return clock_ ? clock_() : juce::Time::getMillisecondCounterHiRes(); }
    std::function<double()> clock_;

    lift::LiftProcessor& proc_;
    Mode mode_ = Tape;
    int page_ = 0;
    int octave_ = 0;
    int track_ = 0;
    ShiftState shift_;
    bool sh() const { return shift_.active(); }
    bool sourcesOpen_ = false;   // SHIFT + REC: the source list
    int sourceSel_ = 0;          // 0 INTERNAL, 1 RADIO, 2 COMPUTER, 3 MASTER
    int source_ = 0;
    float radioKnob_[4] = {0.f, 0.f, 0.5f, 0.f};  // TUNE, PRESET, LEVEL, MONO
    bool mute_[4] = {};
    float val_[4][kMaxPages][2][4] = {};       // [mode][page][0 plain, 1 SHIFT][knob]
    double lastTurn_[4] = {-1e9, -1e9, -1e9, -1e9};
    int pg() const { return mode_ == Radio ? 0 : juce::jmin(page_, kPageCount[mode_] - 1); }  // the real page
    float& val(int side, int k) { return val_[mode_][pg()][side][k]; }
    void apply(int mode, int page, int side, int k);
    juce::String valueText(int side, int k) const;
    void setLoopBars(int bars);
    lift::LiftProcessor::ClipPtr clip_;
    juce::String status_, flash_;
    double flashUntil_ = 0;
    int sounding_[24] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                         -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // note each held key started (-1 = none)
    juce::Array<int> heldKeys_;               // computer keys down (for key-up)
    float scope_[480] = {};
    std::vector<float> scopeBuf_ = std::vector<float>(4096);
};

}  // namespace mini
