// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// The screen: status bar, the six views and the knob foot, now animated.
// Layouts, palette and type are the prototype's; motion is added on top in a
// flat, elastic style (springs that overshoot, squash-and-stretch kicks, quick
// slide transitions). At rest every view draws the static design.
//
// Real state drives the motion wherever the engine has it: transport position,
// tape speed and direction, the stop ramp, wow/flutter, recording, track and
// master levels, the placeholder voice output, knob values, notes and the
// patch model. Where nothing real exists yet the motion is marked
// PLACEHOLDER MOTION.

#include "LiftPanel.h"

#include "PaintUtil.h"

#include "tape/character.h"

#include <cmath>

namespace lift {

using namespace ui;
using namespace draw;

namespace {

constexpr float kTransition = 0.28f;  // seconds, mode-to-mode slide

float clamp01(float v) {
    return juce::jlimit(0.f, 1.f, v);
}

// Settled springs draw on the design's whole-pixel grid; moving ones are free.
float settle(const Spring& s, float px) {
    return s.v == 0.f ? static_cast<float>(juce::roundToInt(px)) : px;
}

// The static waveform per engine (prototype drawing), t in 0..1.
double waveFn(int engine, const std::array<float, 4>& k, double t) {
    const double tau = 2.0 * juce::MathConstants<double>::pi;
    switch (engine) {
    case 0: {
        const double p = std::fmod(t * 3.0, 1.0);
        const double p2 = std::fmod(t * 3.0 * 1.02 + 0.13, 1.0);
        return 0.5 * (2.0 * p - 1.0) + 0.38 * (p2 < 0.5 ? 1.0 : -1.0);
    }
    case 1: {
        const double p = std::fmod(t * 3.0, 1.0);
        const double d = 0.08 + 0.4 * (1.0 - k[0]);
        const double q = p < d ? p * 0.5 / d : 0.5 + (p - d) * 0.5 / (1.0 - d);
        return std::cos(tau * q) * 0.9;
    }
    case 2: {
        double x = (1.0 + 2.5 * k[0]) * std::sin(tau * 3.0 * t);
        int n = 0;
        while ((x > 1.0 || x < -1.0) && n < 12) {
            x = x > 1.0 ? 2.0 - x : -2.0 - x;
            ++n;
        }
        return x * 0.9;
    }
    case 3: return std::sin(tau * 3.0 * t + (0.5 + 3.0 * k[2]) * std::sin(tau * 9.0 * t)) * 0.9;
    case 4: {
        double v = 0.0;
        for (int j = 1; j < 8; ++j) {
            v += std::sin(tau * j * 7.0 * t) / j;
        }
        return v * 0.62 * std::exp(-3.0 * t);
    }
    case 5: {
        double v = 0.0;
        for (int j = 0; j < 5; ++j) {
            v += std::sin(tau * t * 8.0 * (1.0 + j * 0.035));
        }
        return v / 5.0;
    }
    case 6: return std::exp(-4.0 * std::fmod(t * 3.0, 1.0)) * std::sin(t * 620.0) * std::sin(t * 91.3 + 1.0);
    default: return 0.0;
    }
}

}  // namespace

juce::Rectangle<int> LiftPanel::screenArea() {
    return juce::Rectangle<float>(kDevX + 58.f + kMainDX, kDevY + 254.f + kMainDY, 604.f, 410.f).getSmallestIntegerContainer();
}

// ------------------------------------------------------------------ clock

void LiftPanel::advance(double dtD) {
    const float dt = static_cast<float>(dtD);
    t_ += dtD;
    const bool snap = reducedMotion_;
    bool busy = false;

    // state sync with the processor: push panel changes; MIDI start/stop
    // moved the tape, so the transport keys follow
    pushUi();
    const int ts = proc_.transportSerial.load();
    if (ts != seenTransport_) {
        seenTransport_ = ts;
        playing_ = proc_.midiPlay.load();
        rec_ = false;
        rev_ = false;
        busy = true;
    }
    busy = busy || (picker_ != Picker::None && (t_ - pickT_ < 0.5 || t_ - pickStepT_ < 0.6));

    // view changes start a slide; the status label pops
    const int view = bay_ ? 5 : static_cast<int>(mode_);
    if (view != an_.view) {
        an_.prevView = an_.view;
        an_.view = view;
        an_.viewT = t_;
        an_.labelT = t_;
        an_.viewDir = view > an_.prevView ? 1 : -1;
        if (view == 5) {
            an_.bayT = t_;
        }
    }
    busy = busy || (t_ - an_.viewT) < 1.0;

    // knob-driven springs
    for (size_t i = 0; i < 4; ++i) {
        const float target = encAt(slotOf(static_cast<int>(i)));
        an_.foot[i].step(target, dt, snap);
        busy = busy || an_.foot[i].moving(target);
        const float fv = arm_ == static_cast<int>(i) ? enc_[Mix][0] : screen::kMixLevels[i];
        const float pv = arm_ == static_cast<int>(i) ? enc_[Mix][1] : screen::kMixPans[i];
        an_.fader[i].step(fv, dt, snap);
        an_.pan[i].step(pv, dt, snap);
        busy = busy || an_.fader[i].moving(fv) || an_.pan[i].moving(pv);
    }
    an_.eqLo.step(enc_[Mix][2], dt, snap);
    an_.eqHi.step(enc_[Mix][3], dt, snap);
    an_.needle.step(enc_[In][0], dt, snap, 300.f, 9.f);  // radio dial: looser, more overshoot
    an_.gain.step(enc_[In][2], dt, snap);
    busy = busy || an_.eqLo.moving(enc_[Mix][2]) || an_.eqHi.moving(enc_[Mix][3]) ||
           an_.needle.moving(enc_[In][0]) || an_.gain.moving(enc_[In][2]);

    // TAPE: reels follow the real playhead (speed, direction, stop ramp and
    // scrub all come through the position). Constant tape speed means the
    // smaller pack spins faster.
    const double pos = proc_.uiPos.load();
    const int frames = proc_.uiFrames.load();
    const float reel = proc_.uiReelSpeed.load();
    double dpos = an_.lastPos < 0.0 ? 0.0 : pos - an_.lastPos;
    if (std::abs(dpos) > 0.5 * kSampleRate) {
        dpos = reel * dtD * kSampleRate;  // loop wrap or seek jump
    }
    an_.lastPos = pos;
    const float frac = frames > 0 ? clamp01(static_cast<float>(pos / frames)) : 0.f;
    const float packL = 66.f - 20.f * frac, packR = 46.f + 20.f * frac;
    const float turn = static_cast<float>(dpos / kSampleRate) * juce::MathConstants<float>::twoPi / 2.4f;
    an_.reelL = std::fmod(an_.reelL + turn * 56.f / packL, juce::MathConstants<float>::twoPi);
    an_.reelR = std::fmod(an_.reelR + turn * 56.f / packR, juce::MathConstants<float>::twoPi);
    an_.omega = dt > 0.f ? turn / dt : 0.f;
    an_.speedAbs = std::abs(reel);
    an_.wowPhase = proc_.uiWowPhase.load();
    an_.wowDepth = proc_.uiWowDepth.load();
    const bool recording = proc_.uiRecording.load();
    const float lvl = proc_.uiSynthEnv.load();
    const float glowT = recording ? 0.55f + 0.45f * clamp01(lvl * 2.f) : 0.f;
    an_.headGlow += (glowT - an_.headGlow) * (1.f - std::exp(-dt * 14.f));
    if (an_.headGlow < 1e-3f && glowT == 0.f) {
        an_.headGlow = 0.f;
    }
    busy = busy || dpos != 0.0 || reel != 0.f || an_.headGlow > 0.f || (t_ - an_.liftT) < 1.0 ||
           (t_ - an_.dropT) < 1.0;

    // SYNTH: drain the scope tap (placeholder voice output), note and engine events
    const int got = proc_.readScope(an_.scratch.data(), static_cast<int>(an_.scratch.size()));
    for (int i = 0; i < got; ++i) {
        an_.scope[static_cast<size_t>(an_.scopeW % screen::kScopeRing)] = an_.scratch[static_cast<size_t>(i)];
        an_.scopeW = (an_.scopeW + 1) % (screen::kScopeRing * 1024);
    }
    const float envT = proc_.uiSynthEnv.load();
    an_.env += (envT - an_.env) * (1.f - std::exp(-dt * 30.f));
    if (an_.env < 1e-4f && envT == 0.f) {
        an_.env = 0.f;
    }
    if (note_ >= 0 && note_ != an_.note) {
        an_.noteT = t_;
    }
    an_.note = note_;
    if (sel_[Synth] != an_.engine) {
        an_.prevEngine = an_.engine;
        an_.engine = sel_[Synth];
        an_.engineT = t_;
    }
    busy = busy || view == Synth;  // live scope / idle breathing

    // DRUM: the playhead is the sequencer's own step (the engine runs it)
    const bool playing = proc_.uiPlaying.load();
    const int seqStep = proc_.uiDrumStep.load();
    {
        const double stepsPerSec = proc_.tempoBpm.load() / 60.0 * 4.0;
        const double frac = std::fmod(pos / kSampleRate * stepsPerSec, 1.0);
        an_.drumPhase = seqStep >= 0 ? std::fmod(static_cast<double>(seqStep % 16) + frac, 16.0) : 0.0;
    }
    const int step = seqStep >= 0 ? seqStep % 16 : -1;
    if (step != an_.lastStep) {
        const uint32_t pat = drumPat_[static_cast<size_t>(juce::jlimit(0, eng::kKits - 1, sel_[Drum]))]
                                     [static_cast<size_t>(drumVoice_)];
        if (playing && step >= 0 && ((pat >> step) & 1u)) {
            an_.hitT[static_cast<size_t>(step)] = t_;
        }
        an_.lastStep = step;
    }
    const int slice = juce::jlimit(0, 23, static_cast<int>(an_.drumPhase / 16.0 * 24.0));
    if (slice != an_.lastSlice) {
        if (playing && slice > 0) {
            an_.sliceT[static_cast<size_t>(slice)] = t_;
        }
        an_.lastSlice = slice;
    }

    // MIX: meters from the engine (track under the head, master out)
    for (size_t t = 0; t < 5; ++t) {
        const float v = t < 4 ? proc_.uiTrackLevel[t].load() : proc_.uiMasterLevel.load();
        const float target = clamp01(v);
        an_.meter[t] = target > an_.meter[t] ? target : an_.meter[t] * std::exp(-dt * 7.f);
        if (an_.meter[t] < 0.002f) {
            an_.meter[t] = 0.f;
        }
        if (an_.meter[t] >= an_.peak[t]) {
            an_.peak[t] = an_.meter[t];
            an_.peakT[t] = t_;
        } else if (t_ - an_.peakT[t] > 0.8) {
            an_.peak[t] = juce::jmax(an_.meter[t], an_.peak[t] - dt * 0.6f);
        }
        if (an_.peak[t] < 0.002f) {
            an_.peak[t] = 0.f;
        }
        busy = busy || an_.meter[t] > 0.f || an_.peak[t] > 0.f;
    }
    for (size_t t = 0; t < 4; ++t) {
        if (mutes_[t] != an_.lastMute[t]) {
            an_.lastMute[t] = mutes_[t];
            an_.muteT[t] = t_;
        }
        busy = busy || (t_ - an_.muteT[t]) < 1.2;
    }

    // IN: PLACEHOLDER MOTION. There is no audio input path yet, so the meters
    // wander around the prototype's levels like a live signal would.
    if (t_ >= an_.inNext) {
        an_.inTL = 0.70f + 0.30f * an_.rand01();
        an_.inTR = juce::jmax(0.4f, an_.inTL - 0.12f * an_.rand01());
        an_.inNext = t_ + 0.06 + 0.1 * an_.rand01();
    }
    const float inA = snap ? 1.f : 1.f - std::exp(-dt * 22.f);
    an_.inL += (an_.inTL - an_.inL) * inA;
    an_.inR += (an_.inTR - an_.inR) * inA;
    const bool above = an_.gain.x * an_.inL * 0.92f > enc_[In][3];
    if (above && !an_.above) {
        an_.threshT = t_;
    }
    an_.above = above;
    busy = busy || view == In;

    // BAY: a new cord flashes its row
    if (cords_.size() > an_.lastCords) {
        an_.cordT = t_;
    }
    an_.lastCords = cords_.size();
    busy = busy || view == 5;

    // shift: ~120 ms fade for the pad light and the screen legend
    {
        const float target = shiftActive() ? 1.f : 0.f;
        const float before = shiftAmt_;
        if (snap) {
            shiftAmt_ = target;
        } else if (shiftAmt_ < target) {
            shiftAmt_ = juce::jmin(target, shiftAmt_ + dt / 0.12f);
        } else if (shiftAmt_ > target) {
            shiftAmt_ = juce::jmax(target, shiftAmt_ - dt / 0.12f);
        }
        if (shiftAmt_ != before) {
            repaint(shiftKeyArea());
            busy = true;
        }
        busy = busy || (shiftAmt_ > 0.f && ((t_ - shiftOnT_) < 1.0 || (t_ - lastFnT_) < 1.5));
    }

    // status message fade
    if (msg_.isNotEmpty() && t_ > msgUntil_) {
        msg_.clear();
        busy = true;
    }
    busy = busy || msg_.isNotEmpty() || (t_ - an_.labelT) < 1.0 || an_.env > 0.f;

    if (busy) {
        repaint(screenArea());
    }
}

// ------------------------------------------------------------------ screen

// The bezel and glass: the wrap (the case recess round the screen; in the
// SVG art when that is used), the black rim and the glass with its sheen and
// inner shadows.
void LiftPanel::drawScreenBezel(Graphics& g, bool wrap) {
    if (wrap) {
        const Path w = rrect({44.f, 240.f, 632.f, 438.f}, 10.f);
        fill(g, w, hex(0xd9cdb1));
        inset(g, w, rgba(60, 50, 30, 0.28f), 0.f, 2.f, 4.f);
        inset(g, w, W(0.7f), 0.f, -1.f, 0.f);
    }
    const Rectangle<float> S(60.f, 256.f, 600.f, 406.f);
    const Path scr = rrect(S, 4.f);
    fill(g, rrect(S.expanded(2.f), 6.f), hex(0x121212));
    fill(g, scr, hex(0x0b0b0b));
    {
        const float dx = std::sin(122.f * kPi / 180.f), dy = -std::cos(122.f * kPi / 180.f);
        const float len = 600.f * std::abs(dx) + 406.f * std::abs(dy);
        const float cx = S.getCentreX(), cy = S.getCentreY();
        fill(g, scr,
             linear(cx - dx * len * 0.5f, cy - dy * len * 0.5f, cx + dx * len * 0.5f, cy + dy * len * 0.5f,
                    {{0.f, W(0.075f)}, {0.34f, W(0.02f)}, {0.345f, W(0.f)}, {1.f, W(0.f)}}));
    }
    inset(g, scr, B(0.85f), 0.f, 0.f, 50.f);
    inset(g, scr, B(0.9f), 0.f, 2.f, 6.f);
}

void LiftPanel::paintScreenBg(Graphics& g) {
    // The bezel, glass and inset shadows never move: cached once per scale.
    const float sc = juce::jmax(0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (screenBg_.isNull() || screenBgScale_ != sc) {
        screenBgScale_ = sc;
        // Origin at dev (44, 239.5): a whole canvas pixel (the face sits at
        // y 87.5), so at 1x the cached layer blits without resampling.
        screenBg_ = juce::Image(juce::Image::ARGB, juce::roundToInt(632.f * sc), juce::roundToInt(439.f * sc), true);
        Graphics ig(screenBg_);
        ig.addTransform(AffineTransform::translation(-44.f, -239.5f).scaled(sc));
        drawScreenBezel(ig, true);
    }
    // The layer is at device resolution, so nearest-neighbour is enough when
    // the window scale leaves it off the pixel grid.
    Graphics::ScopedSaveState s(g);
    g.setImageResamplingQuality(Graphics::lowResamplingQuality);
    g.drawImageTransformed(screenBg_, AffineTransform::scale(1.f / sc).translated(44.f, 239.5f));
}

void LiftPanel::paintScreen(Graphics& g) {
    paintScreenBg(g);
    const Rectangle<float> S(60.f, 256.f, 600.f, 406.f);
    Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(rrect(S, 4.f));
    g.addTransform(AffineTransform::translation(S.getX(), S.getY()));
    paintStatus(g);
    paintView(g);
    paintHint(g);
    paintFoot(g);
}

void LiftPanel::paintStatus(Graphics& g) {
    const juce::Font reg = mono(false, 11.f, 0.06f);
    const juce::Font bold = mono(true, 11.f, 0.06f);
    const Rectangle<float> row(0.f, 0.f, 600.f, 32.f);
    static const char* labelsM[5] = {"SYNTH", "DRUM", "TAPE", "MIX", "IN"};
    const juce::String label = bay_ ? "BAY" : labelsM[mode_];
    juce::String sub;
    if (bay_) {
        sub = "PATCH";
    } else if (mode_ == Synth) {
        sub = ENGINES[sel_[Synth]].n;
    } else if (mode_ == Drum) {
        sub = KITS[sel_[Drum]];
    } else if (mode_ == Tape) {
        sub = "DECK";
    } else if (mode_ == Mix) {
        sub = "T" + juce::String(arm_ + 1);
    } else {
        sub = INPUTS[sel_[In]];
    }
    float x = 16.f;
    {
        // the mode label pops when the screen changes
        const float b = reducedMotion_ ? 0.f : kick(t_ - an_.labelT, 24.f, 8.f);
        Graphics::ScopedSaveState ss(g);
        if (b != 0.f) {
            g.addTransform(AffineTransform::scale(1.f + 0.22f * b, 1.f - 0.22f * b, x, 16.f));
        }
        text(g, label, bold, bay_ ? hex(0x4c82e6) : hex(0xf4be2a), row.withX(x), Justification::centredLeft);
    }
    x += cssWidth(700, true, 11.f, 0.06f, label) + 10.f;
    text(g, sub, reg, hex(0x8c877b), row.withX(x), Justification::centredLeft);

    juce::String noteText;
    if (note_ >= 0) {
        noteText = juce::String(NAMES[note_ % 12]) + juce::String(3 + oct_ + note_ / 12);
    }
    juce::String pickText;
    if (pick_.valid()) {
        pickText = pick_.r == 'o' ? juce::String(OUTS[pick_.i].n) + juce::String::fromUTF8(" \xe2\x86\x92 PICK AN IN")
                                  : juce::String::fromUTF8("PICK AN OUT \xe2\x86\x92 ") + INS[pick_.i].n;
    }
    const bool showMsg = pickText.isEmpty() && msg_.isNotEmpty();
    juce::String centre = pickText.isNotEmpty() ? pickText
                          : showMsg             ? msg_
                          : noteText.isNotEmpty() ? "NOTE " + noteText
                                                  : counterText();
    Colour cc = (pick_.valid() || msg_.isNotEmpty()) ? hex(0xf4be2a)
                : (rec_ && playing_)                ? hex(0xe8473a)
                                                    : hex(0xede6d6);
    float dy = 0.f;
    if (showMsg && !reducedMotion_) {
        // messages drop in and fade out
        const float in = clamp01(static_cast<float>(t_ - an_.msgT) / 0.12f);
        const float out = clamp01(static_cast<float>(msgUntil_ - t_) / 0.3f);
        cc = cc.withMultipliedAlpha(juce::jmin(in, out));
        dy = -6.f * (1.f - easeOutBack(in));
    }
    const float cw = cssWidth(400, true, 11.f, 0.06f, centre);
    text(g, centre, reg, cc, {300.f - cw * 0.5f, dy, cw + 10.f, 32.f}, Justification::centredLeft);

    struct Item {
        juce::String s;
        Colour c;
    };
    std::vector<Item> items = {{juce::String(juce::roundToInt(proc_.tempoBpm.load())), proc_.clockSlaved.load() ? hex(0xede6d6) : hex(0x8c877b)},
                               {juce::String(speedOf(), 2) + juce::String::fromUTF8("\xc3\x97"), hex(0xede6d6)},
                               {"T" + juce::String(arm_ + 1), hex(SCR[arm_])}};
    if (rev_) {
        items.push_back({"REV", hex(0xf4be2a)});
    }
    if (shift_) {
        items.push_back({"SHIFT", hex(0xede6d6)});
    }
    float rx = 584.f;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const float w = cssWidth(400, true, 11.f, 0.06f, it->s);
        text(g, it->s, reg, it->c, {rx - w, 0.f, w + 10.f, 32.f}, Justification::centredLeft);
        rx -= w + 12.f;
    }
}

void LiftPanel::paintView(Graphics& g) {
    Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(Rectangle<int>(0, 32, 600, 324));
    const int view = bay_ ? 5 : static_cast<int>(mode_);
    const float p = static_cast<float>(t_ - an_.viewT) / kTransition;
    if (reducedMotion_ || view != an_.view || an_.prevView == an_.view || p >= 1.f) {
        paintViewFor(g, view);
    } else {
        paintTransition(g, view, p);
    }
    if (shiftAmt_ > 0.f) {
        Graphics::ScopedSaveState o(g);
        const float sc = 600.f / 720.f;
        g.addTransform(AffineTransform::scale(sc).translated(0.f, 32.f + (324.f - 319.f * sc) * 0.5f));
        paintShiftOverlay(g, shiftAmt_);
    }
    if (picker_ != Picker::None) {
        Graphics::ScopedSaveState o(g);
        const float sc = 600.f / 720.f;
        g.addTransform(AffineTransform::scale(sc).translated(0.f, 32.f + (324.f - 319.f * sc) * 0.5f));
        paintPicker(g);
    }
}

void LiftPanel::paintTransition(Graphics& g, int view, float p) {
    // snappy slide with a little overshoot: the old view is pushed out
    const float e = easeOutBack(p, 1.2f);
    const float d = static_cast<float>(an_.viewDir) * 600.f;
    {
        Graphics::ScopedSaveState o(g);
        g.addTransform(AffineTransform::translation(-d * e, 0.f));
        paintViewFor(g, an_.prevView);
    }
    {
        Graphics::ScopedSaveState o(g);
        g.addTransform(AffineTransform::translation(d * (1.f - e), 0.f));
        g.setColour(hex(0x0b0b0b));
        g.fillRect(0.f, 32.f, 600.f, 324.f);
        paintViewFor(g, view);
    }
    const float edge = an_.viewDir > 0 ? 600.f * (1.f - e) : 600.f * (1.f - e) * -1.f + 600.f;
    g.setColour(hex(0xf4be2a, 0.8f * (1.f - p)));
    g.fillRect(edge - 1.5f, 32.f, 3.f, 324.f);
}

void LiftPanel::paintViewFor(Graphics& g, int view) {
    Graphics::ScopedSaveState s(g);
    const float sc = 600.f / 720.f;
    g.addTransform(AffineTransform::scale(sc).translated(0.f, 32.f + (324.f - 319.f * sc) * 0.5f));
    switch (view) {
    case 5: paintViewBay(g); break;
    case Tape: paintViewTape(g); break;
    case Synth: paintViewSynth(g); break;
    case Drum: paintViewDrum(g); break;
    case Mix: paintViewMix(g); break;
    default: paintViewIn(g); break;
    }
}

// ------------------------------------------------------------------ TAPE

void LiftPanel::paintViewTape(Graphics& g) {
    const Colour cr = hex(0xede6d6);
    const int frames = proc_.uiFrames.load();
    const double pos = proc_.uiPos.load();
    const float frac = frames > 0 ? clamp01(static_cast<float>(pos / frames)) : 0.f;
    const bool playing = proc_.uiPlaying.load();

    // Tape path. Wobbles with the engine's real wow LFO, scaled by depth and
    // tape speed; flat when the tape is still.
    const float amp = reducedMotion_ ? 0.f : juce::jlimit(0.f, 4.5f, an_.wowDepth * an_.speedAbs * 1500.f);
    Path p;
    auto span = [&](juce::Point<float> a, juce::Point<float> b, float ph, bool first) {
        const juce::Point<float> dv = b - a;
        const float len = dv.getDistanceFromOrigin();
        const juce::Point<float> nrm(-dv.y / len, dv.x / len);
        const int n = amp > 0.f ? 14 : 1;
        for (int i = first ? 0 : 1; i <= n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n);
            const float off = amp * std::sin(kPi * u) *
                              (0.7f * std::sin(ph + u * 5.f) + 0.3f * std::sin(static_cast<float>(t_) * 23.f + u * 9.f));
            const juce::Point<float> q = a + dv * u + nrm * off;
            if (first && i == 0) {
                p.startNewSubPath(q);
            } else {
                p.lineTo(q);
            }
        }
    };
    span({200.f, 218.f}, {300.f, 262.f}, an_.wowPhase, true);
    p.lineTo(420.f, 262.f);
    span({420.f, 262.f}, {520.f, 218.f}, an_.wowPhase + 1.3f, false);
    g.setColour(cr);
    g.strokePath(p, juce::PathStrokeType(6.f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    fill(g, circle(300.f, 262.f, 13.f), cr);
    fill(g, circle(420.f, 262.f, 13.f), cr);

    // Head: glows red while recording, brighter with signal; REC LVL sets the
    // glow size and low BIAS adds grit (jitter).
    float hx = 348.f, hy = 246.f;
    if (an_.headGlow > 0.f) {
        const float grit = (1.f - enc_[Tape][1]) * 1.6f * an_.headGlow;
        hx += grit * std::sin(static_cast<float>(t_) * 97.f);
        hy += grit * std::sin(static_cast<float>(t_) * 71.f + 1.f);
        const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(t_) * 9.f);
        const float rad = 4.f + 16.f * enc_[Tape][2];
        for (int k = 3; k >= 1; --k) {
            const float e = rad * static_cast<float>(k) / 3.f;
            g.setColour(hex(0xe8473a, 0.16f * an_.headGlow * pulse));
            g.fillRoundedRectangle(hx - e, hy - e, 24.f + 2.f * e, 24.f + 2.f * e, e);
        }
    }
    g.setColour(rec_ ? hex(0xe8473a) : cr);
    g.fillRect(hx, hy, 24.f, 24.f);

    // The marker above the head hops on every beat while the tape runs
    // (tape clock, placeholder 120 BPM).
    float ty = 0.f, sx = 1.f;
    if (playing && !reducedMotion_) {
        const double beats = pos / kSampleRate * proc_.tempoBpm.load() / 60.0;
        const float fr = static_cast<float>(beats - std::floor(beats));
        const float h = std::pow(juce::jmax(0.f, 1.f - fr * 3.f), 2.f);
        ty = -8.f * h;
        sx = 1.f - 0.18f * h + 0.22f * std::pow(juce::jmax(0.f, 1.f - std::abs(fr - 0.36f) * 12.f), 2.f);
    }
    Path tri;
    tri.addTriangle(360.f, 214.f + ty, 360.f + 14.f * sx, 236.f + ty, 360.f - 14.f * sx, 236.f + ty);
    fill(g, tri, hex(0xf4be2a));

    // LIFT: a strip of the armed track's colour peels off the head and floats up.
    const Colour tc = hex(SCR[arm_]);
    if (!reducedMotion_ && t_ - an_.liftT < 0.7) {
        const float u = static_cast<float>(t_ - an_.liftT) / 0.7f;
        const float rise = 1.f - std::pow(1.f - u, 3.f);
        const float w = 46.f * (1.f - 0.35f * (1.f - u)), h = 8.f + 12.f * (1.f - u);
        g.setColour(tc.withAlpha(1.f - u));
        g.fillRoundedRectangle(360.f - w * 0.5f, 240.f - 120.f * rise - h, w, h, 3.f);
    }
    // DROP: the strip falls back onto the head, bounces and squashes.
    if (!reducedMotion_ && t_ - an_.dropT < 0.8) {
        const float u = static_cast<float>(t_ - an_.dropT);
        const float fall = easeOutBounce(u / 0.5f);
        const float sq = juce::jmax(0.f, kick(u - 0.18, 30.f, 10.f));
        const float w = 46.f * (1.f + 0.4f * sq), h = 10.f * (1.f - 0.4f * sq);
        const float a = u < 0.45f ? 1.f : clamp01(1.f - (u - 0.45f) / 0.35f);
        g.setColour(tc.withAlpha(a));
        g.fillRoundedRectangle(360.f - w * 0.5f, 130.f + 110.f * fall - h, w, h, 3.f);
    }

    auto reel = [&](float x, float pack, float angle) {
        fill(g, circle(x, 130.f, pack), hex(0x2a2925));
        g.setColour(cr);
        // motion ghosts of the spokes when the reels are fast
        const float w = std::abs(an_.omega);
        if (!reducedMotion_ && w > 3.f) {
            for (int gk = 2; gk >= 1; --gk) {
                const AffineTransform gr =
                    AffineTransform::rotation(angle - an_.omega * 0.02f * static_cast<float>(gk)).translated(x, 130.f);
                g.setColour(cr.withAlpha(juce::jmin(0.22f, w * 0.012f) / static_cast<float>(gk)));
                for (int k = 0; k < 3; ++k) {
                    Path sp;
                    sp.addRectangle(-6.f, -78.f, 12.f, 62.f);
                    g.fillPath(sp, AffineTransform::rotation(static_cast<float>(k) * 2.f * kPi / 3.f).followedBy(gr));
                }
            }
            g.setColour(cr);
        }
        const AffineTransform rot = AffineTransform::rotation(angle).translated(x, 130.f);
        Path ring;
        ring.addEllipse(-80.f, -80.f, 160.f, 160.f);
        g.strokePath(ring, juce::PathStrokeType(9.f), rot);
        for (int k = 0; k < 3; ++k) {
            Path sp;
            sp.addRectangle(-6.f, -78.f, 12.f, 62.f);
            g.fillPath(sp, AffineTransform::rotation(static_cast<float>(k) * 2.f * kPi / 3.f).followedBy(rot));
        }
        fill(g, circle(x, 130.f, 20.f), cr);
        fill(g, circle(x, 130.f, 7.f), hex(0x0b0b0b));
    };
    // the packs trade size as tape moves from the left reel to the right one
    reel(200.f, 66.f - 20.f * frac, an_.reelL);
    reel(520.f, 46.f + 20.f * frac, an_.reelR);

    svgText(g, "TAPE", 40.f, 300.f, 13.f, cr, -1, false, 1.f);
    line(g, 200.f, 296.f, 620.f, 296.f, hex(0x3a3833), 3.f);
    if (frac > 0.f) {
        line(g, 200.f, 296.f, 200.f + 420.f * frac, 296.f, proc_.uiRecording.load() ? hex(0xe8473a) : cr, 3.f);
    }
    svgText(g, "0" + juce::String(sel_[Tape] + 1), 680.f, 300.f, 13.f, cr, 1);
}

// ------------------------------------------------------------------ SYNTH

void LiftPanel::paintViewSynth(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 40.f));
    const EngineDef& e = ENGINES[sel_[Synth]];
    const auto& k = enc_[Synth];
    const double sinceEngine = t_ - an_.engineT;
    const float ep = reducedMotion_ ? 1.f : clamp01(static_cast<float>(sinceEngine) / 0.3f);

    // Title: slides in on an engine change, squashes on every note.
    {
        const float b = reducedMotion_ ? 0.f : kick(t_ - juce::jmax(an_.noteT, an_.engineT), 24.f, 8.f);
        const float slide = 30.f * (1.f - easeOutBack(ep));
        const float tw = cssWidth(700, true, 30.f, 0.f, e.n);
        Graphics::ScopedSaveState ss(g);
        if (b != 0.f || slide != 0.f) {
            g.addTransform(AffineTransform::scale(1.f + 0.1f * b, 1.f - 0.14f * b, 24.f + tw * 0.5f, 38.f)
                               .translated(slide, 0.f));
        }
        svgText(g, e.n, 24.f, 38.f, 30.f, hex(0xede6d6), -1, true);
    }
    svgText(g, juce::String::fromUTF8(e.d), 24.f, 58.f, 10.f, hex(0x8c877b), -1, false, 1.f);
    line(g, 24.f, 150.f, 696.f, 150.f, hex(0x262626), 1.f);

    // Waveform. Idle: the engine's drawn shape, breathing gently (PLACEHOLDER
    // MOTION until the real engines exist). With a note: morphs into a live
    // scope of the placeholder voice, triggered on a rising zero crossing and
    // showing three cycles. Engine changes morph old shape into new with an
    // elastic overshoot.
    const float live = reducedMotion_ ? (an_.env > 0.01f ? 1.f : 0.f) : clamp01(an_.env * 4.f);
    const float breathe = reducedMotion_ ? 1.f : 1.f + 0.05f * std::sin(static_cast<float>(t_) * 2.1f);
    int start = 0, span = 0;
    if (live > 0.f && an_.scopeW > 0) {
        const int noteNum = 48 + 12 * oct_ + juce::jmax(0, an_.note);
        const double sr = juce::jmax(1.0, getSampleRateForScope());
        const int period = juce::jlimit(8, 1200, static_cast<int>(sr / (440.0 * std::pow(2.0, (noteNum - 69) / 12.0))));
        span = juce::jmin(screen::kScopeRing - period * 2 - 2, period * 3);
        const int end = an_.scopeW;
        int trig = end - 1;
        auto at = [&](int i) {
            return an_.scope[static_cast<size_t>(((i % screen::kScopeRing) + screen::kScopeRing) % screen::kScopeRing)];
        };
        for (int i = end - 1; i > end - 1 - period * 2; --i) {
            if (at(i - 1) < 0.f && at(i) >= 0.f) {
                trig = i;
                break;
            }
        }
        start = trig - span;
        Path w;
        for (int i = 0; i <= 260; ++i) {
            const double t = i / 260.0;
            const float x = static_cast<float>(24.0 + t * 672.0);
            double idle = waveFn(sel_[Synth], k, t) * breathe;
            const float sv = at(start + static_cast<int>(t * span)) / 0.3f;
            const double y = juce::jmap(static_cast<double>(live), idle, static_cast<double>(juce::jlimit(-1.f, 1.f, sv)));
            const float py = static_cast<float>(150.0 - juce::jlimit(-1.0, 1.0, y) * 70.0);
            if (i == 0) {
                w.startNewSubPath(x, py);
            } else {
                w.lineTo(x, py);
            }
        }
        g.setColour(hex(0xf4be2a));
        g.strokePath(w, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    } else {
        const float morph = ep < 1.f ? easeOutBack(ep, 1.4f) : 1.f;
        Path w;
        for (int i = 0; i <= 260; ++i) {
            const double t = i / 260.0;
            const float x = static_cast<float>(24.0 + t * 672.0);
            double y = waveFn(sel_[Synth], k, t);
            if (morph < 1.f || ep < 1.f) {
                y = juce::jmap(static_cast<double>(morph), waveFn(an_.prevEngine, k, t), y);
            }
            y *= breathe;
            const float py = static_cast<float>(150.0 - juce::jlimit(-1.0, 1.0, y) * 70.0);
            if (i == 0) {
                w.startNewSubPath(x, py);
            } else {
                w.lineTo(x, py);
            }
        }
        g.setColour(hex(0xf4be2a));
        g.strokePath(w, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    // Voice lights pop one after another on each note.
    for (int i = 0; i < 6; ++i) {
        const float pop = reducedMotion_ ? 0.f : juce::jmax(0.f, kick(t_ - an_.noteT - i * 0.035, 22.f, 8.f));
        const float h = 12.f * (1.f + 0.8f * pop), w = 22.f * (1.f - 0.15f * pop);
        const float lv = juce::jlimit(0.f, 1.f, proc_.uiVoiceEnv[i].load() * 1.6f);
        g.setColour(hex(0x262626).interpolatedWith(hex(0xf4be2a), lv > 0.02f ? 0.35f + 0.65f * lv : 0.f));
        g.fillRect(546.f + static_cast<float>(i) * 25.f + (22.f - w) * 0.5f, 32.f - h * 0.5f, w, h);
    }
}

// ------------------------------------------------------------------ DRUM

void LiftPanel::paintViewDrum(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 40.f));
    const int kit = juce::jlimit(0, eng::kKits - 1, sel_[Drum]);
    svgText(g, "KIT " + juce::String(kit + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + eng::kitInfo(kit).name +
                   juce::String::fromUTF8(" \xc2\xb7 ") + eng::kDrumVoiceNames[drumVoice_],
            24.f, 30.f, 18.f, hex(0xede6d6), -1, true);
    const auto& kitPat = drumPat_[static_cast<size_t>(kit)];
    const bool playing = proc_.uiPlaying.load() && !reducedMotion_;
    const float phx = 24.f + static_cast<float>(an_.drumPhase / 16.0) * 672.f;
    // PLACEHOLDER MOTION: the sample and pattern are the prototype's demo
    // content; the playhead itself runs on the real tape clock.
    for (int i = 0; i < 112; ++i) {
        const double hv = (0.12 + 0.88 * std::exp(-(i % 7) / 2.1)) * (0.45 + 0.55 * std::abs(std::sin(i * 1.7))) * 62.0;
        float h = static_cast<float>(juce::jmax(2, static_cast<int>(std::floor(hv + 0.5))));
        const float x = 24.f + static_cast<float>(i) * 6.f;
        Colour c = i % 7 == 0 ? hex(0xede6d6) : hex(0x4c82e6);
        if (playing) {
            const float boost = std::exp(-std::abs(x - phx) / 10.f);
            h *= 1.f + 0.35f * boost;
            c = c.interpolatedWith(hex(0xede6d6), boost);
        }
        g.setColour(c);
        g.fillRect(x, 77.f - h / 2.f, 3.f, h);
    }
    for (int i = 1; i < 24; ++i) {
        const float fl = playing ? decayFrom(t_ - an_.sliceT[static_cast<size_t>(i)], 7.f) : 0.f;
        line(g, 24.f + i * 28.f, 42.f - 4.f * fl, 24.f + i * 28.f, 112.f + 4.f * fl, hex(0xf4be2a, 0.45f + 0.55f * fl),
             1.f + 1.5f * fl);
    }
    if (playing) {
        line(g, phx, 40.f, phx, 114.f, hex(0xede6d6, 0.85f), 2.f);
    }
    const int cur = static_cast<int>(an_.drumPhase) % 16;
    for (int i = 0; i < 16; ++i) {
        const float x = 24.f + i * 42.4f;
        const float b = playing ? juce::jmax(0.f, kick(t_ - an_.hitT[static_cast<size_t>(i)], 26.f, 9.f)) : 0.f;
        const Rectangle<float> cell = Rectangle<float>(x, 142.f, 36.f, 36.f).withSizeKeepingCentre(36.f * (1.f + 0.22f * b),
                                                                                              36.f * (1.f - 0.12f * b));
        const bool on = ((kitPat[static_cast<size_t>(drumVoice_)] >> i) & 1u) != 0;
        g.setColour(on ? rgba(232, 71, 58, 0.3f + 0.6f * b) : hex(0x141414));
        g.fillRect(cell);
        g.setColour(on ? hex(0xe8473a) : (i % 4 == 0 ? hex(0x444444) : hex(0x262626)));
        g.drawRect(cell.expanded(0.5f), 1.f);
        if (playing && i == cur) {
            g.setColour(hex(0xede6d6));
            g.drawRect(cell.expanded(3.f), 2.f);
        }
        if (on) {
            int busy = 0;  // how much of the kit lands on this step
            for (uint32_t m : kitPat) {
                busy += static_cast<int>((m >> i) & 1u);
            }
            const float vel = juce::jmin(1.f, 0.35f + 0.16f * static_cast<float>(busy));
            const float v = vel * (1.f + (playing ? 0.9f * decayFrom(t_ - an_.hitT[static_cast<size_t>(i)], 8.f) : 0.f));
            g.setColour(hex(0xe8473a));
            g.fillRect(x, 184.f, 36.f, static_cast<float>(juce::jmax(2, juce::roundToInt(v * 12.f))));
        }
    }
}

// ------------------------------------------------------------------ MIX

void LiftPanel::paintViewMix(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 52.f));
    for (int t = 0; t < 4; ++t) {
        const size_t ti = static_cast<size_t>(t);
        const float x = 60.f + t * 110.f;
        const float v = an_.fader[ti].x;
        const float p = an_.pan[ti].x;
        const float cy = settle(an_.fader[ti], 176.f - v * 126.f);
        svgText(g, "T" + juce::String(t + 1), x, 22.f, 12.f, hex(SCR[t]), 0, true);
        line(g, x - 18.f, 34.f, x + 18.f, 34.f, hex(0x333333), 3.f);
        fill(g, circle(x - 18.f + p * 36.f, 34.f, 4.f), hex(SCR[t]));
        line(g, x, 50.f, x, 176.f, hex(0x333333), 4.f);
        line(g, x, cy, x, 176.f, hex(SCR[t]), 4.f);
        // live meter: the track's tape signal under the head, with peak hold
        if (an_.meter[ti] > 0.f || an_.peak[ti] > 0.f) {
            const float mh = 126.f * clamp01(an_.meter[ti] * 1.6f);
            g.setColour(hex(SCR[t], 0.85f));
            g.fillRect(x + 12.f, 176.f - mh, 3.f, mh);
            g.setColour(hex(0xede6d6));
            g.fillRect(x + 11.f, 176.f - 126.f * clamp01(an_.peak[ti] * 1.6f) - 1.f, 5.f, 2.f);
        }
        g.setColour(hex(0xede6d6));
        g.fillRect(x - 18.f, cy - 5.f, 36.f, 10.f);
        const float mb = reducedMotion_ ? 0.f : kick(t_ - an_.muteT[ti], 28.f, 10.f);
        Graphics::ScopedSaveState ss(g);
        if (mb != 0.f) {
            g.addTransform(AffineTransform::scale(1.f + 0.25f * mb, 1.f - 0.25f * mb, x, 195.f));
        }
        if (mutes_[ti]) {
            g.setColour(hex(0xf4be2a));
            g.fillRect(x - 12.f, 186.f, 24.f, 18.f);
        }
        g.setColour(hex(0x444444));
        g.drawRect(x - 12.5f, 185.5f, 25.f, 19.f, 1.f);
        svgText(g, "M", x, 199.f, 9.f, mutes_[ti] ? hex(0x0a0a0a) : hex(0x8c877b), 0);
    }
    svgText(g, "MST", 500.f, 22.f, 12.f, hex(0xede6d6), 0, true);
    line(g, 500.f, 50.f, 500.f, 176.f, hex(0x333333), 4.f);
    line(g, 500.f, 74.f, 500.f, 176.f, hex(0xede6d6), 4.f);
    if (an_.meter[4] > 0.f || an_.peak[4] > 0.f) {
        const float mh = 126.f * clamp01(an_.meter[4]);
        g.setColour(hex(0xede6d6, 0.85f));
        g.fillRect(512.f, 176.f - mh, 3.f, mh);
        g.setColour(hex(0xe8473a));
        g.fillRect(511.f, 176.f - 126.f * clamp01(an_.peak[4]) - 1.f, 5.f, 2.f);
    }
    g.setColour(hex(0xede6d6));
    g.fillRect(482.f, 69.f, 36.f, 10.f);
    g.setColour(hex(0x262626));
    g.drawRect(559.5f, 33.5f, 137.f, 111.f, 1.f);
    // EQ curve: springs to LOW/HIGH, the mid bump breathes with the master level
    const float lo = an_.eqLo.x, hi = an_.eqHi.x;
    const float mid = -10.f * an_.meter[4];
    Path eq;
    eq.startNewSubPath(560.f, 90.f - (lo - 0.5f) * 40.f);
    eq.cubicTo(584.f, 88.f - (lo - 0.5f) * 40.f, 596.f, 80.f + mid, 610.f, 89.f + mid);
    eq.cubicTo(628.f, 100.f + mid, 646.f, 98.f + mid, 662.f, 86.f + mid);
    eq.cubicTo(674.f, 78.f, 686.f, 90.f - (hi - 0.5f) * 40.f, 696.f, 90.f - (hi - 0.5f) * 44.f);
    g.setColour(hex(0xf4be2a));
    g.strokePath(eq, juce::PathStrokeType(2.f));
    svgText(g, juce::String::fromUTF8("LOW \xc2\xb7 MID \xc2\xb7 HIGH"), 628.f, 166.f, 9.f, hex(0x8c877b), 0, false, 1.f);
}

// ------------------------------------------------------------------ IN

void LiftPanel::paintViewIn(Graphics& g) {
    g.addTransform(AffineTransform::translation(0.f, 46.f));
    const auto& k = enc_[In];
    const int st = juce::jmin(4, static_cast<int>(std::floor(k[0] * 5.f)));
    const juce::String name = sel_[In] == 2
                                  ? juce::String::fromUTF8("RADIO \xc2\xb7 STATION ") + juce::String(st + 1).paddedLeft('0', 2)
                                  : juce::String::fromUTF8("INPUT \xc2\xb7 ") + INPUTS[sel_[In]];
    svgText(g, name, 24.f, 30.f, 18.f, hex(0xede6d6), -1, true);
    g.setColour(hex(0x121212));
    g.fillRect(24.f, 48.f, 672.f, 78.f);
    g.setColour(hex(0x262626));
    g.drawRect(23.5f, 47.5f, 673.f, 79.f, 1.f);
    // dial needle eases to STATION with overshoot; ticks it passes lean up
    const float nx = settle(an_.needle, 40.f + an_.needle.x * 640.f);
    const bool moving = an_.needle.v != 0.f;
    for (int i = 0; i <= 41; ++i) {
        const float tx = 40.f + i * 16.f;
        const float lift = moving ? 6.f * std::exp(-std::abs(tx - nx) / 14.f) : 0.f;
        line(g, tx, (i % 5 == 0 ? 82.f : 92.f) - lift, tx, 104.f, hex(0x4a4740), 1.f);
    }
    for (int i = 0; i < 5; ++i) {
        const float lx = 104.f + i * 128.f;
        const float prox = moving ? clamp01(1.f - std::abs(nx - lx) / 64.f) : 0.f;
        const Colour c = i == st ? hex(0xede6d6) : hex(0x5e5a50).interpolatedWith(hex(0xede6d6), prox);
        svgText(g, "STATION " + juce::String(i + 1).paddedLeft('0', 2), lx, 72.f, 10.f, c, 0);
    }
    line(g, nx, 54.f, nx, 120.f, hex(0xe8473a), 3.f);
    g.setColour(hex(0x1a1a1a));
    g.fillRect(64.f, 146.f, 560.f, 12.f);
    g.fillRect(64.f, 170.f, 560.f, 12.f);
    // PLACEHOLDER MOTION: no input path yet; levels wander around the design's
    // 92 % / 84 % so the meters read like a live signal. GAIN is real (knob).
    const float gn = an_.gain.x;
    g.setColour(hex(0x4c82e6));
    g.fillRect(64.f, 146.f, static_cast<float>(juce::roundToInt(560.f * gn * an_.inL * (reducedMotion_ ? 1.f : 0.92f / 0.92f))), 12.f);
    g.fillRect(64.f, 170.f, static_cast<float>(juce::roundToInt(560.f * gn * an_.inR)), 12.f);
    // threshold line pulses when the level crosses it
    const float tx = static_cast<float>(juce::roundToInt(64.f + k[3] * 560.f));
    const float pulse = reducedMotion_ ? 0.f : decayFrom(t_ - an_.threshT, 6.f);
    if (pulse > 0.01f) {
        g.setColour(hex(0xe8473a, 0.25f * pulse));
        g.fillRect(tx - 6.f * pulse - 1.f, 140.f, 12.f * pulse + 2.f, 48.f);
    }
    {
        Path l;
        l.startNewSubPath(tx, 140.f);
        l.lineTo(tx, 188.f);
        Path d;
        const float dash[2] = {3.f, 2.f};
        juce::PathStrokeType(2.f + 2.f * pulse).createDashedStroke(d, l, dash, 2);
        fill(g, d, hex(0xe8473a).interpolatedWith(hex(0xede6d6), 0.5f * pulse));
    }
    svgText(g, "L", 24.f, 156.f, 9.f, hex(0x8c877b));
    svgText(g, "R", 24.f, 180.f, 9.f, hex(0x8c877b));
}

// ------------------------------------------------------------------ BAY

// The audit view: everything patched in one list, cables first (oldest
// first), then pins by address, then the normals still in use (dimmed).
// Only links that close a true cycle in the graph carry FEEDBACK z^-1.
void LiftPanel::paintViewBay(Graphics& g) {
    const auto& plan = proc_.patch.plan();
    int nPins = 0;
    for (auto p : pins_) nPins += p != 0 ? 1 : 0;
    const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
    svgText(g, "CORDS " + juce::String(static_cast<int>(cords_.size())) + dot + "PINS " + juce::String(nPins) + dot + "INPUTS SUM",
            24.f, 26.f, 11.f, hex(0x8c877b), -1, false, 1.f);
    svgText(g, "32-SAMPLE BLOCK", 696.f, 26.f, 11.f, hex(0x8c877b), 1, false, 1.f);
    struct Item {
        juce::String a, b, tag, amt;
        juce::Colour sw;
        bool dim = false, fb = false, cable = false;
        int k = 0;
    };
    std::vector<Item> items;
    items.reserve(cords_.size() + static_cast<size_t>(nPins) + 10);
    for (size_t k = 0; k < cords_.size(); ++k) {
        const Cord& c = cords_[k];
        Item it;
        it.a = juce::String::fromUTF8(OUTS[c.o].n);
        it.b = juce::String::fromUTF8(INS[c.i].n);
        it.sw = hex(CLOTH[c.c].c);
        it.fb = eng::cableIsFeedback(plan, c.o, c.i);
        it.amt = "100%";
        it.cable = true;
        it.k = static_cast<int>(k);
        items.push_back(it);
    }
    static const char* kAmt[4] = {"", "+100%", "+50%", "\xe2\x88\x92" "100%"};
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            const int v = pin(r, c);
            if (v == 0) continue;
            Item it;
            it.tag = "PIN " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + r)) + juce::String(c + 1);
            it.a = juce::String::fromUTF8(MXR[r].n);
            it.b = juce::String::fromUTF8(MXC[c].n);
            it.sw = v == 1 ? hex(0xede6d6) : v == 2 ? hex(0x8c877b) : hex(0x2a2a2a);
            it.fb = eng::pinIsFeedback(plan, r, c);
            it.amt = juce::String::fromUTF8(kAmt[v]);
            items.push_back(it);
        }
    }
    // normals still in use
    auto cabled = [&](int in) {
        for (const auto& c : cords_) {
            if (c.i == in) return true;
        }
        return false;
    };
    auto colPinned = [&](int c, bool pitchOnly) {
        for (int r = 0; r < 16; ++r) {
            if (pin(r, c) != 0 && (!pitchOnly || MXR[r].sym == 'p')) return true;
        }
        return false;
    };
    auto normal = [&](const char* from, const char* to) {
        Item it;
        it.a = juce::String::fromUTF8(from);
        it.b = juce::String::fromUTF8(to);
        it.tag = "NORMAL";
        it.dim = true;
        it.sw = hex(0x3a3a36);
        items.push_back(it);
    };
    if (!cabled(eng::I_PITCH) && !colPinned(0, true)) normal("A PITCH", "PITCH");
    if (!cabled(eng::I_GATE) && !colPinned(9, false)) normal("A GATE", "GATE");
    if (!cabled(eng::I_QUANT)) normal("S&H", "QUANT");
    if (!cabled(eng::I_SH)) normal("NOISE", "S&H");
    if (!cabled(eng::I_CLK)) normal("INTERNAL CLOCK", "CLK IN");
    if (!cabled(eng::I_REVERSE)) normal("REV KNOB", "REVERSE");
    if (!cabled(eng::I_AUDIOL)) normal("INTERFACE 1 / 2", "AUDIO L / R");
    if (!colPinned(15, false)) normal("C16 ENV", "VCA");
    if (!colPinned(10, false)) normal("H11 VEL", "LEVEL");

    constexpr int kRows = 13;
    const int n = juce::jmin(kRows, static_cast<int>(items.size()));
    const float tt = static_cast<float>(t_);
    for (int k = 0; k < n; ++k) {
        const Item& it = items[static_cast<size_t>(k)];
        const float y = 40.f + static_cast<float>(k) * 21.f;
        // rows deal in from the left, one after another, when BAY opens
        const float rp = reducedMotion_ ? 1.f : clamp01((static_cast<float>(t_ - an_.bayT) - static_cast<float>(k) * 0.03f) / 0.32f);
        if (rp <= 0.f) {
            continue;
        }
        Graphics::ScopedSaveState ss(g);
        if (rp < 1.f) {
            g.addTransform(AffineTransform::translation(-60.f * (1.f - easeOutBack(rp, 1.6f)), 0.f));
        }
        if (it.cable && it.k == static_cast<int>(cords_.size()) - 1 && !reducedMotion_) {
            const float f = decayFrom(t_ - an_.cordT, 4.f);
            if (f > 0.01f) {
                g.setColour(it.sw.withMultipliedAlpha(0.35f * f));
                g.fillRect(18.f, y - 4.f, 684.f, 20.f);
            }
        }
        const juce::Colour fg = it.dim ? hex(0x6a665c) : hex(0xede6d6);
        if (it.cable) {
            g.setColour(it.sw);
            g.fillRect(24.f, y, 12.f, 12.f);
            g.setColour(hex(0xede6d6, 0.3f));
            g.drawRect(23.5f, y - 0.5f, 13.f, 13.f, 1.f);
        } else {
            fill(g, circle(30.f, y + 6.f, 5.5f), it.sw);
            if (!it.dim) {
                g.setColour(hex(0xede6d6, 0.5f));
                g.drawEllipse(24.5f, y + 0.5f, 11.f, 11.f, 1.f);
            }
        }
        if (it.tag.isNotEmpty()) {
            svgText(g, it.tag, 46.f, y + 10.f, 10.f, it.dim ? hex(0x4e4b44) : hex(0x8c877b));
        }
        const float x0 = it.cable ? 46.f : 118.f;
        svgText(g, it.a, x0, y + 11.f, 12.f, fg);
        svgText(g, juce::String::fromUTF8("\xe2\x86\x92"), 290.f, y + 11.f, 12.f, hex(0x5e5a50));
        svgText(g, it.b, 318.f, y + 11.f, 12.f, fg);
        if (it.cable) {
            // signal dots travel from the OUT to the IN, faster with the OUT jack's level
            const Cord& c = cords_[static_cast<size_t>(it.k)];
            const float lvl = juce::jlimit(0.f, 1.f, proc_.uiJackLevel[juce::jlimit(0, 15, static_cast<int>(c.o))].load());
            if (!reducedMotion_ && lvl > 0.002f) {
                const float rate = 0.35f + 1.2f * std::sqrt(lvl);
                for (int j = 0; j < 2; ++j) {
                    float ph = tt * rate + static_cast<float>(k) * 0.17f + static_cast<float>(j) * 0.5f;
                    ph -= std::floor(ph);
                    const float a = std::sin(kPi * ph);
                    fill(g, circle(214.f + 64.f * ph, y + 6.f, 2.2f), it.sw.brighter(0.3f).withAlpha((0.35f + 0.55f * std::sqrt(lvl)) * a));
                }
            }
        }
        if (it.fb) {
            const float pulse = reducedMotion_ ? 1.f : 0.6f + 0.4f * (0.5f + 0.5f * std::sin(tt * 6.f));
            svgText(g, juce::String::fromUTF8("FEEDBACK z\xe2\x81\xbb\xc2\xb9"), 610.f, y + 11.f, 10.f, hex(0xf4be2a, pulse), 1);
        }
        if (it.amt.isNotEmpty()) {
            svgText(g, it.amt, 696.f, y + 11.f, 12.f, hex(0x8c877b), 1);
        }
    }
    if (cords_.empty() && nPins == 0) {
        svgText(g, juce::String::fromUTF8("NOTHING PATCHED \xc2\xb7 DRAG FROM AN OUT TO AN IN, OR CLICK A PIN"), 24.f, 330.f, 11.f,
                hex(0x8c877b));
    }
    const int more = static_cast<int>(items.size()) - n;
    const juce::String foot = (more > 0 ? "+" + juce::String(more) + " MORE" + dot : juce::String()) + "SOFT CLIP + DC BLOCK ON EVERY LOOP";
    svgText(g, foot, 696.f, 322.f, 10.f, hex(0x5e5a50), 1, false, 1.f);
}

// ------------------------------------------------------------------ foot

void LiftPanel::paintFoot(Graphics& g) {
    const auto L = labels();
    const float cw = (568.f - 42.f) / 4.f;
    for (int i = 0; i < 4; ++i) {
        const size_t ii = static_cast<size_t>(i);
        const float x = 16.f + static_cast<float>(i) * (cw + 14.f);
        const juce::Font f = mono(false, 9.f, 0.06f);
        text(g, L[slotOf(i)], f, hex(KSCR[i]), {x, 364.f, cw, 13.5f}, Justification::centredLeft);
        const juce::String v = encDisplay(slotOf(i));
        const float vw = cssWidth(400, true, 9.f, 0.06f, v);
        text(g, v, f, hex(0xede6d6), {x + cw - vw, 364.f, vw + 6.f, 13.5f}, Justification::centredLeft);
        g.setColour(hex(0x1c1c1c));
        g.fillRect(x, 383.5f, cw, 6.f);
        // bars spring to the knob (and to the new screen's knobs on a mode change)
        const Spring& s = an_.foot[ii];
        const float target = encAt(slotOf(i));
        const bool rest = !s.init || (s.v == 0.f && s.x == target);
        const float w = rest ? cw * static_cast<float>(juce::roundToInt(target * 100.f)) / 100.f
                             : cw * juce::jlimit(0.f, 1.04f, s.x);
        g.setColour(hex(KSCR[i]));
        g.fillRect(x, 383.5f, w, 6.f);
    }
}

}  // namespace lift
