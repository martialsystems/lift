// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// v3.1 panel behaviour that is not drawing: the pin matrix, the push-encoders'
// press / turn / hold gestures, the context hint line and the one history
// (undo) that covers cables, pins, keeps, drops and overdubs.

#include "LiftPanel.h"

#include "Fonts.h"
#include "PaintUtil.h"
#include "PanelData.h"

namespace lift {

using namespace ui;
using namespace draw;

namespace {
const char* const kPinNames[4] = {"OFF", "+100 %", "+50 %", "\xe2\x88\x92" "100 %"};
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");
}  // namespace

// ---------------------------------------------------------------- matrix

void LiftPanel::setPin(int r, int c, int k) {
    if (r < 0 || r > 15 || c < 0 || c > 15) {
        return;
    }
    k = juce::jlimit(0, 3, k);
    auto& p = pins_[static_cast<size_t>(r * 16 + c)];
    if (p == k) {
        return;
    }
    pushUndo();
    p = static_cast<std::uint8_t>(k);
    const juce::String route = juce::String::fromUTF8(MXR[r].n) + juce::String::fromUTF8(" \xe2\x86\x92 ") + juce::String::fromUTF8(MXC[c].n);
    flash(k ? "PIN " + route + " " + juce::String::fromUTF8(kPinNames[k]) : "PIN OUT " + route);
    say(k ? "Pin " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + r)) + juce::String(c + 1) + ": " + route +
                ", " + juce::String::fromUTF8(kPinNames[k]) + "."
          : "Pin removed: " + route + ".");
    hint("PIN " + route + kDot + juce::String::fromUTF8(kPinNames[k]));
    publishPatch();
    repaint();
}

void LiftPanel::clickPin(int r, int c) {
    setPin(r, c, (pin(r, c) + 1) % 4);
}

// ---------------------------------------------------------------- push-encoders

juce::String LiftPanel::encHint(int fn) const {
    switch (fn) {
    case 0: return "LIFT: TAP KEEPS WHAT YOU JUST HEARD" + kDot + "TURN = HOW FAR BACK" + kDot + "HOLD = SELECT";
    case 1: return "LOOP: TAP LOOPS THE TAPE" + kDot + "TURN = LOOP LENGTH";
    case 4: return "DROP: TAP PLACES THE KEPT SOUND" + kDot + "SHIFT+DROP = STRAIGHT TO THE LAST PLACE";
    case 3: return "REV: TAP REVERSES" + kDot + "TURN = REVERSE SPEED";
    case 2: return "SHIFT: TAP LATCHES, HOLD = MOMENTARY" + kDot + "SHIFT+TURN MOVES THE SELECTION";
    case 5: return "REC: TAP RECORDS WHAT YOU HEAR" + kDot + "TURN = REC LEVEL" + kDot + "HOLD = SELECT";
    case 6:
    case 8: return "OCT: TAP OR TURN STEPS THE KEYBOARD OCTAVE";
    case 7: return "PLAY: TAP PLAYS / PAUSES" + kDot + "TURN = NUDGE";
    default: return "STOP: TAP STOPS" + kDot + "TURN = STOP FADE";
    }
}

void LiftPanel::turnMk(int slot, float v) {
    slot = juce::jlimit(0, 9, slot);
    const int fn = MEM_FN[slot];
    const float prev = mk_[static_cast<size_t>(slot)];
    const float nv = juce::jlimit(0.f, 1.f, v);
    mk_[static_cast<size_t>(slot)] = nv;
    static const char* names[10] = {"LIFT LEN", "LOOP LEN", "SHIFT", "REV SPEED", "DROP FADE",
                                    "REC LVL",  "OCTAVE",   "NUDGE", "OCTAVE",    "STOP FADE"};
    if (fn == 6 || fn == 8) {
        // the octave knobs step the octave as they turn
        const int steps = static_cast<int>(nv * 7.f) - static_cast<int>(prev * 7.f);
        if (steps != 0) {
            oct_ = juce::jlimit(-3, 3, oct_ + steps);
        }
        flash("OCTAVE " + juce::String(oct_ > 0 ? "+" : "") + juce::String(oct_));
    } else if (fn == 0) {
        // LIFT: how far back the next lift reaches (1 .. 60 s of the capture buffer)
        flash("LIFT " + juce::String(liftSeconds(), 1) + " S BACK");
        proc_.liftBack.store(liftSeconds());
    } else if (fn == 5) {
        enc_[Tape][2] = nv;  // REC LVL is the TAPE screen's black knob too
        syncEngine();
        proc_.setKnobValue(Tape, 2, nv);
        flash(juce::String(names[fn]) + " " + juce::String(juce::roundToInt(nv * 99.f)).paddedLeft('0', 2));
    } else {
        flash(juce::String(names[fn]) + " " + juce::String(juce::roundToInt(nv * 99.f)).paddedLeft('0', 2));
    }
    if (selectOpen_ && (fn == 0 || fn == 5 || fn == 2)) {
        selectTurn(fn, nv - prev);
    }
    hint(encHint(fn));
    repaint();
}

float LiftPanel::liftSeconds() const {
    // 0..1 -> 1 .. 60 s, exponential (fine near the present)
    return 1.f * std::pow(60.f, mk_[0]);
}

void LiftPanel::memHold(int slot) {
    const int fn = MEM_FN[slot];
    if (fn == 0 || fn == 5) {
        openSelect(fn == 0);  // hold LIFT / REC opens SELECT on what would be kept
        return;
    }
    static const char* names[10] = {"LIFT", "LOOP", "SHIFT", "REV", "DROP", "REC", "OCT -", "PLAY", "OCT +", "STOP"};
    flash(juce::String(names[fn]) + " HELD");
    hint(encHint(fn));
}

// ---------------------------------------------------------------- hint line

juce::String LiftPanel::contextText() const {
    static const char* labelsM[5] = {"SYNTH", "DRUM", "TAPE", "MIX", "IN"};
    juce::String scr = bay_ ? "BAY" : labelsM[mode_];
    juce::String page;
    if (selectOpen_) {
        page = "SELECT";
    } else if (bay_) {
        page = "AUDIT";
    } else if (fxEdit_) {
        page = juce::String("FX ") + eng::kFxNames[fxType_];
    } else if (mode_ == Synth) {
        page = ENGINES[sel_[Synth]].n;
    } else if (mode_ == Drum) {
        page = KITS[sel_[Drum]];
    } else if (mode_ == Tape) {
        page = "LOOPER";
    } else if (mode_ == Mix) {
        page = "T" + juce::String(arm_ + 1);
    } else {
        page = INPUTS[sel_[In]];
    }
    return scr + kDot + page + kDot + (shiftActive() ? "B" : "A");
}

juce::String LiftPanel::hintText() const {
    if (hint_.isNotEmpty() && t_ - hintT_ < 6.0) {
        return hint_;
    }
    if (selectOpen_) {
        return juce::String("RED START") + kDot + "YELLOW LENGTH" + kDot + "BLUE ZOOM" + kDot + "BLACK SNAP" + kDot + "DROP PLACES";
    }
    if (bay_) {
        return juce::String("EVERYTHING PATCHED: CABLES, PINS, NORMALS");
    }
    const auto L = labels();
    return L[slotOf(0)] + " / " + L[slotOf(1)] + " / " + L[slotOf(2)] + " / " + L[slotOf(3)] + kDot + "TOUCH A CONTROL TO SEE WHAT IT DOES";
}

// One line at the foot of the view: the context (screen, page, knob side)
// and what the touched or held control does. Glass-local pixels.
void LiftPanel::paintHint(juce::Graphics& g) {
    const juce::Rectangle<float> band(0.f, 338.f, 600.f, 18.f);
    g.setColour(hex(0x050505, 0.92f));
    g.fillRect(band);
    g.setColour(hex(0x262626));
    g.fillRect(0.f, 338.f, 600.f, 1.f);
    const juce::Font f = mono(false, 9.f, 0.04f);
    const juce::String ctx = contextText();
    const float cw = cssWidth(700, true, 9.f, 0.04f, ctx);
    text(g, ctx, mono(true, 9.f, 0.04f), selectOpen_ ? hex(0xec4b3c) : hex(0xf4be2a), band.withX(16.f).withWidth(cw + 8.f),
         juce::Justification::centredLeft);
    text(g, hintText(), f, hex(0xa8a194), band.withX(16.f + cw + 14.f).withWidth(600.f - 46.f - cw),
         juce::Justification::centredLeft);
}

// ---------------------------------------------------------------- history

void LiftPanel::pushUndo(int audioOp) {
    HistEntry h;
    h.cords = cords_;
    h.pins = pins_;
    h.audioOp = audioOp;
    hist_.push_back(std::move(h));
    if (hist_.size() > 64) {
        hist_.erase(hist_.begin());
    }
}

// Undo, newest first. While SELECT is open its own edits go first; during an
// overdub pass undo drops that pass only (the processor handles it).
void LiftPanel::undo() {
    if (selectOpen_ && selectUndo()) {
        return;
    }
    if (proc_.overdubbing()) {
        proc_.send(Cmd::UndoPass);
        flash("UNDO PASS");
        hint("UNDO: THE CURRENT OVERDUB PASS IS DISCARDED");
        return;
    }
    if (hist_.empty()) {
        flash("NOTHING TO UNDO");
        return;
    }
    HistEntry h = std::move(hist_.back());
    hist_.pop_back();
    if (h.audioOp >= 0) {
        proc_.send(Cmd::UndoAudio, h.audioOp);
        flash("UNDO " + juce::String(proc_.audioOpName(h.audioOp)));
    } else {
        flash("UNDO PATCH");
    }
    cords_ = h.cords;
    pins_ = h.pins;
    publishPatch();
    repaint();
}

// ---------------------------------------------------------------- SELECT (stub until P3)

void LiftPanel::openSelect(bool lift) {
    selectOpen_ = true;
    flash(juce::String::fromUTF8(lift ? "SELECT \xc2\xb7 LIFT" : "SELECT \xc2\xb7 REC"));
}
void LiftPanel::selectTurn(int, float) {}
bool LiftPanel::selectUndo() { return false; }

}  // namespace lift

int lift::LiftPanel::slotOf(int k) const {
    return fxEdit_ ? k : lift::ui::knobSlot(mode_, sel_[0], k);
}

int lift::LiftPanel::physicalKnob(int slot) const {
    for (int k = 0; k < 4; ++k) {
        if (slotOf(k) == slot) {
            return k;
        }
    }
    return slot;
}
