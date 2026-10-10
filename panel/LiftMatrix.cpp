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
        openSelect(fn == 0 ? 1 : 2);  // hold LIFT / REC opens SELECT on what would be kept
        return;
    }
    if (fn == 4) {
        if (shiftActive()) {
            openPicker(Picker::Save);  // SHIFT + hold DROP: save to a slot
        } else {
            act("droppick");  // hold DROP: pick the place
        }
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
    // the pre-keep meter: loudness of what LIFT would keep now, against the
    // keep target (the trim stage levels it there)
    const float lu = proc_.preKeepLufs(liftSeconds());
    const juce::Rectangle<float> meter(600.f - 132.f, 338.f, 120.f, 18.f);
    {
        const float x0 = meter.getX() + 46.f, w = 70.f;
        g.setColour(hex(0x1c1c1c));
        g.fillRect(x0, 344.f, w, 6.f);
        const float fr = juce::jlimit(0.f, 1.f, (lu + 40.f) / 40.f);  // -40 .. 0 LUFS
        g.setColour(lu > eng::kTargetLufs + 3.f ? hex(0xec4b3c) : hex(0x6fbf5a));
        g.fillRect(x0, 344.f, w * fr, 6.f);
        const float tx = x0 + w * (eng::kTargetLufs + 40.f) / 40.f;
        g.setColour(hex(0xf4be2a));
        g.fillRect(tx - 0.5f, 342.f, 1.f, 10.f);
        text(g, lu > -100.f ? juce::String(juce::roundToInt(lu)) : juce::String("--"), mono(true, 9.f, 0.04f), hex(0xa8a194),
             meter.withWidth(44.f), juce::Justification::centredRight);
    }
    text(g, hintText(), f, hex(0xa8a194), band.withX(16.f + cw + 14.f).withWidth(600.f - 46.f - cw - 120.f),
         juce::Justification::centredLeft);
}

// The SELECT view, over whatever screen was up: a red banner (SELECT is open),
// the kept audio's waveform (what was heard, post chain), the selection
// (red start, yellow length), snap (black) and zoom (blue), chop lines.
void LiftPanel::paintSelectBanner(juce::Graphics& g) {
    const juce::Rectangle<float> b(0.f, 32.f, 600.f, 22.f);
    g.setColour(hex(0xd8382b));
    g.fillRect(b);
    juce::String t = juce::String("SELECT OPEN") + kDot + (selectFromLift_ ? "LIFT" : "REC") + kDot +
                     (clip_ != nullptr && clip_->loop ? "LOOP" : "HIT") + kDot + "DROP PLACES" + kDot + "UNDO = LAST EDIT";
    text(g, t, mono(true, 10.f, 0.06f), hex(0xfff6e8), b.reduced(12.f, 0.f), juce::Justification::centredLeft);
}

void LiftPanel::paintViewSelect(juce::Graphics& g) {
    g.setColour(hex(0x0b0b0b));
    g.fillRect(0.f, 32.f, 600.f, 306.f);
    paintSelectBanner(g);
    if (clip_ == nullptr) {
        return;
    }
    const int n = clip_->frames();
    const double sr = static_cast<double>(::kSampleRate);
    // the view: zoomed around the selection
    const double viewLen = juce::jmax(64.0, n * std::pow(1.0 / 64.0, static_cast<double>(selZoom_)));
    const double mid = selc_.start + 0.5 * selc_.length;
    const double v0 = juce::jlimit(0.0, juce::jmax(0.0, n - viewLen), mid - 0.5 * viewLen);
    const juce::Rectangle<float> wr(16.f, 70.f, 568.f, 200.f);
    const int W = static_cast<int>(wr.getWidth());
    const juce::int64 key = (static_cast<juce::int64>(clip_->id) << 40) ^ (static_cast<juce::int64>(v0) << 8) ^
                            static_cast<juce::int64>(viewLen);
    if (key != waveKey_ || wavePk_.size() != static_cast<size_t>(W * 2)) {
        wavePk_.assign(static_cast<size_t>(W * 2), 0.f);
        for (int x = 0; x < W; ++x) {
            const int a = static_cast<int>(v0 + viewLen * x / W), b = juce::jmax(a + 1, static_cast<int>(v0 + viewLen * (x + 1) / W));
            float lo = 0.f, hi = 0.f;
            const int step = juce::jmax(1, (b - a) / 64);  // at most 64 reads a column
            for (int i = a; i < b && i < n; i += step) {
                const float m = 0.5f * (clip_->l[static_cast<size_t>(i)] + clip_->r[static_cast<size_t>(i)]);
                lo = juce::jmin(lo, m);
                hi = juce::jmax(hi, m);
            }
            wavePk_[static_cast<size_t>(2 * x)] = lo;
            wavePk_[static_cast<size_t>(2 * x + 1)] = hi;
        }
        waveKey_ = key;
    }
    auto fx = [&](double f) { return wr.getX() + static_cast<float>((f - v0) / viewLen) * wr.getWidth(); };
    const float s0 = fx(selc_.start), s1 = fx(selc_.start + selc_.length);
    g.setColour(hex(0xf2b705, 0.16f));
    g.fillRect(juce::Rectangle<float>(juce::jmax(wr.getX(), s0), wr.getY(), juce::jmin(wr.getRight(), s1) - juce::jmax(wr.getX(), s0), wr.getHeight()));
    const float cy = wr.getCentreY(), hh = wr.getHeight() * 0.5f;
    for (int x = 0; x < W; ++x) {
        const float px = wr.getX() + static_cast<float>(x);
        const bool in = px >= s0 && px <= s1;
        g.setColour(in ? hex(0xede6d6) : hex(0x5a5650));
        const float lo = wavePk_[static_cast<size_t>(2 * x)], hi = wavePk_[static_cast<size_t>(2 * x + 1)];
        g.fillRect(px, cy - hi * hh, 1.f, juce::jmax(1.f, (hi - lo) * hh));
    }
    // chop lines
    if (selc_.chop > 1) {
        g.setColour(hex(0xf2b705, 0.6f));
        for (int k = 1; k < selc_.chop; ++k) {
            const float x = fx(selc_.start + static_cast<double>(selc_.length) * k / selc_.chop);
            if (x > wr.getX() && x < wr.getRight()) g.fillRect(x, wr.getY(), 1.f, wr.getHeight());
        }
    }
    g.setColour(hex(0xd8382b));
    if (s0 >= wr.getX() && s0 <= wr.getRight()) g.fillRect(s0 - 1.f, wr.getY() - 6.f, 2.f, wr.getHeight() + 12.f);
    g.setColour(hex(0xf2b705));
    if (s1 >= wr.getX() && s1 <= wr.getRight()) g.fillRect(s1 - 1.f, wr.getY() - 6.f, 2.f, wr.getHeight() + 12.f);
    // the four knob readouts, in knob colours
    const juce::String vals[4] = {
        "START " + juce::String(selc_.start / sr, 3) + " S",
        "LENGTH " + juce::String(selc_.length / sr, 3) + " S",
        "ZOOM " + juce::String(juce::roundToInt(n / viewLen)) + "X",
        "SNAP " + juce::String(eng::kSnapNames[selc_.snap]) + (selc_.chop > 1 ? kDot + "CHOP " + juce::String(selc_.chop) : juce::String())};
    const juce::uint32 cols[4] = {0xec4b3c, 0xf4be2a, 0x4f86d8, 0xd8d2c4};
    for (int k = 0; k < 4; ++k) {
        text(g, vals[k], mono(true, 10.f, 0.04f), hex(cols[k]), juce::Rectangle<float>(16.f + 142.f * k, 288.f, 140.f, 18.f),
             juce::Justification::centredLeft);
    }
    text(g, juce::String("KEPT ") + juce::String(n / sr, 1) + " S" + kDot + juce::String(clip_->lufsIn, 1) + " LUFS IN" + kDot +
                "TRIM " + juce::String(clip_->gainDb, 1) + " DB" + kDot + "KEYS AUDITION" + kDot + "1-8 CHOP",
         mono(false, 9.f, 0.04f), hex(0x8f897d), juce::Rectangle<float>(16.f, 310.f, 568.f, 16.f), juce::Justification::centredLeft);
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

// Undo, newest first, over one history: cables, pins, keeps, drops and
// overdub passes. While SELECT is open its own edits go first; during an
// overdub pass undo discards that pass only (and stops recording).
void LiftPanel::undo() {
    if (selectOpen_ && selectUndo()) {
        return;
    }
    if (proc_.overdubbing()) {
        const int pass = proc_.uiPassCount.load();
        proc_.send(Cmd::UndoPass, 0, arm_);
        rec_ = false;
        wasRec_ = false;  // no REC keep for a discarded pass
        if (!hist_.empty() && hist_.back().audioOp == 2 && hist_.back().pass == pass) {
            hist_.pop_back();
        }
        flash("UNDO PASS");
        hint("UNDO: THE CURRENT OVERDUB PASS IS DISCARDED, EARLIER PASSES STAY");
        repaint();
        return;
    }
    if (hist_.empty()) {
        flash("NOTHING TO UNDO");
        return;
    }
    HistEntry h = std::move(hist_.back());
    hist_.pop_back();
    if (h.audioOp == 0) {  // KEEP: the clip before comes back
        clip_ = h.prevClip;
        selc_ = h.prevSel;
        if (selectOpen_) closeSelect();
        flash("UNDO KEEP");
    } else if (h.audioOp == 1) {  // DROP
        if (h.dest < DestKeys) {
            proc_.send(Cmd::UndoDrop);
        } else if (h.dest == DestKeys) {
            keysAt_ = h.prevClip;
            proc_.setKeysClip(keysAt_);
        } else if (h.dest == DestKey && h.note >= 0) {
            keyAt_[static_cast<size_t>(h.note)] = h.prevClip;
            proc_.setKeyClip(h.note, h.prevClip);
        } else if (h.dest == DestDrum) {
            drumAt_ = h.prevClip;
            proc_.setSliceKit(drumAt_, drumAt_ != nullptr ? 24 : 0);
        } else if (h.dest == DestIn) {
            // the pool keeps it (append only); nothing plays it yet
        }
        flash("UNDO DROP");
    } else if (h.audioOp == 2) {  // a finished overdub pass
        proc_.send(Cmd::UndoPass, h.pass, h.track);
        flash("UNDO OVERDUB T" + juce::String(h.track + 1));
    } else {
        flash("UNDO PATCH");
    }
    cords_ = h.cords;
    pins_ = h.pins;
    publishPatch();
    repaint();
}

// ---------------------------------------------------------------- keep / place

// Every keep: trimmed in the processor (loudness to target, true-peak safe),
// default selection attached (the SELECT fast path), one history entry.
bool LiftPanel::keep(LiftProcessor::ClipPtr c, const juce::String& what) {
    if (c == nullptr) {
        flash("NOTHING HEARD TO KEEP");
        return false;
    }
    pushUndo(0);
    hist_.back().prevClip = clip_;
    hist_.back().prevSel = selc_;
    clip_ = std::move(c);
    selectFromLift_ = !what.startsWith("REC");
    selc_ = eng::defaultSelection(*clip_, static_cast<double>(::kSampleRate));
    selHist_.clear();
    keptT_ = t_;
    an_.liftT = t_;
    flash(what + kDot + juce::String(clip_->frames() / static_cast<double>(::kSampleRate), 1) + " S" + kDot +
          juce::String(clip_->lufsIn, 1) + " LUFS " + juce::String(clip_->gainDb >= 0.f ? "+" : "") +
          juce::String(clip_->gainDb, 1) + " DB");
    hint(juce::String(clip_->loop ? "KEPT A LOOP" : "KEPT A HIT") + kDot + "DROP PLACES IT" + kDot +
         "TURN A KNOB TO OPEN SELECT");
    return true;
}

// DROP: the selection (default or edited) to a place. dest -1 = the armed loop.
void LiftPanel::place(int dest, int note) {
    if (clip_ == nullptr) {
        flash("NOTHING KEPT: LIFT OR REC FIRST");
        return;
    }
    auto cut = eng::cutSelection(*clip_, selc_);
    if (cut == nullptr) {
        flash("EMPTY SELECTION");
        return;
    }
    if (dest < 0) {
        dest = DestLoop + arm_;
    }
    pushUndo(1);
    HistEntry& h = hist_.back();
    h.dest = dest;
    h.note = note;
    juce::String where;
    if (dest < DestKeys) {
        proc_.dropToLoop(cut, dest);
        where = "T" + juce::String(dest + 1);
    } else if (dest == DestKeys) {
        h.prevClip = keysAt_;
        keysAt_ = cut;
        proc_.setKeysClip(cut);
        where = "THE KEYS";
    } else if (dest == DestKey) {
        note = juce::jlimit(0, 127, note);
        h.note = note;
        h.prevClip = keyAt_[static_cast<size_t>(note)];
        keyAt_[static_cast<size_t>(note)] = cut;
        proc_.setKeyClip(note, cut);
        lastDestNote_ = note;
        where = "KEY " + juce::String(NAMES[note % 12]) + juce::String(note / 12 - 1);
    } else if (dest == DestDrum) {
        h.prevClip = drumAt_;
        drumAt_ = cut;
        proc_.setSliceKit(cut, selc_.chop > 1 ? selc_.chop : 24);
        where = "DRUM SLICES";
    } else {
        proc_.addToPool(cut);
        where = "IN POOL " + juce::String(static_cast<int>(proc_.pool().size()));
    }
    lastDest_ = dest;
    an_.dropT = t_;
    if (selectOpen_) {
        closeSelect();
    }
    flash("DROPPED" + kDot + where);
    hint("DROP" + kDot + where + kDot + "UNDO TAKES IT BACK");
    repaint();
}

void LiftPanel::exportTo(const juce::File& folder) {
    juce::StringArray files;
    const auto cut = clip_ != nullptr ? eng::cutSelection(*clip_, selc_) : nullptr;
    const bool ok = proc_.exportWavs(folder, cut, &files);
    flash(ok ? "EXPORTED " + juce::String(files.size()) + " WAV FILES" : juce::String("EXPORT FAILED"));
    say(ok ? "Exported to " + folder.getFullPathName() + ": " + files.joinIntoString(", ") : "Export failed.");
}

// ---------------------------------------------------------------- SELECT
//
// SELECT is a window over the newest keep. It stays shut on the fast path:
// a keep comes with its default selection (HIT for a short sound, the whole
// LOOP for a loop, snapped to zero crossings), so DROP places it at once and
// SHIFT+DROP keeps and places in one go (into the last place used). SELECT
// opens when a main knob is turned within 3 s of a keep, or on hold LIFT /
// hold REC. Knobs by colour: red START, yellow LENGTH, blue ZOOM, black SNAP
// (off / zero / hit / beat / bar). SHIFT+red moves the window (start and end
// together); SHIFT+yellow halves / doubles the length. Keys audition the
// selection, keys 1-8 chop it (1 = whole .. 8 slices), DROP places it.
// The waveform is the kept audio itself: what was heard, post chain.

void LiftPanel::openSelect(int how) {
    const bool lift = how == 1;
    if (lift && (clip_ == nullptr || t_ - keptT_ > 0.5)) {
        // hold LIFT: keep now, then open on it
        keep(proc_.keepLast(liftSeconds()), "LIFT");
    }
    if (clip_ == nullptr) {
        flash("NOTHING KEPT TO SELECT");
        return;
    }
    selectOpen_ = true;
    if (how != 0) {
        selectFromLift_ = lift;  // (a knob keeps where the keep came from)
    }
    selHist_.clear();
    flash(juce::String::fromUTF8(selectFromLift_ ? "SELECT \xc2\xb7 LIFT" : "SELECT \xc2\xb7 REC"));
    hint(juce::String("RED START") + kDot + "YELLOW LENGTH" + kDot + "BLUE ZOOM" + kDot + "BLACK SNAP");
    repaint();
}

void LiftPanel::closeSelect() {
    selectOpen_ = false;
    selHist_.clear();
    proc_.setAudition(nullptr);
    repaint();
}

// LIFT / REC / SHIFT encoders while SELECT is open: LIFT nudges the start,
// REC the length, SHIFT moves the window.
void LiftPanel::selectTurn(int fn, float delta) {
    selectKnob(fn == 0 ? 3 : 0, delta);
}

void LiftPanel::selectKnob(int colour, float delta) {
    if (clip_ == nullptr || delta == 0.f) {
        return;
    }
    // one undo step per gesture (a knob turned after a pause, or another knob)
    if (selEditKnob_ != colour || t_ - selEditT_ > 0.6) {
        selHist_.push_back(selc_);
        if (selHist_.size() > 64) selHist_.erase(selHist_.begin());
    }
    selEditKnob_ = colour;
    selEditT_ = t_;
    const int n = clip_->frames();
    const double beat = 60.0 / juce::jmax(20.0, proc_.tempoBpm.load()) * ::kSampleRate;
    const double viewLen = n * std::pow(1.0 / 64.0, static_cast<double>(selZoom_));
    eng::Selection s = selc_;
    const bool sh = shiftActive();
    if (colour == 3) {  // red: start (SHIFT: the whole window)
        const int d = static_cast<int>(delta * viewLen);
        s.start = juce::jlimit(0, n - 64, s.start + d);
        s.start = eng::snapFrame(*clip_, s.start, s.snap, beat);
        if (!sh) {
            s.length = juce::jlimit(64, n - s.start, selc_.start + selc_.length - s.start);
        } else {
            s.length = juce::jmin(s.length, n - s.start);
        }
    } else if (colour == 0) {  // yellow: length (SHIFT: halve / double per step)
        if (sh) {
            static float acc = 0.f;
            acc += delta;
            if (std::abs(acc) >= 0.08f) {
                s.length = acc > 0 ? s.length * 2 : s.length / 2;
                acc = 0.f;
            }
        } else {
            s.length += static_cast<int>(delta * viewLen);
        }
        s.length = juce::jlimit(64, n - s.start, s.length);
        const int end = eng::snapFrame(*clip_, s.start + s.length, s.snap, beat);
        s.length = juce::jlimit(64, n - s.start, end - s.start);
    } else if (colour == 1) {  // blue: zoom
        selZoom_ = juce::jlimit(0.f, 1.f, selZoom_ + delta);
    } else {  // black: snap
        static float acc = 0.f;
        acc += delta;
        if (std::abs(acc) >= 0.1f) {
            s.snap = juce::jlimit(0, eng::kSnaps - 1, s.snap + (acc > 0 ? 1 : -1));
            acc = 0.f;
        }
    }
    selc_ = s;
    flash(juce::String("SELECT") + kDot + juce::String(s.start / static_cast<double>(::kSampleRate), 2) + " S +" +
          juce::String(s.length / static_cast<double>(::kSampleRate), 2) + " S" + kDot + "SNAP " + eng::kSnapNames[s.snap]);
    repaint();
}

bool LiftPanel::selectUndo() {
    if (selHist_.empty()) {
        return false;
    }
    selc_ = selHist_.back();
    selHist_.pop_back();
    selEditKnob_ = -1;
    flash("UNDO SELECT EDIT");
    repaint();
    return true;
}

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
