// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// The shift layer: SHIFT on the keypad (tap = latch, press and hold =
// momentary) or the computer's Shift key (momentary). While shifted, the 14
// white keys and 10 black buttons run per-mode functions, four keypad keys
// run combos, knobs turn fine, the screen shows a legend and the SHIFT pad
// glows from underneath. Map and status (real vs placeholder): panel/SHIFT.md.

#include "LiftPanel.h"

#include "PaintUtil.h"

#include "tape/character.h"

#include <cmath>

namespace lift {

using namespace ui;
using namespace draw;

namespace {

constexpr int kStepDivs[5] = {4, 8, 16, 32, 12};  // 12 = 1/8 triplets
const char* const kStepNames[5] = {"1/4", "1/8", "1/16", "1/32", "1/8T"};
constexpr int kDrumLens[6] = {4, 8, 12, 16, 24, 32};
constexpr int kSwings[5] = {0, 15, 30, 45, 60};
constexpr float kSpeedPresets[3] = {0.5f, 1.f, 2.f};
const char* const kRecSources[3] = {"SYNTH", "INPUT", "RESAMPLE"};
constexpr int kMinLoop = 2400;  // 50 ms

float clamp01Shift(float v) {
    return juce::jlimit(0.f, 1.f, v);
}

// note 0..23 -> white key index 0..13 or black button index 0..9 (kind 0/1)
void keyIndex(int note, int& kind, int& idx) {
    const int o = note / 12, s = note % 12;
    for (int j = 0; j < 7; ++j) {
        if (NAT_SEMI[j] == s) {
            kind = 0;
            idx = o * 7 + j;
            return;
        }
    }
    for (int b = 0; b < 5; ++b) {
        if (SHARP_DEF[b][0] == s) {
            kind = 1;
            idx = o * 5 + b;
            return;
        }
    }
    kind = -1;
    idx = -1;
}

juce::String secs(int frames) {
    const int ms = static_cast<int>(static_cast<double>(frames) * 1000.0 / kSampleRate);
    return juce::String(ms / 60000).paddedLeft('0', 2) + ":" + juce::String((ms / 1000) % 60).paddedLeft('0', 2) + "." +
           juce::String((ms / 10) % 100).paddedLeft('0', 2);
}

}  // namespace

// ------------------------------------------------------------------ state

void LiftPanel::updateShift() {
    const bool on = shiftActive();
    shift_ = on;
    if (on != shiftWas_) {
        shiftWas_ = on;
        if (on) {
            shiftOnT_ = t_;
        }
        repaint(shiftKeyArea());
        repaint(screenArea());
    }
}

void LiftPanel::setShiftKey(bool down) {
    if (shiftKey_ != down) {
        shiftKey_ = down;
        updateShift();
    }
}

void LiftPanel::modifierKeysChanged(const juce::ModifierKeys& mods) {
    setShiftKey(mods.isShiftDown());
}

int LiftPanel::shiftGroup() const {
    if (bay_) {
        return 0;
    }
    return mode_ == Synth ? 1 : mode_ == Drum ? 2 : 0;
}

void LiftPanel::pressKey(int note) {
    if (shiftActive()) {
        shiftKeyFn(note);
    } else {
        noteOn(note);
    }
}

void LiftPanel::markFn(int kind, int idx) {
    lastFnKind_ = kind;
    lastFnIdx_ = idx;
    lastFnT_ = t_;
    repaint(screenArea());
}

void LiftPanel::sendLoop() {
    proc_.send(Cmd::LoopSet, loop_ ? loopIn_ : 0, loop_ ? loopOut_ : 0);
}

juce::Rectangle<int> LiftPanel::shiftKeyArea() const {
    // the SHIFT pad and the sheet its light spills onto (canvas pixels)
    return memRect(kShiftSlot).expanded(44.f, 40.f).translated(kDevX, kDevY).getSmallestIntegerContainer();
}

// ------------------------------------------------------------------ legend

juce::String LiftPanel::shiftLabel(int group, int kind, int idx, bool& real) const {
    real = true;
    if (kind == 2) {  // keypad combos: 0 REC, 1 LOOP, 2 STOP, 3 LIFT, 4 DROP, 5 PLAY, 6 REV
        switch (idx) {
        case 0:
            real = recSource_ != 1;
            return juce::String("SOURCE: ") + kRecSources[recSource_];
        case 1: return "LOOP END HERE";
        case 2: return "HARD STOP";
        case 3: return "LIFT ALL TRACKS";
        case 4: return "SAVE SLOT";
        case 5: return "LOAD SLOT";
        default: return learnArm_ ? "MIDI LEARN: ON" : "MIDI LEARN";
        }
    }
    if (group == 0) {
        if (kind == 0) {
            static const char* w[14] = {"LOOP IN",  "LOOP OUT", "LOOP OFF", "TRACK 1",  "TRACK 2",  "TRACK 3",    "TRACK 4",
                                        "JUMP M1",  "JUMP M2",  "JUMP M3",  "JUMP M4",  "TO START", "TO LOOP IN", "LOOP X2"};
            return w[idx];
        }
        static const char* b[10] = {"0.5X SPEED", "1X SPEED", "2X SPEED", "REVERSE", "UNDO",
                                    "CLEAR TRACK", "SET MARK", "CASSETTE", "BACK 1 S", "FWD 1 S"};
        return b[idx];
    }
    if (group == 1) {
        if (kind == 0) {
            if (idx < 7) {
                return "OCT " + juce::String(idx - 3 > 0 ? "+" : "") + juce::String(idx - 3);
            }
            real = false;  // one placeholder voice plays for every engine
            return ENGINES[idx - 7].n;
        }
        if (idx < 5) {
            return "TRANSPOSE " + juce::String(idx - 2 > 0 ? "+" : "") + juce::String(idx - 2);
        }
        real = false;
        return juce::String("STEP ") + kStepNames[idx - 5];
    }
    real = false;  // no drum engine yet
    if (kind == 0) {
        return idx < 8 ? "KIT " + juce::String(idx + 1) : "LENGTH " + juce::String(kDrumLens[idx - 8]);
    }
    return idx < 5 ? juce::String("STEP ") + kStepNames[idx] : "SWING " + juce::String(kSwings[idx - 5]) + "%";
}

// ------------------------------------------------------------------ functions

void LiftPanel::shiftKeyFn(int note) {
    int kind = -1, idx = -1;
    keyIndex(note, kind, idx);
    if (kind < 0) {
        return;
    }
    const int group = shiftGroup();
    const int pos = static_cast<int>(proc_.uiPos.load());
    const int frames = juce::jmax(1, proc_.uiFrames.load());
    bool real = true;
    const juce::String label = shiftLabel(group, kind, idx, real);
    juce::String msg = label;
    if (group == 0 && kind == 0) {
        if (idx == 0) {
            loopIn_ = juce::jlimit(0, frames - 1, pos);
            if (loopOut_ <= loopIn_ + kMinLoop) {
                loopOut_ = juce::jmin(frames, loopIn_ + LiftProcessor::kLoopSeconds * kSampleRate);
            }
            loop_ = true;
            sendLoop();
            msg = "LOOP IN " + secs(loopIn_);
        } else if (idx == 1) {
            if (pos > loopIn_ + kMinLoop) {
                loopOut_ = juce::jmin(frames, pos);
                loop_ = true;
                sendLoop();
                msg = "LOOP OUT " + secs(loopOut_);
            } else {
                msg = "LOOP OUT GOES AFTER LOOP IN";
            }
        } else if (idx == 2) {
            loop_ = false;
            sendLoop();
        } else if (idx < 7) {
            act("arm:" + juce::String(idx - 3));
        } else if (idx < 11) {
            proc_.send(Cmd::Jump, 0, 0, marks_[static_cast<size_t>(idx - 7)]);
        } else if (idx == 11) {
            proc_.send(Cmd::Jump, 0, 0, 0.0);
        } else if (idx == 12) {
            proc_.send(Cmd::Jump, 0, 0, loopIn_);
        } else {
            loopOut_ = juce::jmin(frames, loopIn_ + 2 * (loopOut_ - loopIn_));
            loop_ = true;
            sendLoop();
            msg = "LOOP " + secs(loopIn_) + " - " + secs(loopOut_);
        }
    } else if (group == 0) {
        if (idx < 3) {
            enc_[Tape][0] = std::log(kSpeedPresets[idx] / 0.25f) / std::log(16.f);
            syncEngine();
        } else if (idx == 3) {
            act("rev");
        } else if (idx == 4) {
            undo();  // the one history (cables, pins, keeps, drops, overdubs)
        } else if (idx == 5) {
            if (clearConfirm_ == arm_ && t_ - clearT_ < 2.0) {
                proc_.send(Cmd::Clear, arm_);
                clearConfirm_ = -1;
                msg = "CLEARED T" + juce::String(arm_ + 1);
            } else {
                clearConfirm_ = arm_;
                clearT_ = t_;
                msg = "CLEAR T" + juce::String(arm_ + 1) + "? PRESS AGAIN";
            }
        } else if (idx == 6) {
            marks_[static_cast<size_t>(nextMark_)] = pos;
            msg = "MARK " + juce::String(nextMark_ + 1) + " AT " + secs(pos);
            nextMark_ = (nextMark_ + 1) % 4;
        } else if (idx == 7) {
            // the cassette stage: OFF -> character 1 .. N -> OFF
            if (!cassette_) {
                cassette_ = true;
                character_ = 0;
            } else if (character_ + 1 < kCharacterCount) {
                ++character_;
            } else {
                cassette_ = false;
            }
            proc_.send(Cmd::Character, character_);
            proc_.send(Cmd::Cassette, cassette_ ? 1 : 0);
            msg = cassette_ ? "CASSETTE " + juce::String(character_ + 1) : juce::String("CASSETTE OFF");
        } else {
            proc_.send(Cmd::Jump, 0, 0, static_cast<double>(pos + (idx == 8 ? -kSampleRate : kSampleRate)));
        }
    } else if (group == 1) {
        if (kind == 0 && idx < 7) {
            oct_ = idx - 3;
        } else if (kind == 0) {
            sel_[Synth] = idx - 7;
            proc_.synthEngine.store(idx - 7);
        } else if (idx < 5) {
            transpose_ = idx - 2;
        } else {
            seqDiv_ = kStepDivs[idx - 5];
            proc_.seqStepDiv.store(seqDiv_);
        }
    } else {
        if (kind == 0 && idx < 8) {
            sel_[Drum] = idx;
            proc_.drumKit.store(idx);
        } else if (kind == 0) {
            drumLen_ = kDrumLens[idx - 8];
            proc_.drumLength.store(drumLen_);
        } else if (idx < 5) {
            drumDiv_ = kStepDivs[idx];
            proc_.drumStepDiv.store(drumDiv_);
        } else {
            swing_ = kSwings[idx - 5];
            proc_.drumSwing.store(swing_);
        }
    }
    flash(msg);
    markFn(kind, idx);
    repaint();
}

void LiftPanel::shiftCombo(int fn) {
    if (fn == 5) {  // REC: record source
        recSource_ = (recSource_ + 1) % 3;
        proc_.send(Cmd::RecSource, recSource_);
        flash(juce::String("REC SOURCE ") + kRecSources[recSource_]);
        markFn(2, 0);
    } else if (fn == 1) {  // LOOP: loop end at the playhead
        const int pos = static_cast<int>(proc_.uiPos.load());
        if (pos > loopIn_ + kMinLoop) {
            loopOut_ = pos;
        } else {
            loopIn_ = 0;
            loopOut_ = juce::jmax(kMinLoop * 2, pos);
        }
        loop_ = true;
        sendLoop();
        flash("LOOP " + secs(loopIn_) + " - " + secs(loopOut_));
        markFn(2, 1);
    } else if (fn == 9) {  // STOP: hard stop
        playing_ = false;
        rec_ = false;
        rev_ = false;
        proc_.send(Cmd::HardStop);
        flash("HARD STOP");
        markFn(2, 2);
    } else if (fn == 0) {  // LIFT: all tracks
        keep(proc_.keepTracks(), "LIFT ALL TRACKS");
        markFn(2, 3);
    } else if (fn == 4) {  // DROP: the fast commit (SHIFT + hold DROP saves to a slot)
        act("dropnow");
        markFn(2, 4);
    } else if (fn == 7) {  // PLAY: load a slot
        openPicker(Picker::Load);
        markFn(2, 5);
    } else if (fn == 3) {  // REV: MIDI learn on/off
        learnArm_ = !learnArm_;
        proc_.learnTarget.store(-1);
        flash(learnArm_ ? "MIDI LEARN: TOUCH A KNOB" : "MIDI LEARN OFF");
        markFn(2, 6);
    }
    repaint();
}

// ------------------------------------------------------------------ drawing

// Warm light from under the SHIFT pad: a diffuse bloom on the black sheet
// around the pad plus light leaking through the gap under its edges.
void LiftPanel::paintShiftBloom(Graphics& g, Rectangle<float> r, float a) {
    // Drawn after the keys, on the sheet and the key surrounds only: the pad
    // faces themselves stay unlit.
    const Rectangle<float> M(52.f, 706.f, 616.f, 134.f);
    Path clip = rrect(M, 6.f);
    for (int k = 0; k < 10; ++k) {
        clip.addRoundedRectangle(memRect(k), 5.f);
    }
    clip.setUsingNonZeroWinding(false);
    Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(clip);
    // stacked soft rings: a cheap, smooth falloff (dense near the pad, gone ~30 px out)
    constexpr int kRings = 28;
    for (int i = kRings; i >= 0; --i) {
        const float u = static_cast<float>(i) / kRings;
        const Colour c = hex(0xff8a3c).interpolatedWith(hex(0xff3010), u);
        g.setColour(c.withAlpha(0.11f * a * (1.f - u) * (1.f - u)));
        g.fillRoundedRectangle(r.expanded(2.f + 26.f * u), 7.f + 14.f * u);
    }
}

void LiftPanel::paintShiftOverlay(Graphics& g, float amtLin) {
    const float a = amtLin * amtLin * (3.f - 2.f * amtLin);  // smoothstep
    g.setColour(hex(0x0b0b0b, 0.94f * a));
    g.fillRect(-10.f, -60.f, 740.f, 420.f);
    const int group = shiftGroup();
    const float since = static_cast<float>(t_ - shiftOnT_);
    const float dy = reducedMotion_ ? 0.f : 12.f * (1.f - easeOutBack(clamp01Shift(since / 0.2f)));
    Graphics::ScopedSaveState s(g);
    g.addTransform(AffineTransform::translation(0.f, dy));
    const Colour cr = hex(0xede6d6, a), grey = hex(0x8c877b, a), yel = hex(0xf4be2a, a), dim = hex(0x5e5a50, a);
    static const char* groupNames[3] = {"TRANSPORT", "SYNTH", "DRUM"};
    svgText(g, "SHIFT", 24.f, 26.f, 18.f, yel, -1, true);
    svgText(g, groupNames[group], 24.f + 74.f, 26.f, 18.f, cr, -1, true);
    svgText(g, juce::String::fromUTF8(shiftLatch_ ? "LATCHED \xc2\xb7 TAP SHIFT TO RELEASE" : "HELD \xc2\xb7 TAP SHIFT TO LATCH"), 696.f, 24.f, 9.f,
            grey, 1, false, 1.f);
    svgText(g, "WHITE KEYS", 24.f, 48.f, 9.f, grey, -1, false, 1.f);
    svgText(g, "BLACK BUTTONS", 372.f, 48.f, 9.f, grey, -1, false, 1.f);

    auto isCurrent = [&](int kind, int idx) {
        if (kind == 2) {
            return false;
        }
        if (group == 0) {
            if (kind == 0) {
                return (idx >= 3 && idx < 7 && arm_ == idx - 3) || (idx == 2 && !loop_);
            }
            return (idx < 3 && std::abs(speedOf() - kSpeedPresets[idx]) < 0.01f) || (idx == 3 && rev_);
        }
        if (group == 1) {
            if (kind == 0) {
                return idx < 7 ? oct_ == idx - 3 : sel_[Synth] == idx - 7;
            }
            return idx < 5 ? transpose_ == idx - 2 : seqDiv_ == kStepDivs[idx - 5];
        }
        if (kind == 0) {
            return idx < 8 ? sel_[Drum] == idx : drumLen_ == kDrumLens[idx - 8];
        }
        return idx < 5 ? drumDiv_ == kStepDivs[idx] : swing_ == kSwings[idx - 5];
    };
    auto row = [&](int kind, int idx, float x, float y, int order) {
        const float rp = reducedMotion_ ? 1.f : clamp01Shift((since - static_cast<float>(order) * 0.012f) / 0.18f);
        if (rp <= 0.f) {
            return;
        }
        Graphics::ScopedSaveState rs(g);
        g.addTransform(AffineTransform::translation(-14.f * (1.f - easeOutBack(rp, 1.6f)), 0.f));
        const float ra = a * clamp01Shift(rp * 1.5f);
        const float fl = (lastFnKind_ == kind && lastFnIdx_ == idx) ? decayFrom(t_ - lastFnT_, 3.f) : 0.f;
        if (fl > 0.01f) {
            g.setColour(hex(0xf4be2a, 0.3f * fl * ra));
            g.fillRect(x - 6.f, y - 12.f, 168.f, 17.f);
        }
        bool real = true;
        const juce::String label = shiftLabel(group, kind, idx, real);
        const bool cur = isCurrent(kind, idx);
        if (kind == 0) {
            g.setColour(hex(0xede6d6, ra));
            if (cur) {
                g.fillRoundedRectangle(x, y - 10.f, 9.f, 13.f, 2.f);
            } else {
                g.drawRoundedRectangle(x + 0.75f, y - 9.25f, 7.5f, 11.5f, 2.f, 1.5f);
            }
        } else if (kind == 1) {
            fill(g, circle(x + 5.f, y - 3.5f, 5.f), cur ? hex(0xf4be2a, ra) : hex(0x2a2925, ra));
            g.setColour(hex(0xede6d6, ra));
            g.drawEllipse(x, y - 8.5f, 10.f, 10.f, 1.2f);
        }
        if (kind < 2) {
            svgText(g, juce::String(idx + 1), x + 26.f, y, 9.f, dim.withAlpha(ra), 1);
        }
        const Colour lc = cur ? hex(0xf4be2a, ra) : real ? hex(0xede6d6, ra) : hex(0x8c877b, ra);
        svgText(g, label, kind < 2 ? x + 32.f : x + 52.f, y, 11.f, lc, -1, cur);
    };
    int order = 0;
    for (int i = 0; i < 14; ++i) {
        row(0, i, i < 7 ? 24.f : 196.f, 68.f + static_cast<float>(i % 7) * 19.f, order++);
    }
    for (int i = 0; i < 10; ++i) {
        row(1, i, i < 5 ? 372.f : 540.f, 68.f + static_cast<float>(i % 5) * 19.f, order++);
    }
    svgText(g, "KEYPAD + SHIFT", 372.f, 178.f, 9.f, grey, -1, false, 1.f);
    // keypad combos: REC LOOP DROP PLAY | STOP LIFT REV (legend index order 0 1 4 5 | 2 3 6)
    static const char* keys[7] = {"REC", "LOOP", "STOP", "LIFT", "DROP", "PLAY", "REV"};
    static const int col[7] = {0, 0, 1, 1, 0, 0, 1};
    static const int rowN[7] = {0, 1, 0, 1, 2, 3, 2};
    for (int i = 0; i < 7; ++i) {
        const float x = col[i] == 0 ? 372.f : 540.f, y = 198.f + static_cast<float>(rowN[i]) * 19.f;
        const float rp = reducedMotion_ ? 1.f : clamp01Shift((since - static_cast<float>(order) * 0.012f) / 0.18f);
        const float ra = a * clamp01Shift(rp * 1.5f);
        g.setColour((i == 0 || i == 2) ? hex(0xea3e3e, ra) : hex(0x4ec4ec, ra));
        g.fillRoundedRectangle(x, y - 11.f, 44.f, 14.f, 3.f);
        svgText(g, keys[i], x + 22.f, y - 0.5f, 9.f, hex(0xffffff, ra), 0, true);
        row(2, i, x, y, order++);
    }
    svgText(g, "SHIFT + KNOB = FINE", 24.f, 198.f, 9.f, grey, -1, false, 1.f);
    if (group == 0 && mode_ != Tape) {
        svgText(g, "MIX, IN AND BAY USE THE TRANSPORT MAP", 24.f, 217.f, 9.f, grey, -1, false, 1.f);
    }
    svgText(g, "GREY = PLACEHOLDER, NO ENGINE BEHIND IT YET", 24.f, 300.f, 9.f, dim, -1, false, 1.f);
    svgText(g, "LOOP " + secs(loopIn_) + " - " + secs(loopOut_) + (loop_ ? "" : " (OFF)"), 696.f, 300.f, 9.f,
            loop_ ? cr : dim, 1, false, 1.f);
}

}  // namespace lift
