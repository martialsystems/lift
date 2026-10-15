// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
//
// LIFT Mini screen: the 4.3 in 480 x 272 display, drawn at that logical size
// and scaled to whatever the window is (800 x 480 on the Waveshare DSI panel).
// Plain 2D drawing, a capped frame rate, no OpenGL.
#pragma once

#include "LiftProcessor.h"
#include "MiniControls.h"

namespace mini {

constexpr int kW = 480, kH = 272;

class MiniUi : public juce::Component, private juce::Timer {
public:
    MiniUi(lift::LiftProcessor& p, int fps);
    ~MiniUi() override;
    void paint(juce::Graphics&) override;
    void handle(const Event& e);              // every control lands here
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isDown) override;
    void setStatus(const juce::String& s) { status_ = s; }

    enum Mode { Synth, Drum, Tape, Mix };

private:
    void timerCallback() override;
    void button(Btn b, bool down);
    void key(int k, bool down);
    void turn(int enc, int detents);
    void push(int enc, bool down);
    int screenIndex() const;                  // LiftProcessor knob screen for the mode
    void paintTape(juce::Graphics&, juce::Rectangle<float>);
    void paintSynth(juce::Graphics&, juce::Rectangle<float>);
    void paintDrum(juce::Graphics&, juce::Rectangle<float>);
    void paintMix(juce::Graphics&, juce::Rectangle<float>);
    void paintFoot(juce::Graphics&, juce::Rectangle<float>);

    lift::LiftProcessor& proc_;
    Mode mode_ = Tape;
    int page_ = 0;
    int octave_ = 0;
    int track_ = 0;
    bool shift_ = false;
    bool mute_[4] = {};
    float knob_[5][4] = {};
    lift::LiftProcessor::ClipPtr clip_;
    juce::String status_, flash_;
    double flashUntil_ = 0;
    juce::Array<int> heldKeys_;               // computer keys down (for key-up)
    float scope_[480] = {};
    std::vector<float> scopeBuf_ = std::vector<float>(4096);
};

}  // namespace mini
