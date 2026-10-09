// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "LiftProcessor.h"
#include "ScreenAnim.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

namespace lift {

// The LIFT front panel, drawn natively 1:1 from the HTML prototype
// (LIFT.html). Everything is laid out in the prototype's CSS pixels: the
// component's logical size is the prototype page at scale 1 (1432 x 996:
// the control bar and info line, then the 1400 x 920 stage). LiftEditor
// scales it to the window with a transform.
class LiftPanel : public juce::Component, private juce::Timer {
public:
    static constexpr int kW = 1432;
    static constexpr int kH = 996;

    explicit LiftPanel(LiftProcessor& p);
    ~LiftPanel() override;

    enum Mode { Synth = 0, Drum, Tape, Mix, In };

    // Panel actions. Mouse, keyboard and tests all go through these.
    void act(const juce::String& a);
    void noteOn(int n);
    void noteOff(int n = -1);
    void setEnc(int i, float v);
    float enc(int mode, int i) const { return enc_[static_cast<size_t>(mode)][static_cast<size_t>(i)]; }
    Mode mode() const { return mode_; }
    const std::vector<Cord>& cords() const { return cords_; }
    juce::String infoText() const { return info_; }
    void tick() { advance(1.0 / 60.0); }
    // Steps the screen animation clock. The 60 Hz timer calls it with wall
    // time; tests call it with a fixed step to render frame sequences.
    void advance(double dt);
    void setReducedMotion(bool r) { reducedMotion_ = r; }
    double animTime() const { return t_; }
    // Screen area in component coordinates (what the animation repaints).
    static juce::Rectangle<int> screenArea();

    // Shift layer (see panel/SHIFT.md). Tap SHIFT to latch, press and hold it
    // (or hold the computer's Shift key) for momentary shift.
    bool shiftActive() const { return shiftLatch_ || shiftMouse_ || shiftKey_; }
    void setShiftKey(bool down);              // the computer Shift key
    void pressMem(int slot) { memAct(slot); } // a click on a keypad slot
    void pressKey(int note);                  // a key press: plays, or runs its shifted function
    juce::String shiftLabel(int group, int kind, int idx, bool& real) const;
    int shiftGroup() const;                   // 0 transport (TAPE, MIX, IN, BAY), 1 SYNTH, 2 DRUM
    int loopIn() const { return loopIn_; }
    int loopOut() const { return loopOut_; }

    void paint(juce::Graphics& g) override;
    void modifierKeysChanged(const juce::ModifierKeys& mods) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed(const juce::KeyPress& k) override;
    bool keyStateChanged(bool isKeyDown) override;

private:
    struct JackId {
        char r = 0;  // 'o' or 'i', 0 = none
        int i = -1;
        bool valid() const { return r != 0; }
        bool operator==(const JackId& o) const { return r == o.r && i == o.i; }
    };
    struct Drag {
        bool active = false;
        bool isMove = false;
        int n = -1;
        int c = 0;
        bool st = false;
        JackId fixed;
        int flv = 0;
        char need = 0;
        JackId from;
        juce::Point<float> p;
        juce::Point<float> start;
        bool started = false;
    };
    struct KnobDrag {
        bool active = false;
        int i = -1;
        float y = 0.f;
        float v0 = 0.f;
    };
    struct MenuItem {
        int kind = 0;  // 0 unplug n, 1 plug new on top, 2 unplug all
        int n = -1;
        juce::String label;
        juce::Colour dot;
        bool hasDot = true;
    };
    struct Menu {
        bool open = false;
        JackId jack;
        juce::String header;
        std::vector<MenuItem> items;
        juce::Rectangle<float> bounds;
        int hover = -1;
    };
    struct Level {
        int o = 0;
        int i = 0;
    };

    // state (mirrors the prototype's S object)
    Mode mode_ = Tape;
    std::array<int, 5> sel_{{0, 0, 0, 0, 2}};
    std::array<std::array<float, 4>, 5> enc_{};
    bool bay_ = false, fx_ = true, playing_ = false, rec_ = false, rev_ = false, loop_ = true, shift_ = false;
    int arm_ = 0;
    std::array<bool, 4> mutes_{{false, false, true, false}};
    int oct_ = 0;
    int note_ = -1;
    juce::String msg_;
    double msgUntil_ = 0.0;
    JackId pick_;
    int color_ = 1;
    bool stack_ = false;
    std::vector<Cord> cords_;
    juce::String info_;

    Drag drag_;
    KnobDrag kd_;
    Menu menu_;
    int focusKnob_ = -1;
    int pressedPad_ = -1;
    int pressedMem_ = -1;
    int pressedTop_ = -1;
    int heldNote_ = -1;
    int heldKeyCode_ = 0;
    bool mouseNote_ = false;

    // shift layer state
    bool shiftLatch_ = false, shiftMouse_ = false, shiftKey_ = false, shiftWas_ = false;
    double shiftDownT_ = 0.0;
    float shiftAmt_ = 0.f;
    double shiftOnT_ = -10.0;
    int lastFnKind_ = -1, lastFnIdx_ = -1;
    double lastFnT_ = -10.0;
    int clearConfirm_ = -1;
    double clearT_ = -10.0;
    std::array<int, 4> marks_{{0, 0, 0, 0}};
    int nextMark_ = 0;
    int loopIn_ = 0;
    int loopOut_ = LiftProcessor::kLoopSeconds * 48000;
    int transpose_ = 0, seqDiv_ = 16, drumDiv_ = 16, drumLen_ = 16, swing_ = 0, recSource_ = 0, character_ = 0;
    void updateShift();
    void shiftKeyFn(int note);
    void shiftCombo(int fn);
    void sendLoop();
    void markFn(int kind, int idx);
    int midiOf(int n) const { return 48 + 12 * oct_ + n + transpose_; }
    void paintShiftOverlay(juce::Graphics& g, float amt);
    void paintShiftBloom(juce::Graphics& g, juce::Rectangle<float> key, float amt);
    juce::Rectangle<int> shiftKeyArea() const;

    double lastTick_ = 0.0;
    double t_ = 0.0;
    bool reducedMotion_ = false;
    ScreenAnim an_;
    juce::Image screenBg_;
    float screenBgScale_ = 0.f;
    juce::Image cableImg_;
    juce::int64 cableKey_ = -1;
    float cableScale_ = 0.f;

    LiftProcessor& proc_;
    juce::Image grain_;

    // helpers
    void say(const juce::String& t);
    void flash(const juce::String& t);
    void syncEngine();
    void publishPatch();
    float speedOf() const;
    juce::StringArray labels() const;
    juce::String encDisplay(int i) const;
    juce::String counterText() const;
    juce::Point<float> toDev(juce::Point<float> p) const;
    JackId jackAt(juce::Point<float> dev) const;
    int knobAt(juce::Point<float> dev) const;
    int padAt(juce::Point<float> dev) const;
    int memAt(juce::Point<float> dev) const;
    int keyAt(juce::Point<float> dev) const;
    int topAt(juce::Point<float> canvas) const;
    std::vector<int> cordsAt(char r, int i) const;
    const Cord* topCord(char r, int i, int except) const;
    bool blocked(char r, int i, int except) const;
    std::vector<Level> levels(int except) const;
    bool validTarget(char r, int i) const;
    bool addCord(int o, int i, int c);
    void clickJack(JackId j, juce::Point<float> canvas);
    void openMenu(JackId j, juce::Point<float> canvas);
    void menuChoose(int k);
    void padAct(int k);
    void memAct(int k);
    void topAct(int k);
    void timerCallback() override;

    // painting (LiftPanelPaint.cpp)
    void paintTopBar(juce::Graphics& g);
    void paintCase(juce::Graphics& g);
    void paintBay(juce::Graphics& g);
    void paintBrand(juce::Graphics& g);
    void paintScreen(juce::Graphics& g);
    void paintStatus(juce::Graphics& g);
    void paintView(juce::Graphics& g);
    void paintViewFor(juce::Graphics& g, int view);
    void paintTransition(juce::Graphics& g, int view, float p);
    void paintScreenBg(juce::Graphics& g);
    double getSampleRateForScope() const { return proc_.getSampleRate() > 0.0 ? proc_.getSampleRate() : 48000.0; }
    void paintViewTape(juce::Graphics& g);
    void paintViewSynth(juce::Graphics& g);
    void paintViewDrum(juce::Graphics& g);
    void paintViewMix(juce::Graphics& g);
    void paintViewIn(juce::Graphics& g);
    void paintViewBay(juce::Graphics& g);
    void paintFoot(juce::Graphics& g);
    void paintKnobs(juce::Graphics& g);
    void paintPads(juce::Graphics& g);
    void paintMembrane(juce::Graphics& g);
    void paintKeys(juce::Graphics& g);
    void paintCables(juce::Graphics& g);
    void paintMenu(juce::Graphics& g);
    void buildGrain();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiftPanel)
};

}  // namespace lift
