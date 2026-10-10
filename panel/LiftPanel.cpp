// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "LiftPanel.h"
#include "PanelLayers.h"

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
    buildRopeScene();
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
    initLayers();
}

LiftPanel::~LiftPanel() {
    vblank_.reset();
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
    proc_.patch.publish(cords_, pins_, arm_);
}

juce::StringArray LiftPanel::labels() const {
    juce::StringArray a;
    for (int i = 0; i < 4; ++i) {
        a.add(juce::String::fromUTF8(fxEdit_ ? eng::kFxKnobNames[fxType_][i]
                                     : mode_ == Synth ? ENGINES[sel_[Synth]].m[i] : MODE_MACROS[mode_][i]));
    }
    return a;
}

float LiftPanel::encAt(int i) const {
    if (fxEdit_) {
        return fxKnobs_[static_cast<size_t>(fxType_)][static_cast<size_t>(i)];
    }
    return enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
}

void LiftPanel::selectDrumVoice(int v, bool announce) {
    v = juce::jlimit(0, eng::kDrumVoices - 1, v);
    const bool changed = v != drumVoice_;
    drumVoice_ = v;
    proc_.drumVoice.store(v);
    // the DRUM knobs show the selected voice: SLICE points at it, the rest are its own
    auto& e = enc_[static_cast<size_t>(Drum)];
    e[0] = (static_cast<float>(v) + 0.5f) / static_cast<float>(eng::kDrumVoices);
    for (int k = 0; k < 3; ++k) {
        e[static_cast<size_t>(k + 1)] = drumKnobs_[static_cast<size_t>(v)][static_cast<size_t>(k)];
    }
    if (announce && changed) {
        flash(juce::String("VOICE ") + eng::kDrumVoiceNames[v]);
    }
}

void LiftPanel::toggleStep(int step) {
    const int kit = juce::jlimit(0, eng::kKits - 1, sel_[Drum]);
    auto& m = drumPat_[static_cast<size_t>(kit)][static_cast<size_t>(drumVoice_)];
    m ^= 1u << step;
    proc_.drumPattern[kit * eng::kDrumVoices + drumVoice_].store(m);
    say(juce::String(eng::kDrumVoiceNames[drumVoice_]) + " step " + juce::String(step + 1) +
        (((m >> step) & 1u) ? " on." : " off."));
}

juce::String LiftPanel::encDisplay(int i) const {
    const float v = encAt(i);
    if (fxEdit_) {
        return juce::String(juce::roundToInt(v * 100.f)) + "%";
    }
    if (mode_ == Drum && i == 0) {
        return eng::kDrumVoiceNames[drumVoice_];
    }
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
            if (d.getDistanceFrom(jackCentre(rc, i)) <= kJackR + 3.f) {
                return {rc, i};
            }
        }
    }
    return {};
}

// The macro slot of the knob under the pointer (the knobs are remapped per
// screen by colour: knobSlot), -1 none.
int LiftPanel::knobAt(juce::Point<float> d) const {
    for (int i = 0; i < 4; ++i) {
        if (d.getDistanceFrom(knobCentre(i)) <= 70.f) {
            return slotOf(i);
        }
    }
    return -1;
}

int LiftPanel::pinAt(juce::Point<float> d) const {
    const float x = d.x - kMxX, y = d.y - kMxY;
    if (x < 0.f || y < 0.f) {
        return -1;
    }
    const int c = static_cast<int>(x / kMxW), r = static_cast<int>(y / kMxH);
    return c < 16 && r < 16 ? r * 16 + c : -1;
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
        if (d.getDistanceFrom(encCentre(k)) <= 25.f) {
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
    juce::ignoreUnused(c);
    pushUndo();
    cords_.push_back({o, i, cableCloth(o), stack_});
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
    if (dropPick_ && (k == "arm" || k == "mode")) {
        dropPick_ = false;
        if (k == "arm") {
            place(DestLoop + juce::jlimit(0, 3, n));
        } else if (arg == "in") {
            place(DestIn);
        } else if (arg == "synth") {
            place(DestKeys);
        } else if (arg == "drum") {
            place(DestDrum);
        }
        return;
    }
    dropPick_ = false;
    if (k == "mode" && selectOpen_) {
        closeSelect();
    }
    if (k == "mode") {
        fxEdit_ = false;
        mode_ = static_cast<Mode>(modeFromId(arg));
        bay_ = false;
        say(arg.toUpperCase() + " screen.");
    } else if (k == "fx") {
        if (fxEdit_) {
            fxEdit_ = false;
            flash(juce::String(eng::kFxNames[fxType_]) + " EDIT DONE");
        } else {
            fx_ = !fx_;
            proc_.fxOn.store(fx_);
            flash(juce::String(eng::kFxNames[fxType_]) + (fx_ ? " ON" : " BYPASSED"));
            say(fx_ ? "Effect insert on." : "Effect insert bypassed.");
        }
    } else if (k == "fxedit") {
        fxEdit_ = true;
        if (!fx_) {
            fx_ = true;
            proc_.fxOn.store(true);
        }
        flash(juce::String(eng::kFxNames[fxType_]) + juce::String::fromUTF8(" \xc2\xb7 KNOBS: ") +
              eng::kFxKnobNames[fxType_][0] + " " + eng::kFxKnobNames[fxType_][1] + " " + eng::kFxKnobNames[fxType_][2] +
              " " + eng::kFxKnobNames[fxType_][3]);
        say("Effect edit: the four knobs set the effect. Tap FX to finish.");
    } else if (k == "fxtype") {
        fxType_ = (fxType_ + 1) % eng::kFxTypes;
        proc_.send(Cmd::FxType, fxType_);
        for (int i = 0; i < 4; ++i) {
            proc_.fxKnobs[i].store(fxKnobs_[static_cast<size_t>(fxType_)][static_cast<size_t>(i)]);
        }
        flash(juce::String("FX ") + eng::kFxNames[fxType_]);
        say(juce::String("Effect: ") + eng::kFxNames[fxType_] + ".");
    } else if (k == "bay") {
        bay_ = !bay_;
        say(bay_ ? "Patch list on the screen." : "Patch list closed. Cables keep working.");
    } else if (k == "radio") {
        mode_ = In;
        sel_[In] = 2;
        bay_ = false;
        say("Radio dial. Encoder 1 picks the station.");
    } else if (k == "num" && selectOpen_) {
        selHist_.push_back(selc_);
        selc_.chop = n + 1;  // keys 1-8 chop the selection
        flash("CHOP " + juce::String(n + 1));
        repaint();
        return;
    } else if (k == "num") {
        if (mode_ == Synth && keysAt_ != nullptr) {
            keysAt_ = nullptr;  // picking an engine gives the keys back to the synth
            proc_.setKeysClip(nullptr);
        }
        const bool unused = mode_ == Mix || (mode_ == In && INPUTS[n][0] == 0);
        if (unused) {
            say("Keys 1 to 8 do nothing on this screen.");
            repaint();
            return;
        }
        sel_[static_cast<size_t>(mode_)] = n;
        if (mode_ == Synth) {
            proc_.synthEngine.store(n);
        } else if (mode_ == Drum) {
            proc_.drumKit.store(n);
        }
    } else if (k == "arm") {
        arm_ = n;
        proc_.send(Cmd::Arm, n);
        say("Track " + juce::String(n + 1) + " armed.");
    } else if (k == "mute") {
        mutes_[static_cast<size_t>(n)] = !mutes_[static_cast<size_t>(n)];
        proc_.send(Cmd::Mute, n, mutes_[static_cast<size_t>(n)] ? 1 : 0);
        say("Track " + juce::String(n + 1) + (mutes_[static_cast<size_t>(n)] ? " muted." : " unmuted."));
    } else if (k == "lift") {
        // LIFT keeps what you just heard: the last N s of the capture (LIFT encoder)
        if (selectOpen_) closeSelect();
        keep(proc_.keepLast(liftSeconds()), "LIFT " + juce::String(liftSeconds(), 1) + " S");
    } else if (k == "drop") {
        // DROP: the selection (the default one unless SELECT edited it) to the last place
        place(lastDest_, lastDestNote_);
    } else if (k == "dropnow") {
        // SHIFT+DROP, the fast commit: keep with the default selection and place
        // straight into the last place used, SELECT skipped
        if (!selectOpen_) {
            keep(proc_.keepLast(liftSeconds()), "LIFT");
        }
        place(lastDest_, lastDestNote_);
    } else if (k == "droppick") {
        dropPick_ = true;
        flash("DROP TO: T1-T4, A KEY, A DRUM KEY, SYNTH (KEYS) OR IN");
        hint("DROP: TAP WHERE IT GOES" + juce::String::fromUTF8(" \xc2\xb7 ") + "T PAD = LOOP, KEY = ONE SOUND, DRUM SCREEN KEY = SLICE KIT");
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
        if (selectOpen_) {
            closeSelect();
        }
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
    if (dropPick_) {
        dropPick_ = false;
        if (mode_ == Drum) {
            place(DestDrum);
        } else {
            place(DestKey, midiOf(n));
        }
        return;
    }
    if (selectOpen_ && clip_ != nullptr) {
        proc_.setAudition(eng::cutSelection(*clip_, selc_));  // keys audition the selection
    } else if (mode_ == Drum && drumAt_ != nullptr) {
        proc_.send(Cmd::SliceHit, ((n % 24) + 24) % 24, 100);  // the slice kit
        heldNote_ = n;
        note_ = n;
        repaint();
        return;
    }
    if (mode_ == Drum && !selectOpen_) {
        // DRUM: the keys play the kit's voices and pick the one the knobs drive;
        // with REC on and the tape running, the hit is written into the current step
        const int v = eng::drumVoiceForKey(((n % 24) + 24) % 24);
        if (v >= 0) {
            selectDrumVoice(v, true);
            proc_.send(Cmd::DrumHit, v, 100);
            if (rec_ && playing_) {
                const int step = proc_.uiDrumStep.load();
                const int kit = juce::jlimit(0, eng::kKits - 1, sel_[Drum]);
                if (step >= 0 && step < drumLen_) {
                    auto& m = drumPat_[static_cast<size_t>(kit)][static_cast<size_t>(v)];
                    m |= 1u << step;
                    proc_.drumPattern[kit * eng::kDrumVoices + v].store(m);
                }
            }
        }
        heldNote_ = n;
        note_ = n;
        repaint();
        return;
    }
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
    if (!selectOpen_ && !fxEdit_ && clip_ != nullptr && t_ - keptT_ < 3.0) {
        openSelect(0);  // a knob turned right after a keep opens SELECT on it
    }
    if (selectOpen_) {
        auto& e = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
        const float d = juce::jlimit(0.f, 1.f, v) - e;
        e = juce::jlimit(0.f, 1.f, v);  // the knob moves; the screen's engine value does not
        selectKnob(i, d);
        return;
    }
    if (fxEdit_) {
        auto& f = fxKnobs_[static_cast<size_t>(fxType_)][static_cast<size_t>(i)];
        f = juce::jlimit(0.f, 1.f, v);
        proc_.fxKnobs[i].store(f);
        flash(juce::String(eng::kFxNames[fxType_]) + juce::String::fromUTF8(" \xc2\xb7 ") + eng::kFxKnobNames[fxType_][i] +
              " " + juce::String(juce::roundToInt(f * 100.f)) + "%");
        repaint();
        return;
    }
    auto& e = enc_[static_cast<size_t>(mode_)][static_cast<size_t>(i)];
    e = juce::jlimit(0.f, 1.f, v);
    if (mode_ == Drum) {
        if (i == 0) {
            selectDrumVoice(juce::jmin(eng::kDrumVoices - 1, static_cast<int>(e * eng::kDrumVoices)), true);
            e = juce::jlimit(0.f, 1.f, v);  // the knob itself stays where it was turned
        } else {
            drumKnobs_[static_cast<size_t>(drumVoice_)][static_cast<size_t>(i - 1)] = e;
            proc_.send(Cmd::DrumKnob, drumVoice_, i - 1, e);
        }
    }
    if (learnArm_) {
        const int target = static_cast<int>(mode_) * 4 + i;
        if (proc_.learnTarget.exchange(target) != target) {
            flash("LEARN " + knobName(target) + ": MOVE A CONTROL");
        }
    }
    // straight to the audio thread, which smooths it (no zipper noise)
    proc_.setKnobValue(static_cast<int>(mode_), i, e);
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
        return;  // v3.1: no colour swatches (a cable takes its source's colour)
    }
    if (k == 6) {
        stack_ = !stack_;
        pick_ = {};
        say(stack_ ? "Stackable cables on. Pick a color and drag: the plugs have an open jack on top, so another "
                     "cable can plug into them."
                   : "Stackable cables off. New cables get closed caps.");
    } else if (k == 7) {
        pushUndo();
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
    // P4 jack options: REC presses LIFT, QUANT scale, DRUM pulse length
    auto option = [&](int n, const juce::String& label) {
        MenuItem it;
        it.kind = 3;
        it.n = n;
        it.hasDot = false;
        it.label = label;
        menu_.items.push_back(it);
    };
    if (j.r == 'i' && j.i == eng::I_REC) {
        option(0, recJackLifts_ ? "This jack presses REC" : "This jack presses LIFT");
    } else if (j.r == 'i' && j.i == eng::I_QUANT) {
        for (int s = 0; s < 5; ++s) {
            option(10 + s, juce::String("Scale: ") + eng::kQuantScaleNames[s] + (proc_.quantScale.load() == s ? juce::String::fromUTF8("  \xe2\x9c\x93") : juce::String()));
        }
    } else if (j.r == 'o' && j.i == eng::O_DRUM) {
        static const int ms[4] = {2, 10, 50, 200};
        for (int s = 0; s < 4; ++s) {
            option(20 + s, "Pulse length " + juce::String(ms[s]) + " ms" +
                               (std::abs(proc_.drumGateMs.load() - static_cast<float>(ms[s])) < 0.5f ? juce::String::fromUTF8("  \xe2\x9c\x93") : juce::String()));
        }
    }
    if (here.empty()) {
        if (menu_.items.empty()) {
            menu_.open = false;
            return;
        }
    } else if (cords_[static_cast<size_t>(here.back())].st) {
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
    if (it.kind == 3) {
        if (it.n == 0) {
            recJackLifts_ = !recJackLifts_;
            proc_.recJackLifts.store(recJackLifts_);
            flash(recJackLifts_ ? "REC JACK PRESSES LIFT" : "REC JACK PRESSES REC");
        } else if (it.n >= 10 && it.n < 15) {
            proc_.quantScale.store(it.n - 10);
            flash(juce::String("QUANT ") + eng::kQuantScaleNames[it.n - 10]);
        } else if (it.n >= 20) {
            static const float ms[4] = {2.f, 10.f, 50.f, 200.f};
            proc_.drumGateMs.store(ms[it.n - 20]);
            flash("DRUM PULSE " + juce::String(juce::roundToInt(ms[it.n - 20])) + " MS");
        }
        menu_.open = false;
        repaint();
        return;
    }
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
    const auto c = toCanvas(e.position);
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
    trackRopes(d, true);
    {
        const int hv = pinAt(d);
        if (hv != mxHover_) {
            mxHover_ = hv;
            if (hv >= 0) {
                const int r = hv / 16, cc = hv % 16;
                hint(juce::String::fromUTF8(MXR[r].n) + " " + kArrow + " " + juce::String::fromUTF8(MXC[cc].n) +
                     juce::String::fromUTF8(" Â· CLICK: +100 / +50 / â" "100 / OFF"));
            }
            refreshParts();
        }
    }
    const JackId j = jackAt(d);
    if (j.valid()) {
        setMouseCursor(cordsAt(j.r, j.i).empty() ? juce::MouseCursor::CrosshairCursor
                                                  : juce::MouseCursor::DraggingHandCursor);
    } else if (knobAt(d) >= 0) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    } else if (memAt(d) >= 0) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    } else if (padAt(d) >= 0 || keyAt(d) >= 0 || topAt(c) >= 0 || pinAt(d) >= 0 ||
               (menu_.open && menu_.bounds.contains(c))) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    } else {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void LiftPanel::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    const auto c = toCanvas(e.position);
    const auto d = toDev(c);
    if (e.mods.isPopupMenu() && addSizeItems && !menu_.open && !jackAt(d).valid()) {
        juce::PopupMenu m;
        addSizeItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
        return;
    }
    if (e.mods.isPopupMenu() && !menu_.open && jackAt(d).valid()) {
        // right-click a jack: its menu (cables, and the P4 jack options)
        pick_ = {};
        openMenu(jackAt(d), c);
        repaint();
        return;
    }
    if (menu_.open) {
        if (menu_.bounds.contains(c)) {
            return;  // chosen on mouse up (a click)
        }
        menu_.open = false;
        repaint();
    }
    if (mode_ == Drum && !bay_ && picker_ == Picker::None && shiftAmt_ <= 0.f) {
        // DRUM screen: click a step cell to toggle it for the selected voice
        const float sc = 600.f / 720.f;
        const float vx = (d.x - 60.f - kMainDX) / sc;
        const float vy = (d.y - 256.f - kMainDY - (32.f + (324.f - 319.f * sc) * 0.5f)) / sc - 40.f;
        if (vy >= 140.f && vy <= 200.f && vx >= 24.f) {
            const int i = static_cast<int>((vx - 24.f) / 42.4f);
            if (i >= 0 && i < 16 && vx - 24.f - static_cast<float>(i) * 42.4f <= 38.f) {
                toggleStep(i);
                repaint(screenArea());
                return;
            }
        }
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
    const int pc = pinAt(d);
    if (pc >= 0) {
        if (e.mods.isPopupMenu()) {
            setPin(pc / 16, pc % 16, 0);
        } else {
            clickPin(pc / 16, pc % 16);
        }
        return;
    }
    const int k = knobAt(d);
    if (k >= 0) {
        hint(labels()[k] + juce::String::fromUTF8(" Â· DRAG UP / DOWN Â· SHIFT FINE Â· DOUBLE-CLICK CENTRE"));
        focusKnob_ = k;
        kd_ = {true, k, c.y, encAt(k)};
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
    if (pressedPad_ == 5) {
        fxDownT_ = t_;
    }
    pressedMem_ = memAt(d);
    if (pressedMem_ >= 0) {
        // push-encoder: press, turn (drag), hold, hold + turn
        mkd_ = {};
        mkd_.active = true;
        mkd_.slot = pressedMem_;
        mkd_.y = c.y;
        mkd_.v0 = mk_[static_cast<size_t>(pressedMem_)];
        mkd_.t0 = t_;
        hint(encHint(MEM_FN[pressedMem_]));
    }
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
    const auto c = toCanvas(e.position);
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
            refreshParts();  // the cable layer and jack highlights only; the screen is not touched
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
    if (mkd_.active) {
        const float dy = mkd_.y - c.y;
        if (!mkd_.turned && std::abs(dy) > 4.f) {
            mkd_.turned = true;
            mkd_.held = t_ - mkd_.t0 >= 0.45;  // hold + turn
        }
        if (mkd_.turned) {
            turnMk(mkd_.slot, mkd_.v0 + dy / (e.mods.isShiftDown() ? 900.f : 160.f));
        }
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
    const auto c = toCanvas(e.position);
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
    const MkDrag md = mkd_;
    mkd_ = {};
    if (md.active && md.turned && md.slot != kShiftSlot) {
        pressedMem_ = -1;  // a turn is not a press
        repaint();
        return;
    }
    if (md.active && !md.turned && md.slot != kShiftSlot && t_ - md.t0 >= 0.45) {
        pressedMem_ = -1;
        memHold(md.slot);  // hold
        repaint();
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
    if (pp == 5 && pad == pp) {
        // FX: tap = on / bypass, SHIFT + tap = next effect, hold = the knobs edit the effect
        if (shiftActive()) {
            act("fxtype");
        } else if (fxDownT_ >= 0.0 && t_ - fxDownT_ >= 0.5) {
            act("fxedit");
        } else {
            act("fx");
        }
        fxDownT_ = -1.0;
    } else if (pp >= 0 && pad == pp) {
        padAct(pad);
    } else if (pm >= 0 && mem == pm) {
        memAct(mem);
    } else if (pt >= 0 && top == pt) {
        topAct(top);
    }
    repaint();
}

void LiftPanel::mouseExit(const juce::MouseEvent&) {
    trackRopes({}, false);
    if (mxHover_ >= 0) {
        mxHover_ = -1;
        refreshParts();
    }
    if (mouseNote_) {
        mouseNote_ = false;
        noteOff();
    }
}

void LiftPanel::mouseDoubleClick(const juce::MouseEvent& e) {
    const int k = knobAt(toDev(toCanvas(e.position)));
    if (k >= 0) {
        kd_ = {};
        setEnc(k, 0.5f);
        say(labels()[k] + " back to center.");
    }
}

void LiftPanel::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const int k = knobAt(toDev(toCanvas(e.position)));
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
    const bool fine = e.mods.isShiftDown() || shiftActive();
    const float now = encAt(k);
    if (w.isSmooth) {
        // trackpad / smooth wheel: proportional to the scroll distance, so the
        // knob follows the fingers continuously (a full turn over ~1.6 pages)
        if (w.isInertial) {
            return;  // no run-on after the fingers lift
        }
        setEnc(k, now + delta * (fine ? 0.15f : 0.6f));
        return;
    }
    const float step = fine ? 0.005f : 0.025f;  // one notch
    setEnc(k, now + (delta > 0.f ? step : -step));
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
        const float v = encAt(focusKnob_);
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

}  // namespace lift
