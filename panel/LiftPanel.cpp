// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "LiftPanel.h"

#include "Fonts.h"
#include "PanelData.h"
#include "tape/character.h"

#include <cmath>

namespace lift {

using namespace ui;

namespace {
const char* const kMapKeys = "awsedftgyhujkolp;'";
const char* const kModeIds[5] = {"synth", "drum", "tape", "mix", "in"};

int modeFromId(const juce::String& s) {
    for (int k = 0; k < 5; ++k) {
        if (s == kModeIds[k]) {
            return k;
        }
    }
    return 2;
}

juce::String jackName(char r, int i) {
    return juce::String::fromUTF8(r == 'o' ? OUTS[i].n : INS[i].n);
}

const juce::String kArrow = juce::String::fromUTF8("\xe2\x86\x92");
const juce::String kLarrow = juce::String::fromUTF8("\xe2\x86\x90");
}  // namespace

LiftPanel::LiftPanel(LiftProcessor& p) : proc_(p) {
    // the processor keeps the state (it outlives the editor and saves it)
    learn_.fill(-1);
    applyUi(proc_.uiState());
    lastPushed_ = captureUi();
    seenTransport_ = proc_.transportSerial.load();
    info_ = "Drag from an OUT jack to an IN jack to patch. Drag a knob up or down to turn it.";
    setSize(kW, kH);
    setWantsKeyboardFocus(true);
    setOpaque(true);
    buildGrain();
    publishPatch();
    syncEngine();
    an_.hitT.fill(-10.0);
    an_.sliceT.fill(-10.0);
    an_.peakT.fill(-10.0);
    an_.muteT.fill(-10.0);
    for (int t = 0; t < 4; ++t) {
        an_.lastMute[static_cast<size_t>(t)] = mutes_[static_cast<size_t>(t)];
    }
    an_.lastCords = cords_.size();
    advance(0.0);
    lastTick_ = juce::Time::getMillisecondCounterHiRes();
    proc_.setListener(this);
    startTimerHz(60);
}

LiftPanel::~LiftPanel() {
    stopTimer();
    pushUi();
    proc_.setListener(nullptr);
    proc_.learnTarget.store(-1);
}

// ---------------------------------------------------------------- helpers

void LiftPanel::say(const juce::String& t) {
    info_ = t;
}

void LiftPanel::flash(const juce::String& t) {
    msg_ = t;
    an_.msgT = t_;
    msgUntil_ = t_ + 1.6;
}

float LiftPanel::speedOf() const {
    return 0.25f * std::pow(16.f, enc_[Tape][0]);
}

// TAPE knobs drive the engine: SPEED (0.25x..4x), BIAS, REC LVL (drive; 89 on
// the dial is the Deck default of 1.8), SCRUB (playhead while stopped).
void LiftPanel::syncEngine() {
    proc_.speed.store(speedOf());
    proc_.bias.store(enc_[Tape][1]);
    proc_.drive.store(1.8f * std::pow(16.f, enc_[Tape][2] - 0.7f));
}

void LiftPanel::publishPatch() {
    proc_.patch.publish(cords_);
}

juce::StringArray LiftPanel::labels() const {
    juce::StringArray a;
    for (int i = 0; i < 4; ++i) {
        a.add(juce::String::fromUTF8(mode_ == Synth ? ENGINES[sel_[Synth]].m[i] : MODE_MACROS[mode_][i]));
    }
    return a;
}

juce::String LiftPanel::encDisplay(int i) const {
    const float v = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
    if (mode_ == Tape && i == 0) {
        return juce::String(speedOf(), 2) + juce::String::fromUTF8("\xc3\x97");
    }
    if (mode_ == Mix && i == 1) {
        if (v < 0.48f) {
            return "L" + juce::String(juce::roundToInt((0.5f - v) * 200.f));
        }
        if (v > 0.52f) {
            return "R" + juce::String(juce::roundToInt((v - 0.5f) * 200.f));
        }
        return "C";
    }
    if (mode_ == Synth && sel_[Synth] == 7) {
        return "--";
    }
    return juce::String(juce::roundToInt(v * 99.f)).paddedLeft('0', 2);
}

// The tape counter shows the real playhead.
juce::String LiftPanel::counterText() const {
    const double secs = proc_.uiPos.load() / static_cast<double>(::kSampleRate);
    const int ms = static_cast<int>(secs * 1000.0);
    const juce::String mmss =
        juce::String(ms / 60000).paddedLeft('0', 2) + ":" + juce::String((ms / 1000) % 60).paddedLeft('0', 2);
    if (rec_ && playing_) {
        return "REC " + mmss;
    }
    return mmss + "." + juce::String(ms % 1000).paddedLeft('0', 3);
}

juce::Point<float> LiftPanel::toDev(juce::Point<float> p) const {
    return {p.x - kDevX, p.y - kDevY};
}

LiftPanel::JackId LiftPanel::jackAt(juce::Point<float> d) const {
    for (int r = 0; r < 2; ++r) {
        const char rc = r == 0 ? 'o' : 'i';
        for (int i = 0; i < 16; ++i) {
            if (d.getDistanceFrom({jx(i), jy(rc)}) <= 16.f) {
                return {rc, i};
            }
        }
    }
    return {};
}

int LiftPanel::knobAt(juce::Point<float> d) const {
    for (int i = 0; i < 4; ++i) {
        if (d.getDistanceFrom(knobCentre(i)) <= 70.f) {
            return i;
        }
    }
    return -1;
}

int LiftPanel::padAt(juce::Point<float> d) const {
    for (int k = 0; k < 24; ++k) {
        if (padRect(k).contains(d)) {
            return k;
        }
    }
    return -1;
}

int LiftPanel::memAt(juce::Point<float> d) const {
    for (int k = 0; k < 10; ++k) {
        if (memRect(k).contains(d)) {
            return k;
        }
    }
    return -1;
}

int LiftPanel::keyAt(juce::Point<float> d) const {
    for (int o = 0; o < 2; ++o) {
        for (int b = 0; b < 5; ++b) {
            if (d.getDistanceFrom(sharpCentre(o, b)) <= 21.f) {
                return o * 12 + SHARP_DEF[b][0];
            }
        }
    }
    for (int j = 0; j < 14; ++j) {
        if (natRect(j).contains(d)) {
            return (j / 7) * 12 + NAT_SEMI[j % 7];
        }
    }
    return -1;
}

int LiftPanel::topAt(juce::Point<float> c) const {
    const auto r = topBarRects();
    for (int k = 0; k < 9; ++k) {
        if (r[static_cast<size_t>(k)].contains(c)) {
            return k;
        }
    }
    return -1;
}

std::vector<int> LiftPanel::cordsAt(char r, int i) const {
    std::vector<int> v;
    for (size_t n = 0; n < cords_.size(); ++n) {
        if ((r == 'o' ? cords_[n].o : cords_[n].i) == i) {
            v.push_back(static_cast<int>(n));
        }
    }
    return v;
}

const Cord* LiftPanel::topCord(char r, int i, int except) const {
    const Cord* t = nullptr;
    for (size_t n = 0; n < cords_.size(); ++n) {
        if (static_cast<int>(n) != except && (r == 'o' ? cords_[n].o : cords_[n].i) == i) {
            t = &cords_[n];
        }
    }
    return t;
}

bool LiftPanel::blocked(char r, int i, int except) const {
    const Cord* t = topCord(r, i, except);
    return t != nullptr && !t->st;
}

std::vector<LiftPanel::Level> LiftPanel::levels(int except) const {
    std::vector<Level> out(cords_.size());
    int so[16] = {};
    int si[16] = {};
    for (size_t n = 0; n < cords_.size(); ++n) {
        if (static_cast<int>(n) == except) {
            continue;
        }
        out[n] = {so[cords_[n].o]++, si[cords_[n].i]++};
    }
    return out;
}

bool LiftPanel::validTarget(char r, int i) const {
    if (!drag_.active || r != drag_.need) {
        return false;
    }
    const int o = r == 'o' ? i : drag_.fixed.i;
    const int ii = r == 'i' ? i : drag_.fixed.i;
    for (size_t n = 0; n < cords_.size(); ++n) {
        if (cords_[n].o == o && cords_[n].i == ii && !(drag_.isMove && static_cast<int>(n) == drag_.n)) {
            return false;
        }
    }
    return !blocked(r, i, drag_.isMove ? drag_.n : -1);
}

bool LiftPanel::addCord(int o, int i, int c) {
    for (const auto& x : cords_) {
        if (x.o == o && x.i == i) {
            say(jackName('o', o) + " is already patched to " + jackName('i', i) + ".");
            return false;
        }
    }
    if (blocked('o', o, -1) || blocked('i', i, -1)) {
        say("That jack already has a plug with a closed cap. Only a stackable plug, with an open jack on top, takes "
            "another cable.");
        return false;
    }
    cords_.push_back({o, i, c, stack_});
    flash("PATCHED " + jackName('o', o) + " " + kArrow + " " + jackName('i', i));
    say("Patched " + jackName('o', o) + " to " + jackName('i', i) + ".");
    publishPatch();
    return true;
}

// ---------------------------------------------------------------- actions

void LiftPanel::act(const juce::String& a) {
    const juce::String k = a.upToFirstOccurrenceOf(":", false, false);
    const juce::String arg = a.fromFirstOccurrenceOf(":", false, false);
    const int n = arg.getIntValue();
    if (k == "mode") {
        mode_ = static_cast<Mode>(modeFromId(arg));
        bay_ = false;
        say(arg.toUpperCase() + " screen.");
    } else if (k == "fx") {
        fx_ = !fx_;
        say(fx_ ? "Effect insert on." : "Effect insert bypassed.");
    } else if (k == "bay") {
        bay_ = !bay_;
        say(bay_ ? "Patch list on the screen." : "Patch list closed. Cables keep working.");
    } else if (k == "radio") {
        mode_ = In;
        sel_[In] = 2;
        bay_ = false;
        say("Radio dial. Encoder 1 picks the station.");
    } else if (k == "num") {
        const bool unused = mode_ == Mix || (mode_ == In && INPUTS[n][0] == 0);
        if (unused) {
            say("Keys 1 to 8 do nothing on this screen.");
            repaint();
            return;
        }
        sel_[static_cast<size_t>(mode_)] = n;
    } else if (k == "arm") {
        arm_ = n;
        proc_.send(Cmd::Arm, n);
        say("Track " + juce::String(n + 1) + " armed.");
    } else if (k == "mute") {
        mutes_[static_cast<size_t>(n)] = !mutes_[static_cast<size_t>(n)];
        proc_.send(Cmd::Mute, n, mutes_[static_cast<size_t>(n)] ? 1 : 0);
        say("Track " + juce::String(n + 1) + (mutes_[static_cast<size_t>(n)] ? " muted." : " unmuted."));
    } else if (k == "lift") {
        proc_.send(Cmd::Lift);
        an_.liftT = t_;
        flash(loop_ ? "LIFTED " + juce::String::fromUTF8("\xc2\xb7") + " LOOP ON T" + juce::String(arm_ + 1)
                    : "LIFTED " + juce::String::fromUTF8("\xc2\xb7") + " ALL OF T" + juce::String(arm_ + 1));
    } else if (k == "drop") {
        proc_.send(Cmd::Drop);
        an_.dropT = t_;
        flash(juce::String::fromUTF8("DROPPED \xc2\xb7 OVERDUB \xc2\xb7 5 MS FADES"));
    } else if (k == "loop") {
        loop_ = !loop_;
        sendLoop();
    } else if (k == "shift") {
        shiftLatch_ = !shiftLatch_;
        updateShift();
        say(shiftLatch_ ? "Shift latched. Keys and buttons run their shifted functions; tap SHIFT to release."
                        : "Shift released.");
    } else if (k == "rev") {
        rev_ = !rev_;
        proc_.send(Cmd::Rev, rev_ ? 1 : 0);
    } else if (k == "rec") {
        rec_ = !rec_;
        if (playing_) {
            proc_.send(Cmd::Transport, 1, rec_ ? 1 : 0);
        }
    } else if (k == "play") {
        playing_ = !playing_;
        proc_.send(Cmd::Transport, playing_ ? 1 : 0, rec_ ? 1 : 0);
        if (mode_ != Tape && playing_) {
            say("Tape running. Open TAPE to see the reels.");
        }
    } else if (k == "stop") {
        playing_ = false;
        rec_ = false;
        rev_ = false;
        proc_.send(Cmd::Stop);
    } else if (k == "octd") {
        oct_ = juce::jmax(-3, oct_ - 1);
    } else if (k == "octu") {
        oct_ = juce::jmin(3, oct_ + 1);
    }
    repaint();
}

void LiftPanel::noteOn(int n) {
    if (heldMidi_ >= 0) {
        proc_.send(Cmd::NoteOff, heldMidi_);
    }
    heldNote_ = n;
    note_ = n;
    heldMidi_ = midiOf(n);
    proc_.send(Cmd::NoteOn, heldMidi_, 100);
    repaint();
}

void LiftPanel::noteOff(int n) {
    if (n >= 0 && n != heldNote_) {
        return;
    }
    if (heldMidi_ >= 0) {
        proc_.send(Cmd::NoteOff, heldMidi_);  // only the panel's note: MIDI-held notes keep sounding
    }
    heldMidi_ = -1;
    heldNote_ = -1;
    heldKeyCode_ = 0;
    note_ = -1;
    repaint();
}

void LiftPanel::setEnc(int i, float v) {
    auto& e = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
    e = juce::jlimit(0.f, 1.f, v);
    if (learnArm_) {
        const int target = static_cast<int>(mode_) * 4 + i;
        if (proc_.learnTarget.exchange(target) != target) {
            flash("LEARN " + knobName(target) + ": MOVE A CONTROL");
        }
    }
    if (mode_ == Tape) {
        syncEngine();
        if (i == 3) {
            proc_.send(Cmd::Seek, 0, 0, static_cast<double>(e) * static_cast<double>(proc_.uiFrames.load()));
        }
    }
    repaint();
}

void LiftPanel::padAct(int k) {
    if (k < 5) {
        act(juce::String("mode:") + kModeIds[k]);
    } else if (k == 5) {
        act("fx");
    } else if (k == 6) {
        act("bay");
    } else if (k == 7) {
        act("radio");
    } else if (k < 16) {
        act("num:" + juce::String(k - 8));
    } else if (k < 20) {
        act("arm:" + juce::String(k - 16));
    } else {
        act("mute:" + juce::String(k - 20));
    }
}

void LiftPanel::memAct(int k) {
    const char* ids[10] = {"lift", "loop", "shift", "rev", "drop", "rec", "octd", "play", "octu", "stop"};
    const int fn = MEM_FN[k];
    if (picker_ != Picker::None && fn != 2) {
        // the slot picker owns the keypad: OCT-/OCT+ step, PLAY (or the same
        // combo) confirms, anything else cancels
        if (fn == 6 || fn == 8) {
            pickStep(fn == 6 ? -1 : 1);
        } else if (fn == 7 || (shiftActive() && ((fn == 4 && picker_ == Picker::Save) || (fn == 7 && picker_ == Picker::Load)))) {
            pickConfirm();
        } else if (shiftActive() && (fn == 4 || fn == 7)) {
            openPicker(fn == 4 ? Picker::Save : Picker::Load);
        } else {
            pickCancel();
        }
        return;
    }
    if (shiftActive() && (fn == 0 || fn == 1 || fn == 5 || fn == 9 || fn == 3 || fn == 4 || fn == 7)) {
        shiftCombo(fn);
        return;
    }
    act(ids[fn]);
}

void LiftPanel::topAct(int k) {
    if (k < 6) {
        color_ = k;
        say("New cables will be " + juce::String(CLOTH[k].n).toLowerCase() + (stack_ ? " stackables." : "."));
    } else if (k == 6) {
        stack_ = !stack_;
        pick_ = {};
        say(stack_ ? "Stackable cables on. Pick a color and drag: the plugs have an open jack on top, so another "
                     "cable can plug into them."
                   : "Stackable cables off. New cables get closed caps.");
    } else if (k == 7) {
        cords_.clear();
        pick_ = {};
        publishPatch();
        say("All cables unplugged.");
    } else if (k == 8) {
        for (int m = 0; m < 5; ++m) {
            for (int i = 0; i < 4; ++i) {
                enc_[static_cast<size_t>(m)][static_cast<size_t>(i)] = ENC_DEFAULT[m][i];
            }
        }
        syncEngine();
        say("Knobs reset on every screen.");
    }
    repaint();
}

// ---------------------------------------------------------------- patching

void LiftPanel::clickJack(JackId j, juce::Point<float> canvas) {
    if (pick_.valid()) {
        if (pick_ == j) {
            pick_ = {};
            say("Cancelled.");
            return;
        }
        if (pick_.r != j.r) {
            const int o = j.r == 'o' ? j.i : pick_.i;
            const int ii = j.r == 'i' ? j.i : pick_.i;
            pick_ = {};
            addCord(o, ii, color_);
            return;
        }
    }
    if (!cordsAt(j.r, j.i).empty()) {
        pick_ = {};
        openMenu(j, canvas);
        return;
    }
    pick_ = j;
    say(jackName(j.r, j.i) + " picked. Click " + (j.r == 'o' ? "an IN" : "an OUT") +
        " jack to patch it, or the same jack to cancel.");
}

void LiftPanel::openMenu(JackId j, juce::Point<float> canvas) {
    const auto here = cordsAt(j.r, j.i);
    menu_ = {};
    menu_.open = true;
    menu_.jack = j;
    menu_.header = jackName(j.r, j.i) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(static_cast<int>(here.size())) +
                   " cable" + (here.size() > 1 ? "s" : "");
    for (int n : here) {
        const Cord& c = cords_[static_cast<size_t>(n)];
        MenuItem it;
        it.kind = 0;
        it.n = n;
        it.dot = hex(CLOTH[c.c].c);
        it.label = "Unplug " + (j.r == 'o' ? kArrow + " " + jackName('i', c.i) : kLarrow + " " + jackName('o', c.o)) +
                   (c.st ? juce::String::fromUTF8(" \xc2\xb7 stackable") : juce::String());
        menu_.items.push_back(it);
    }
    if (cords_[static_cast<size_t>(here.back())].st) {
        MenuItem it;
        it.kind = 1;
        it.dot = hex(CLOTH[color_].c);
        it.label = "Plug a new cable on top";
        menu_.items.push_back(it);
    }
    if (here.size() > 1) {
        MenuItem it;
        it.kind = 2;
        it.hasDot = false;
        it.label = "Unplug all";
        menu_.items.push_back(it);
    }
    // size: padding 4, header ~26, rows ~33; min width 220
    const juce::Font f = jost(400, 13.f);
    float w = 220.f;
    for (const auto& it : menu_.items) {
        w = juce::jmax(w, 8.f + 20.f + (it.hasDot ? 21.f : 0.f) + juce::GlyphArrangement::getStringWidth(f, it.label));
    }
    w = juce::jmax(w, 8.f + 20.f + cssWidth(400, false, 11.f, 0.14f, menu_.header.toUpperCase()));
    const float h = 8.f + 26.3f + 32.9f * static_cast<float>(menu_.items.size());
    float x = canvas.x + 10.f;
    float y = canvas.y + 10.f;
    if (x + w > static_cast<float>(kW) - 8.f) {
        x = static_cast<float>(kW) - w - 8.f;
    }
    if (y + h > static_cast<float>(kH) - 8.f) {
        y = juce::jmax(8.f, canvas.y - h - 10.f);
    }
    menu_.bounds = {x, y, w, h};
}

void LiftPanel::menuChoose(int k) {
    const MenuItem it = menu_.items[static_cast<size_t>(k)];
    const JackId j = menu_.jack;
    if (it.kind == 0) {
        const Cord c = cords_[static_cast<size_t>(it.n)];
        cords_.erase(cords_.begin() + it.n);
        flash("UNPLUGGED " + jackName('o', c.o) + " " + kArrow + " " + jackName('i', c.i));
        say("Unplugged " + jackName('o', c.o) + " from " + jackName('i', c.i) + ".");
    } else if (it.kind == 1) {
        pick_ = j;
        say(juce::String("Click ") + (j.r == 'o' ? "an IN" : "an OUT") + " jack to finish the cable.");
    } else {
        std::vector<Cord> keep;
        for (const auto& c : cords_) {
            if ((j.r == 'o' ? c.o : c.i) != j.i) {
                keep.push_back(c);
            }
        }
        cords_ = keep;
        say("Unplugged everything from " + jackName(j.r, j.i) + ".");
    }
    menu_.open = false;
    publishPatch();
    repaint();
}

// ---------------------------------------------------------------- mouse

void LiftPanel::mouseMove(const juce::MouseEvent& e) {
    const auto c = e.position;
    const auto d = toDev(c);
    if (menu_.open) {
        int hv = -1;
        if (menu_.bounds.contains(c)) {
            const float y = c.y - menu_.bounds.getY() - 4.f - 26.3f;
            if (y >= 0.f) {
                hv = juce::jmin(static_cast<int>(menu_.items.size()) - 1, static_cast<int>(y / 32.9f));
            }
        }
        if (hv != menu_.hover) {
            menu_.hover = hv;
            repaint(menu_.bounds.getSmallestIntegerContainer().expanded(2));
        }
    }
    const JackId j = jackAt(d);
    if (j.valid()) {
        setMouseCursor(cordsAt(j.r, j.i).empty() ? juce::MouseCursor::CrosshairCursor
                                                  : juce::MouseCursor::DraggingHandCursor);
    } else if (knobAt(d) >= 0) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    } else if (padAt(d) >= 0 || memAt(d) >= 0 || keyAt(d) >= 0 || topAt(c) >= 0 ||
               (menu_.open && menu_.bounds.contains(c))) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    } else {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void LiftPanel::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    const auto c = e.position;
    const auto d = toDev(c);
    if (menu_.open) {
        if (menu_.bounds.contains(c)) {
            return;  // chosen on mouse up (a click)
        }
        menu_.open = false;
        repaint();
    }
    const JackId j = jackAt(d);
    if (j.valid()) {
        const auto here = cordsAt(j.r, j.i);
        const int topN = here.empty() ? -1 : here.back();
        drag_ = {};
        drag_.active = true;
        drag_.p = d;
        drag_.start = c;
        drag_.from = j;
        if (topN >= 0 && !(stack_ && cords_[static_cast<size_t>(topN)].st)) {
            const Cord& tc = cords_[static_cast<size_t>(topN)];
            const char fr = j.r == 'o' ? 'i' : 'o';
            const auto lv = levels(-1)[static_cast<size_t>(topN)];
            drag_.isMove = true;
            drag_.n = topN;
            drag_.c = tc.c;
            drag_.st = tc.st;
            drag_.fixed = {fr, j.r == 'o' ? tc.i : tc.o};
            drag_.flv = fr == 'o' ? lv.o : lv.i;
            drag_.need = j.r;
        } else {
            drag_.isMove = false;
            drag_.c = color_;
            drag_.st = stack_;
            drag_.fixed = j;
            drag_.flv = static_cast<int>(here.size());
            drag_.need = j.r == 'o' ? 'i' : 'o';
        }
        return;
    }
    const int k = knobAt(d);
    if (k >= 0) {
        focusKnob_ = k;
        kd_ = {true, k, c.y, enc_[static_cast<size_t>(mode_)][static_cast<size_t>(k)]};
        pickDragY_ = c.y;
        return;
    }
    const int key = keyAt(d);
    if (key >= 0) {
        if (shiftActive()) {
            pressKey(key);
            return;
        }
        mouseNote_ = true;
        noteOn(key);
        return;
    }
    pressedPad_ = padAt(d);
    pressedMem_ = memAt(d);
    if (pressedMem_ == kShiftSlot) {
        // press and hold = momentary shift; a quick tap latches (on release)
        shiftMouse_ = true;
        shiftDownT_ = t_;
        updateShift();
    }
    pressedTop_ = topAt(c);
    repaint();
}

void LiftPanel::mouseDrag(const juce::MouseEvent& e) {
    const auto c = e.position;
    if (drag_.active) {
        if (!drag_.started && c.getDistanceFrom(drag_.start) > 5.f) {
            drag_.started = true;
            pick_ = {};
            say(!drag_.isMove ? juce::String("Drop on ") + (drag_.need == 'i' ? "a blue IN" : "a red OUT") +
                                    " jack. Esc cancels."
                              : juce::String("Drop on another ") + (drag_.need == 'i' ? "IN" : "OUT") +
                                    " jack to move it, or on empty space to unplug.");
        }
        if (drag_.started) {
            drag_.p = toDev(c);
            repaint();
        }
        return;
    }
    if (kd_.active && picker_ != Picker::None) {
        // a knob scrolls the slot number: one slot per 6 px (24 px fine)
        const float per = (e.mods.isShiftDown() || shiftActive()) ? 24.f : 6.f;
        const int steps = static_cast<int>((pickDragY_ - c.y) / per);
        if (steps != 0) {
            pickStep(steps);
            pickDragY_ -= static_cast<float>(steps) * per;
        }
        return;
    }
    if (kd_.active) {
        const float range = (e.mods.isShiftDown() || shiftActive()) ? 1200.f : 220.f;
        setEnc(kd_.i, kd_.v0 + (kd_.y - c.y) / range);
        return;
    }
    if (mouseNote_) {
        const int key = keyAt(toDev(c));
        if (key < 0) {  // pointerleave from the keys releases the note
            mouseNote_ = false;
            noteOff();
        }
    }
}

void LiftPanel::mouseUp(const juce::MouseEvent& e) {
    const auto c = e.position;
    const auto d = toDev(c);
    if (menu_.open && menu_.bounds.contains(c)) {
        if (menu_.hover >= 0) {
            menuChoose(menu_.hover);
        }
        return;
    }
    if (drag_.active) {
        const Drag dr = drag_;
        drag_ = {};
        if (!dr.started) {
            clickJack(dr.from, c);
            repaint();
            return;
        }
        const JackId t = jackAt(d);
        if (!dr.isMove) {
            if (t.valid() && t.r == dr.need) {
                const int o = dr.need == 'o' ? t.i : dr.fixed.i;
                const int ii = dr.need == 'i' ? t.i : dr.fixed.i;
                const bool saveStack = stack_;
                stack_ = dr.st;
                const int saveColor = color_;
                addCord(o, ii, dr.c);
                stack_ = saveStack;
                color_ = saveColor;
            } else {
                say(t.valid() ? "An OUT goes to an IN. Nothing was patched." : "Nothing was patched.");
            }
        } else {
            Cord cd = cords_[static_cast<size_t>(dr.n)];
            if (t.valid() && t.r == dr.need) {
                const int o = dr.need == 'o' ? t.i : cd.o;
                const int ii = dr.need == 'i' ? t.i : cd.i;
                bool dup = false;
                for (size_t n = 0; n < cords_.size(); ++n) {
                    if (static_cast<int>(n) != dr.n && cords_[n].o == o && cords_[n].i == ii) {
                        dup = true;
                    }
                }
                if (dup) {
                    say("That pair is already patched. The cable stayed where it was.");
                } else if (blocked(t.r, t.i, dr.n)) {
                    say("That jack has a plug with a closed cap. The cable stayed where it was.");
                } else {
                    cords_.erase(cords_.begin() + dr.n);
                    cd.o = o;
                    cd.i = ii;
                    cords_.push_back(cd);
                    flash(juce::String("MOVED ") + kArrow + " " + jackName('o', o) + " " + kArrow + " " + jackName('i', ii));
                    say("Moved the cable: " + jackName('o', o) + " to " + jackName('i', ii) + ".");
                }
            } else if (t.valid() && !(t == dr.from)) {
                say(juce::String("That plug fits a ") + (dr.need == 'i' ? "blue IN" : "red OUT") +
                    " jack. The cable stayed where it was.");
            } else if (!t.valid()) {
                cords_.erase(cords_.begin() + dr.n);
                flash("UNPLUGGED " + jackName('o', cd.o) + " " + kArrow + " " + jackName('i', cd.i));
                say("Unplugged " + jackName('o', cd.o) + " from " + jackName('i', cd.i) + ".");
            }
        }
        publishPatch();
        repaint();
        return;
    }
    if (kd_.active) {
        kd_ = {};
        return;
    }
    if (mouseNote_) {
        mouseNote_ = false;
        noteOff();
        return;
    }
    // buttons act on click: press and release on the same control
    const int pad = padAt(d);
    const int mem = memAt(d);
    const int top = topAt(c);
    const int pp = pressedPad_, pm = pressedMem_, pt = pressedTop_;
    pressedPad_ = pressedMem_ = pressedTop_ = -1;
    if (pm == kShiftSlot) {
        const bool tap = t_ - shiftDownT_ < 0.3;
        shiftMouse_ = false;
        if (tap && mem == pm) {
            memAct(pm);  // toggles the latch
        } else {
            updateShift();
        }
        repaint();
        return;
    }
    if (pp >= 0 && pad == pp) {
        padAct(pad);
    } else if (pm >= 0 && mem == pm) {
        memAct(mem);
    } else if (pt >= 0 && top == pt) {
        topAct(top);
    }
    repaint();
}

void LiftPanel::mouseExit(const juce::MouseEvent&) {
    if (mouseNote_) {
        mouseNote_ = false;
        noteOff();
    }
}

void LiftPanel::mouseDoubleClick(const juce::MouseEvent& e) {
    const int k = knobAt(toDev(e.position));
    if (k >= 0) {
        kd_ = {};
        setEnc(k, 0.5f);
        say(labels()[k] + " back to center.");
    }
}

void LiftPanel::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const int k = knobAt(toDev(e.position));
    if (k < 0) {
        return;
    }
    // Shift+wheel arrives as a horizontal scroll on some systems.
    const float delta = w.deltaY != 0.f ? w.deltaY : w.deltaX;
    if (delta == 0.f) {
        return;
    }
    if (picker_ != Picker::None) {
        pickStep(delta > 0.f ? 1 : -1);
        return;
    }
    const float step = (e.mods.isShiftDown() || shiftActive()) ? 0.005f : 0.025f;
    setEnc(k, enc_[static_cast<size_t>(mode_)][static_cast<size_t>(k)] + (delta > 0.f ? step : -step));
}

// ---------------------------------------------------------------- keys

bool LiftPanel::keyPressed(const juce::KeyPress& key) {
    if (picker_ != Picker::None) {
        const int kc = key.getKeyCode();
        const juce::juce_wchar ch = key.getTextCharacter();
        const int big = key.getModifiers().isShiftDown() ? 10 : 1;
        if (kc == juce::KeyPress::escapeKey) {
            pickCancel();
        } else if (kc == juce::KeyPress::returnKey) {
            pickConfirm();
        } else if (kc == juce::KeyPress::upKey || kc == juce::KeyPress::rightKey) {
            pickStep(big);
        } else if (kc == juce::KeyPress::downKey || kc == juce::KeyPress::leftKey) {
            pickStep(-big);
        } else if (kc == juce::KeyPress::backspaceKey) {
            typed_ = typed_.dropLastCharacters(1);
            if (typed_.isNotEmpty()) {
                pickSlot_ = juce::jlimit(SlotStore::kFirst, SlotStore::kLast, typed_.getIntValue());
            }
            repaint(screenArea());
        } else if (ch >= '0' && ch <= '9') {
            typed_ = (typed_.length() >= 3 ? juce::String() : typed_) + juce::String::charToString(ch);
            const int v = typed_.getIntValue();
            if (v >= SlotStore::kFirst) {
                pickSlot_ = juce::jmin(SlotStore::kLast, v);
            }
            pickStepT_ = t_;
            repaint(screenArea());
        }
        return true;  // the picker keeps the keyboard while it is open
    }
    if (key == juce::KeyPress::escapeKey) {
        if (drag_.active) {
            drag_ = {};
            say("Cancelled.");
        } else if (kd_.active) {
            setEnc(kd_.i, kd_.v0);
            kd_ = {};
            say("Knob change cancelled.");
        } else if (menu_.open) {
            menu_.open = false;
        } else if (pick_.valid()) {
            pick_ = {};
            say("Cancelled.");
        }
        repaint();
        return true;
    }
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown()) {
        return false;
    }
    if (key.getKeyCode() == juce::KeyPress::tabKey) {
        focusKnob_ = mods.isShiftDown() ? (focusKnob_ <= 0 ? 3 : focusKnob_ - 1) : (focusKnob_ + 1) % 4;
        return true;
    }
    if (focusKnob_ >= 0) {
        const float step = (mods.isShiftDown() || shiftActive()) ? 0.01f : 0.05f;
        const float v = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(focusKnob_)];
        const int kc = key.getKeyCode();
        if (kc == juce::KeyPress::upKey || kc == juce::KeyPress::rightKey) {
            setEnc(focusKnob_, v + step);
            return true;
        }
        if (kc == juce::KeyPress::downKey || kc == juce::KeyPress::leftKey) {
            setEnc(focusKnob_, v - step);
            return true;
        }
        if (kc == juce::KeyPress::homeKey) {
            setEnc(focusKnob_, 0.f);
            return true;
        }
        if (kc == juce::KeyPress::endKey) {
            setEnc(focusKnob_, 1.f);
            return true;
        }
    }
    if (key == juce::KeyPress::spaceKey) {
        act("play");
        return true;
    }
    juce::juce_wchar ch = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    if (ch == ':') {
        ch = ';';  // shifted keys on a US layout
    } else if (ch == '"') {
        ch = '\'';
    }
    if (ch == 'z') {
        act("octd");
        return true;
    }
    if (ch == 'x') {
        act("octu");
        return true;
    }
    for (int n = 0; kMapKeys[n] != 0; ++n) {
        if (ch == static_cast<juce::juce_wchar>(kMapKeys[n])) {
            const int note = n;
            if (shiftActive()) {
                if (!key.isKeyCurrentlyDown(heldKeyCode_) || heldKeyCode_ != key.getKeyCode()) {
                    pressKey(note);
                    heldKeyCode_ = key.getKeyCode();
                }
                return true;
            }
            if (heldKeyCode_ != key.getKeyCode() || heldNote_ != n) {
                noteOn(n);
                heldKeyCode_ = key.getKeyCode();
            }
            return true;
        }
    }
    return false;
}

bool LiftPanel::keyStateChanged(bool isKeyDown) {
    if (!isKeyDown && heldKeyCode_ != 0 && !juce::KeyPress::isKeyCurrentlyDown(heldKeyCode_)) {
        noteOff();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- timer

void LiftPanel::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit(0.0, 0.1, (now - lastTick_) / 1000.0);
    lastTick_ = now;
    advance(dt);
}

}  // namespace lift
