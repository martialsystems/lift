// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "LiftProcessor.h"
#include "CableRope.h"
#include "ScreenAnim.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lift {

// The LIFT front panel, drawn natively 1:1 from the HTML prototype
// (LIFT-v3.html, v3.1). Everything is laid out in the prototype's CSS pixels
// ("canvas" pixels, 1360 x 1106: the control bar and info line, then the
// 1328 x 994 case at (16, 76)). The component is sized in real pixels
// (one of LiftEditor's fixed sizes) and draws the canvas at that scale.
//
// Rendering (PanelRender.cpp), the RONIN / BUSHIDO model:
//  - the static art (case, bay labels, brand, screen bezel, membrane sheet) is
//    one image at device resolution, built once per window size;
//  - parts that change (jacks, knobs, pads, keys, top bar) are drawn over it,
//    from cached device-resolution sprites where they carry blurred shadows;
//  - each frame compares every part with what was painted and repaints only
//    the parts that changed (dirty rectangles);
//  - the screen is a fixed-resolution display (Screen, 1200 x 812) redrawn in
//    the display frame loop, and the cables live on their own layer (Cables).
class LiftPanel : public juce::Component, private LiftProcessor::Listener {
public:
    static constexpr int kW = 1360;
    static constexpr int kH = 1106;

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
    // Pin matrix: 16 rows (sources) x 16 columns (destinations); 0 none,
    // 1 +100 %, 2 +50 %, 3 -100 %.
    int pin(int r, int c) const { return pins_[static_cast<size_t>(r * 16 + c)]; }
    void setPin(int r, int c, int k);
    void clickPin(int r, int c);  // cycles none -> +100 -> +50 -> -100 -> none
    // The context hint line: what the touched or held control does.
    juce::String hintText() const;
    juce::String contextText() const;  // screen, page and knob side, e.g. "TAPE \xc2\xb7 DECK \xc2\xb7 A"
    // Push-encoder values (0..1) by keypad slot.
    float mk(int slot) const { return mk_[static_cast<size_t>(slot)]; }
    void turnMk(int slot, float v);
    int physicalKnob(int slot) const;  // where macro `slot` sits on this screen (tests)
    juce::String infoText() const { return info_; }
    void tick() { advance(1.0 / 60.0); }
    // One display frame (the VBlank loop calls it with wall time).
    void frame(double dt);
    // Right-click on the panel: the editor's window-size menu.
    std::function<void(juce::PopupMenu&)> addSizeItems;
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

    // Save slots (SHIFT + DROP = save, SHIFT + PLAY = load): a slot picker on
    // the screen. OCT- / OCT+, any knob, the wheel, arrow keys or typed digits
    // pick 001..999; PLAY, Enter or the same combo again confirms; STOP or Esc
    // cancels.
    enum class Picker { None, Save, Load };
    void openPicker(Picker p);
    void pickStep(int delta);
    void pickConfirm();
    void pickCancel();
    Picker picker() const { return picker_; }
    int pickSlot() const { return pickSlot_; }
    // MIDI learn (SHIFT + REV): touch a knob, then move a controller.
    bool learning() const { return learnArm_; }
    UiState captureUi() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    float uiScale() const { return scale_; }
    // canvas pixels -> component pixels
    juce::Rectangle<int> toLocal(juce::Rectangle<float> canvas) const;
    juce::Point<float> toCanvas(juce::Point<float> local) const { return local / scale_; }
    static juce::Rectangle<float> screenGlass();  // the display, canvas pixels
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
    bool bay_ = false, fx_ = false, playing_ = false, rec_ = false, rev_ = false, loop_ = true, shift_ = false;
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
    std::array<std::uint8_t, 256> pins_{};
    bool recJackLifts_ = false;  // REC jack option: it presses LIFT
    int mxHover_ = -1;  // hovered cell r * 16 + c
    std::array<float, 10> mk_{{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.7f, 0.5f, 0.5f, 0.5f, 0.5f}};
    juce::String hint_;      // the hint line's text for what was last touched
    double hintT_ = -10.0;
    void hint(const juce::String& t) { hint_ = t; hintT_ = t_; }
    int pinAt(juce::Point<float> dev) const;
    int slotOf(int k) const;  // physical knob (by colour) -> macro slot of this screen
    struct MkDrag {
        bool active = false, turned = false, held = false;
        int slot = -1;
        float y = 0.f, v0 = 0.f;
        double t0 = 0.0;
    };
    MkDrag mkd_;
    juce::String encHint(int fn) const;
    float liftSeconds() const;
    void memHold(int slot);
    // SELECT (P3): the window over what would be kept
    bool selectOpen_ = false;
    bool selectFromLift_ = true;
    void openSelect(int how);  // 0 a knob after a keep, 1 hold LIFT, 2 hold REC
    void closeSelect();
    void selectTurn(int fn, float delta);
    void selectKnob(int colour, float delta);  // physical knob: 0 yellow, 1 blue, 2 black, 3 red
    bool selectUndo();
    // resampling: the newest keep, its selection, where DROP puts it
    LiftProcessor::ClipPtr clip_;
    eng::Selection selc_;
    std::vector<eng::Selection> selHist_;
    double selEditT_ = -10.0;
    int selEditKnob_ = -1;
    float selZoom_ = 0.f;          // 0 = the whole clip .. 1 = 1/64 of it, around the selection
    double keptT_ = -10.0;         // when the last keep happened (a main knob turned soon after opens SELECT)
    enum DropDest { DestLoop = 0, DestKeys = 4, DestKey = 5, DestDrum = 6, DestIn = 7 };
    int lastDest_ = -1;            // -1 = the armed loop
    int lastDestNote_ = 60;
    bool dropPick_ = false;        // hold DROP: the next T pad / key / DRUM key / IN picks the place
    LiftProcessor::ClipPtr keysAt_, drumAt_;
    std::array<LiftProcessor::ClipPtr, 128> keyAt_{};
    int seenPass_ = 0;
    int seenLift_ = 0;
    bool wasRec_ = false;
    std::vector<float> wavePk_;    // SELECT waveform cache (min / max per pixel)
    juce::int64 waveKey_ = -1;
    bool keep(LiftProcessor::ClipPtr c, const juce::String& what);
    void place(int dest, int note = -1);
    void paintViewSelect(juce::Graphics& g);
    void paintSelectBanner(juce::Graphics& g);
    // one history: cables, pins, and (audioOp >= 0) a keep / drop / overdub
    struct HistEntry {
        std::vector<Cord> cords;
        std::array<std::uint8_t, 256> pins{};
        int audioOp = -1;               // 0 KEEP, 1 DROP, 2 OVERDUB
        LiftProcessor::ClipPtr prevClip; // KEEP: the clip before; DROP elsewhere: what the place held
        int dest = -1, note = -1;       // DROP: where
        int pass = 0, track = 0;        // OVERDUB: pass number and track
        eng::Selection prevSel;
    };
    std::vector<HistEntry> hist_;
    void pushUndo(int audioOp = -1);
public:
    void undo();
    int historySize() const { return static_cast<int>(hist_.size()); }
    const LiftProcessor::ClipPtr& keptClip() const { return clip_; }
    const eng::Selection& selection() const { return selc_; }
    bool selectIsOpen() const { return selectOpen_; }
    int lastDest() const { return lastDest_; }
    void exportTo(const juce::File& folder);  // standalone: clip, T1-T4, master as 24-bit WAV
private:
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
    bool cassette_ = false;
    void updateShift();
    void shiftKeyFn(int note);
    void shiftCombo(int fn);
    void sendLoop();
    void markFn(int kind, int idx);
    int midiOf(int n) const { return 48 + 12 * oct_ + n + transpose_; }
    void paintShiftOverlay(juce::Graphics& g, float amt);
    juce::Rectangle<int> shiftKeyArea() const;

    double lastTick_ = 0.0;
    double t_ = 0.0;
    bool reducedMotion_ = false;
    ScreenAnim an_;
    juce::Image screenBg_;
    float screenBgScale_ = 0.f;

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

    // processor state sync (LiftProcessor::Listener)
    void applyUi(const UiState& s);
    void pushUi();
    void flushState() override { pushUi(); }
    void stateLoaded(int slot) override;
    void knobFromMidi(int target, float value) override;
    void learned(int cc, int target) override;
    void slotMessage(const juce::String& text) override { flash(text); repaint(); }
    juce::String knobName(int target) const;
    void paintPicker(juce::Graphics& g);
    UiState lastPushed_;
    std::array<int, 128> learn_;
    // DRUM patterns and per-voice knobs, FX type and knobs (mirrored in UiState)
    std::array<std::array<uint32_t, eng::kDrumVoices>, eng::kKits> drumPat_{};
    int drumVoice_ = 0;
    std::array<std::array<float, 3>, eng::kDrumVoices> drumKnobs_{};
    int fxType_ = 0;
    std::array<std::array<float, 4>, eng::kFxTypes> fxKnobs_{};
    bool fxEdit_ = false;      // the four knobs set the effect (long-press FX)
    double fxDownT_ = -1.0;
    float encAt(int i) const;  // the value knob i shows (the screen's, or the effect's in FX edit)
    void selectDrumVoice(int v, bool announce);
    void toggleStep(int step);
    bool learnArm_ = false;
    int heldMidi_ = -1;
    int seenTransport_ = 0;
    Picker picker_ = Picker::None;
    int pickSlot_ = 1;
    double pickT_ = -10.0, pickStepT_ = -10.0;
    juce::String typed_;
    float pickDragY_ = 0.f;

    // painting (LiftPanelPaint.cpp)
    struct PadLook {
        juce::Rectangle<float> r;
        juce::uint32 col = 0;
        bool lit = false, unused = false;
        juce::Colour ink;
        juce::String big, label;
        float bigSize = 0.f;
    };
    void paintTopBar(juce::Graphics& g);
    void paintCase(juce::Graphics& g);
    void paintBayStatic(juce::Graphics& g, bool recess);
    static void drawJack(juce::Graphics& g, char rc, int i, bool picked, bool ok);
    void paintBrand(juce::Graphics& g);
    void paintScreen(juce::Graphics& g);
    void paintStatus(juce::Graphics& g);
    void paintView(juce::Graphics& g);
    void paintViewFor(juce::Graphics& g, int view);
    void paintTransition(juce::Graphics& g, int view, float p);
    void paintScreenBg(juce::Graphics& g);
    static void drawScreenBezel(juce::Graphics& g, bool wrap);
    double getSampleRateForScope() const { return 48000.0; }  // the scope tap runs at the tape rate
    void paintViewTape(juce::Graphics& g);
    void paintViewSynth(juce::Graphics& g);
    void paintViewDrum(juce::Graphics& g);
    void paintViewMix(juce::Graphics& g);
    void paintViewIn(juce::Graphics& g);
    void paintViewBay(juce::Graphics& g);
    void paintFoot(juce::Graphics& g);
    static juce::Point<float> knobOrigin(int i);
    static void drawKnobTicks(juce::Graphics& g, float v);
    static void drawKnobSkirt(juce::Graphics& g, int i);
    static void drawKnobBody(juce::Graphics& g, int i, float v, bool contactShadow = true);
    static void drawKnobContact(juce::Graphics& g);
    static void drawKnobCap(juce::Graphics& g);
    void paintKnobLabels(juce::Graphics& g);
    static void drawPadBody(juce::Graphics& g, juce::Rectangle<float> r, juce::uint32 fillCol, bool lit);
    static void drawPadText(juce::Graphics& g, const PadLook& L, float alpha);
    PadLook padLook(int k) const;
    void paintMembraneStatic(juce::Graphics& g, bool frameToo);
    void paintMemKeys(juce::Graphics& g);
    static void drawSharp(juce::Graphics& g, juce::Point<float> c, bool down);
    static void drawNatural(juce::Graphics& g, juce::Rectangle<float> r, bool down);
    juce::Point<float> sharpPos(int o, int b) const;
    juce::Rectangle<float> naturalRect(int j) const;
    void paintKeyLabels(juce::Graphics& g);
    void paintMatrix(juce::Graphics& g);
    void paintHint(juce::Graphics& g);
    void paintCables(juce::Graphics& g);
    static void drawSettledCable(juce::Graphics& g, const juce::Path& d, float x1, float y1, float x2, float y2, int ci,
                                 bool st, bool cheap);
    void paintLiveCable(juce::Graphics& g);
    static juce::Rectangle<float> cableBounds(const juce::Rectangle<float>& path);
    struct CordEnds {
        float x1, y1, x2, y2;
    };
    CordEnds cordEnds(size_t n, const std::vector<Level>& lv) const;
    ui::Rope& ropeFor(size_t n, const std::vector<Level>& lv);
    void buildRopeScene();
    void trackRopes(juce::Point<float> dev, bool in);
    void stepRopes();
    juce::Point<float> liveFixed() const;
    int liveCloth() const;
    std::unordered_map<int, ui::Rope> ropes_;
    ui::RopeScene scene_;
    int ropeIdle_ = 0;
    bool ropeAwake_ = false;
    juce::Rectangle<int> ropeShown_;
    juce::Rectangle<float> liveCableBounds() const;
    void paintMenu(juce::Graphics& g);
    void buildGrain();

    // layers and invalidation (PanelRender.cpp)
    class Screen;
    class Cables;
    friend class Screen;
    friend class Cables;
    std::unique_ptr<Screen> screen_;
    std::unique_ptr<Cables> cables_;
    std::unique_ptr<juce::VBlankAttachment> vblank_;
    double lastVBlank_ = -1.0;
    double screenClock_ = 0.0;
    float scale_ = 1.f;
    bool screenDirty_ = true;
    bool inFrame_ = false;
    juce::Image art_;
    float artDps_ = 0.f;
    struct Sprite {
        juce::Image img;
        juce::Point<int> off;  // device offset from the anchor's pixel
    };
    std::unordered_map<juce::uint64, Sprite> sprites_;
    float spriteDps_ = 0.f;
    struct KnobImg {
        juce::Image img, skirt, cap, contact;
        juce::Point<int> org;  // device pixel of the image's top-left
        float v = -1.f, dps = 0.f;
        // recent values (one per screen: switching screens re-shows them)
        std::array<std::pair<float, juce::Image>, 5> recent{};
        int next = 0;
    };
    std::array<KnobImg, 4> knobImg_;
    std::vector<juce::uint64> shown_;   // per part: what is on screen
    juce::Image cableImg_;               // settled cables (Cables layer)
    juce::Rectangle<int> cableImgDev_;   // its device rectangle
    juce::uint64 cableKey_ = 0;
    float cableDps_ = 0.f;
    juce::Rectangle<int> cableShown_, liveShown_, menuShown_;
    juce::uint64 menuKey_ = 0;
    juce::Image fb_;                     // the display's framebuffer
    struct CableSprite {
        juce::Image img, gray;
        juce::Rectangle<int> dev;  // device pixels
        bool used = false;
    };
    std::unordered_map<juce::uint64, CableSprite> cableSprites_;
    void buildCableImage(float phys);
    static void makeGray(CableSprite& cs);
    void initLayers();
    void repaint();                                     // state changed: invalidate what changed
    void repaint(juce::Rectangle<int> canvasArea);      // canvas pixels
    void refreshParts();
    void refreshOverlay();
    int partCount() const;
    juce::uint64 partSig(int p) const;
    juce::Rectangle<float> partRect(int p) const;       // canvas pixels
    void ensureArt(float dps);
    void blit(juce::Graphics& g, float phys, juce::uint64 variant, juce::Rectangle<float> devBounds,
              juce::Point<float> devAnchor, const std::function<void(juce::Graphics&)>& draw, float opacity = 1.f);
    void paintKnob(juce::Graphics& g, float phys, int i);
    void paintScreenLayer(juce::Graphics& g);
    void paintCableLayer(juce::Graphics& g);
    void renderFb();
    void onVBlank(double ts);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiftPanel)
};

}  // namespace lift
