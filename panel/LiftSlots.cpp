// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Save slots, state sync with the processor and MIDI learn for the panel.
//
// The processor owns the saved state (UiState plus the tape). The panel
// mirrors it: it pushes its own changes once per animation frame and takes
// the processor's state back when a slot or host state is loaded. MIDI knob
// CCs and MIDI learn arrive through the processor's Listener on the message
// thread, so the two never race.

#include "LiftPanel.h"

#include "PaintUtil.h"

#include <cmath>

namespace lift {

using namespace ui;
using namespace draw;

namespace {
float clamp01Slots(float v) {
    return juce::jlimit(0.f, 1.f, v);
}
juce::String slotName(int slot) {
    return juce::String(slot).paddedLeft('0', 3);
}
}  // namespace

// ------------------------------------------------------------------ state

UiState LiftPanel::captureUi() const {
    UiState s;
    s.mode = static_cast<int>(mode_);
    s.bay = bay_;
    s.fx = fx_;
    s.sel = sel_;
    s.enc = enc_;
    s.loop = loop_;
    s.loopIn = loopIn_;
    s.loopOut = loopOut_;
    s.marks = marks_;
    s.nextMark = nextMark_;
    s.arm = arm_;
    s.mutes = mutes_;
    s.oct = oct_;
    s.transpose = transpose_;
    s.seqDiv = seqDiv_;
    s.drumDiv = drumDiv_;
    s.drumLen = drumLen_;
    s.swing = swing_;
    s.recSource = recSource_;
    s.character = character_;
    s.cassette = cassette_;
    s.color = color_;
    s.stack = stack_;
    s.cords = cords_;
    s.pins = pins_;
    s.recJackLifts = recJackLifts_;
    s.learn = learn_;
    s.drumPat = drumPat_;
    s.drumVoice = drumVoice_;
    s.drumKnobs = drumKnobs_;
    s.fxType = fxType_;
    s.fxKnobs = fxKnobs_;
    return s;
}

void LiftPanel::applyUi(const UiState& s) {
    mode_ = static_cast<Mode>(juce::jlimit(0, 4, s.mode));
    bay_ = s.bay;
    fx_ = s.fx;
    sel_ = s.sel;
    enc_ = s.enc;
    loop_ = s.loop;
    loopIn_ = s.loopIn;
    loopOut_ = s.loopOut;
    marks_ = s.marks;
    nextMark_ = s.nextMark;
    arm_ = s.arm;
    mutes_ = s.mutes;
    oct_ = s.oct;
    transpose_ = s.transpose;
    seqDiv_ = s.seqDiv;
    drumDiv_ = s.drumDiv;
    drumLen_ = s.drumLen;
    swing_ = s.swing;
    recSource_ = s.recSource;
    character_ = s.character;
    cassette_ = s.cassette;
    color_ = s.color;
    stack_ = s.stack;
    cords_ = s.cords;
    pins_ = s.pins;
    recJackLifts_ = s.recJackLifts;
    ropes_.clear();
    learn_ = s.learn;
    drumPat_ = s.drumPat;
    drumKnobs_ = s.drumKnobs;
    fxType_ = s.fxType;
    fxKnobs_ = s.fxKnobs;
    fxEdit_ = false;
    drumVoice_ = s.drumVoice;
    proc_.drumVoice.store(drumVoice_);
    pick_ = {};
    menu_.open = false;
    publishPatch();
    syncEngine();
    repaint();
}

void LiftPanel::pushUi() {
    UiState s = captureUi();
    if (!(s == lastPushed_)) {
        proc_.setUiState(s);
        lastPushed_ = std::move(s);
    }
}

void LiftPanel::stateLoaded(int slot) {
    // a load stops the tape; the panel's transport keys follow
    playing_ = rec_ = rev_ = false;
    noteOff();
    applyUi(proc_.uiState());
    lastPushed_ = captureUi();
    if (slot > 0) {
        flash("LOADED SLOT " + slotName(slot));
    }
}

void LiftPanel::knobFromMidi(int target, float value) {
    const int screen = target / 4, k = target % 4;
    if (screen == static_cast<int>(mode_)) {
        const bool arm = learnArm_;
        learnArm_ = false;  // a controller moving a knob is not a touch to learn
        setEnc(k, value);
        learnArm_ = arm;
    } else {
        enc_[static_cast<size_t>(screen)][static_cast<size_t>(k)] = value;
        if (screen == Tape) {
            syncEngine();
            if (k == 3) {
                proc_.send(Cmd::Seek, 0, 0, static_cast<double>(value) * static_cast<double>(proc_.uiFrames.load()));
            }
        }
    }
    pushUi();
}

void LiftPanel::learned(int cc, int target) {
    for (auto& t : learn_) {
        if (t == target) {
            t = -1;
        }
    }
    learn_[static_cast<size_t>(cc)] = target;
    learnArm_ = false;
    pushUi();
    flash("CC " + juce::String(cc) + " " + juce::String::fromUTF8("\xe2\x86\x92 ") + knobName(target));
    repaint();
}

juce::String LiftPanel::knobName(int target) const {
    static const char* screens[5] = {"SYNTH", "DRUM", "TAPE", "MIX", "IN"};
    const int screen = juce::jlimit(0, 4, target / 4), k = juce::jlimit(0, 3, target % 4);
    const juce::String name = juce::String::fromUTF8(screen == Synth ? ENGINES[sel_[Synth]].m[k] : MODE_MACROS[screen][k]);
    return juce::String(screens[screen]) + " " + name;
}

// ------------------------------------------------------------------ picker

void LiftPanel::openPicker(Picker p) {
    pushUi();
    if (picker_ == Picker::None) {
        pickSlot_ = proc_.currentSlot() > 0 ? proc_.currentSlot() : juce::jmax(1, proc_.slots().lastSlot());
        pickT_ = t_;
    }
    picker_ = p;
    typed_.clear();
    pickStepT_ = t_;
    // the legend gives way to the picker
    shiftLatch_ = false;
    updateShift();
    repaint(screenArea());
}

void LiftPanel::pickStep(int delta) {
    if (picker_ == Picker::None || delta == 0) {
        return;
    }
    // wraps 999 -> 001
    const int span = SlotStore::kLast - SlotStore::kFirst + 1;
    pickSlot_ = ((pickSlot_ - SlotStore::kFirst + delta) % span + span) % span + SlotStore::kFirst;
    typed_.clear();
    pickStepT_ = t_;
    repaint(screenArea());
}

void LiftPanel::pickConfirm() {
    const Picker p = picker_;
    picker_ = Picker::None;
    typed_.clear();
    if (p == Picker::Save) {
        pushUi();
        juce::Component::SafePointer<LiftPanel> self(this);
        const int slot = pickSlot_;
        proc_.saveSlotAsync(slot, [self, slot](bool ok) {
            if (self != nullptr) {
                self->flash(ok ? "SAVED SLOT " + slotName(slot) : "SAVE FAILED");
                self->repaint();
            }
        });
    } else if (p == Picker::Load) {
        const auto r = proc_.loadSlot(pickSlot_);
        if (r == LiftProcessor::LoadResult::Empty) {
            flash("SLOT " + slotName(pickSlot_) + " IS EMPTY");
        } else if (r == LiftProcessor::LoadResult::NewerVersion) {
            flash("SLOT " + slotName(pickSlot_) + " IS FROM A NEWER LIFT");
        } else if (r != LiftProcessor::LoadResult::Ok) {
            flash("SLOT " + slotName(pickSlot_) + " WILL NOT LOAD");
        }
    }
    repaint();
}

void LiftPanel::pickCancel() {
    picker_ = Picker::None;
    typed_.clear();
    flash("CANCELLED");
    repaint();
}

// Drawn over the view in the legend's 720-wide space, in the screen's own
// type and palette.
void LiftPanel::paintPicker(Graphics& g) {
    if (picker_ == Picker::None) {
        return;
    }
    const float since = static_cast<float>(t_ - pickT_);
    const float in = reducedMotion_ ? 1.f : clamp01Slots(since / 0.12f);
    const float a = in * in * (3.f - 2.f * in);
    g.setColour(hex(0x0b0b0b, 0.96f * a));
    g.fillRect(-10.f, -60.f, 740.f, 420.f);
    const Colour cr = hex(0xede6d6, a), grey = hex(0x8c877b, a), yel = hex(0xf4be2a, a), dim = hex(0x5e5a50, a),
                 red = hex(0xe8473a, a);
    const bool save = picker_ == Picker::Save;
    const float dy = reducedMotion_ ? 0.f : 12.f * (1.f - easeOutBack(clamp01Slots(since / 0.2f)));
    Graphics::ScopedSaveState s(g);
    g.addTransform(AffineTransform::translation(0.f, dy));
    svgText(g, save ? "SAVE" : "LOAD", 24.f, 26.f, 18.f, save ? red : yel, -1, true);
    svgText(g, "SLOT", 24.f + 62.f, 26.f, 18.f, cr, -1, true);
    const int cur = proc_.currentSlot();
    svgText(g, cur > 0 ? "CURRENT " + slotName(cur) : "NOTHING SAVED YET", 696.f, 24.f, 9.f, grey, 1, false, 1.f);

    // neighbours either side, the picked slot big in the middle; it kicks on every step
    const float kk = reducedMotion_ ? 0.f : kick(t_ - pickStepT_, 22.f, 10.f);
    for (int d = -2; d <= 2; ++d) {
        if (d == 0) {
            continue;
        }
        const int span = SlotStore::kLast;
        const int n = ((pickSlot_ - 1 + d) % span + span) % span + 1;
        const float x = 360.f + static_cast<float>(d) * 150.f + (d < 0 ? -20.f : 20.f);
        const bool used = proc_.slots().exists(n);
        svgText(g, slotName(n), x, 146.f, 26.f, (used ? cr : dim).withMultipliedAlpha(std::abs(d) == 1 ? 0.7f : 0.35f), 0,
                false);
    }
    {
        Graphics::ScopedSaveState ks(g);
        g.addTransform(AffineTransform::scale(1.f + 0.08f * kk, 1.f - 0.08f * kk, 360.f, 130.f));
        const bool used = proc_.slots().exists(pickSlot_);
        svgText(g, slotName(pickSlot_), 360.f, 162.f, 72.f, used ? cr : grey, 0, true);
        if (typed_.isNotEmpty()) {
            const float w = 0.6f * 72.f * 3.f;
            g.setColour(yel);
            g.fillRect(360.f - w * 0.5f, 174.f, w, 3.f);
        }
    }
    const bool used = proc_.slots().exists(pickSlot_);
    juce::String state;
    if (used) {
        state = "USED " + juce::String::fromUTF8("\xc2\xb7 ") +
                proc_.slots().modified(pickSlot_).formatted("%Y-%m-%d %H:%M").toUpperCase();
        if (save) {
            state += juce::String::fromUTF8(" \xc2\xb7 SAVING REPLACES IT");
        }
    } else {
        state = save ? "EMPTY" : "EMPTY " + juce::String::fromUTF8("\xc2\xb7 NOTHING TO LOAD");
    }
    svgText(g, state, 360.f, 214.f, 11.f, used && save ? yel : grey, 0, false, 1.f);
    svgText(g, juce::String::fromUTF8("OCT \xe2\x88\x92 / OCT + , A KNOB OR DIGITS PICK"), 24.f, 270.f, 9.f, grey, -1, false, 1.f);
    svgText(g, juce::String::fromUTF8(save ? "PLAY SAVES  \xc2\xb7  STOP CANCELS" : "PLAY LOADS  \xc2\xb7  STOP CANCELS"), 696.f, 270.f, 9.f, cr, 1,
            false, 1.f);
    svgText(g, "TAPE, KNOBS, KEYS, PATCH AND SHIFT SETTINGS", 24.f, 300.f, 9.f, dim, -1, false, 1.f);
}

}  // namespace lift
