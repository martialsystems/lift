// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#include "MiniUi.h"
#include "Fonts.h"

namespace mini {
using namespace juce;

namespace col {
const Colour bg(0xff15140f), ink(0xfff1ebdc), dim(0xff6d675b);
const Colour knob[4] = {Colour(0xffe8b923), Colour(0xff3d6fc4), Colour(0xfff1ebdc), Colour(0xffd6402b)};
const Colour rec(0xffd6402b);
}  // namespace col

static const char* kModeName[] = {"SYNTH", "DRUM", "TAPE", "MIX"};
static const char* kKnobNames[4][4] = {
    {"PITCH", "TONE", "AMOUNT", "TIME"},      // SYNTH macros
    {"PITCH", "CHOKE", "DECAY", "SLICE"},     // DRUM
    {"SPEED", "BIAS", "REC LVL", "POSITION"}, // TAPE
    {"LEVEL", "PAN", "LOW", "HIGH"}};         // MIX

MiniUi::MiniUi(lift::LiftProcessor& p, int fps) : proc_(p) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    for (auto& s : knob_) for (auto& k : s) k = 0.5f;
    startTimerHz(jlimit(5, 60, fps));
}
MiniUi::~MiniUi() { proc_.send(lift::Cmd::NoteOff, -1); }

int MiniUi::screenIndex() const {
    // LiftProcessor knob screens: 0 TAPE, 1 SYNTH, 2 DRUM, 3 MIX, 4 IN (panel/MIDI.md)
    switch (mode_) { case Tape: return 0; case Synth: return 1; case Drum: return 2; default: return 3; }
}

void MiniUi::handle(const Event& e) {
    switch (e.kind) {
        case Event::Key: key(e.index, e.value != 0); break;
        case Event::Button: button(Btn(e.index), e.value != 0); break;
        case Event::Turn: turn(e.index, e.value); break;
        case Event::Push: push(e.index, e.value != 0); break;
    }
    repaint();
}

void MiniUi::button(Btn b, bool down) {
    if (b == Btn::Shift) { shift_ = down; return; }
    if (!down) return;
    auto flash = [this](String s) { flash_ = std::move(s); flashUntil_ = Time::getMillisecondCounterHiRes() + 1200; };
    switch (b) {
        case Btn::Synth: case Btn::Drum: case Btn::Tape: case Btn::Mix: {
            const auto m = Mode(int(b));
            page_ = (m == mode_) ? (page_ + 1) % 4 : 0;   // tap again: next page
            mode_ = m;
            break;
        }
        case Btn::OctDown: if (shift_) proc_.send(lift::Cmd::UndoDrop); else octave_ = jmax(-3, octave_ - 1); break;
        case Btn::OctUp: if (shift_) flash("REDO: not in the engine yet"); else octave_ = jmin(3, octave_ + 1); break;
        case Btn::PlayStop:
            if (shift_) proc_.send(lift::Cmd::Seek, 0, 0, 0.0);
            else proc_.send(lift::Cmd::Transport, proc_.uiPlaying.load() ? 0 : 1, 0);
            break;
        case Btn::Rec:
            proc_.send(lift::Cmd::Arm, track_);
            proc_.send(lift::Cmd::Transport, 1, proc_.overdubbing() ? 0 : 1);
            break;
        case Btn::Lift:
            clip_ = shift_ ? proc_.keepTracks() : proc_.keepLast(proc_.liftBack.load());
            if (clip_) { proc_.addToPool(clip_); flash("LIFT  " + String(clip_->frames() / 48000.0, 1) + " S"); }
            break;
        case Btn::Drop:
            if (!clip_) { flash("NOTHING TO DROP: LIFT FIRST"); break; }
            if (mode_ == Tape || mode_ == Mix) { proc_.dropToLoop(clip_, track_); flash("DROP  T" + String(track_ + 1)); }
            else if (mode_ == Synth) { proc_.setKeysClip(clip_); flash("DROP  KEYS"); }
            else { proc_.setSliceKit(clip_, 24); flash("DROP  24 SLICES"); }
            break;
        default: break;
    }
}

void MiniUi::key(int k, bool down) {
    if (shift_ && down && k < 8) { proc_.saveSlotAsync(k + 1, nullptr); flash_ = "SAVED SLOT " + String(k + 1);
        flashUntil_ = Time::getMillisecondCounterHiRes() + 1200; return; }
    if (mode_ == Drum) {
        if (down) {
            if (proc_.sliceKitLoaded()) proc_.send(lift::Cmd::SliceHit, k, 110);
            else proc_.send(lift::Cmd::DrumHit, k % lift::eng::kDrumVoices, 110);
        }
        return;
    }
    const int note = 48 + octave_ * 12 + k;
    proc_.send(down ? lift::Cmd::NoteOn : lift::Cmd::NoteOff, note, 100);
}

void MiniUi::turn(int e, int detents) {
    const int s = screenIndex();
    const float step = shift_ ? 0.005f : 0.02f;
    float& v = knob_[s][e];
    v = jlimit(0.f, 1.f, v + step * float(detents));
    proc_.setKnobValue(s, e, v);
}

void MiniUi::push(int e, bool down) {
    if (!down) return;
    if (mode_ == Tape) { track_ = e; proc_.send(lift::Cmd::Arm, e); }
    else if (mode_ == Mix) { mute_[e] = !mute_[e]; proc_.send(lift::Cmd::Mute, e, mute_[e] ? 1 : 0); }
    else if (shift_) { const int s = screenIndex(); knob_[s][e] = 0.5f; proc_.setKnobValue(s, e, 0.5f); }
}

bool MiniUi::keyPressed(const KeyPress& k) {
    if (keyboardEvent(k, true, [this](const Event& e) { handle(e); })) {
        heldKeys_.addIfNotAlreadyThere(k.getKeyCode());
        return true;
    }
    return false;
}

bool MiniUi::keyStateChanged(bool) {
    for (int i = heldKeys_.size(); --i >= 0;) {
        const int code = heldKeys_[i];
        if (!KeyPress::isKeyCurrentlyDown(code)) {
            heldKeys_.remove(i);
            keyboardEvent(KeyPress(code), false, [this](const Event& e) { if (e.kind != Event::Turn) handle(e); });
        }
    }
    return false;
}

void MiniUi::timerCallback() {
    const int n = proc_.readScope(scopeBuf_.data(), int(scopeBuf_.size()));
    if (n > 0) {
        const int stride = jmax(1, n / kW);
        for (int i = 0; i < kW; ++i) scope_[i] = scopeBuf_[size_t(jmin(n - 1, i * stride))];
    }
    proc_.drainMidiEvents();
    repaint();
}

void MiniUi::paint(Graphics& g) {
    g.fillAll(Colours::black);
    const float sc = jmin(getWidth() / float(kW), getHeight() / float(kH));
    g.addTransform(AffineTransform::scale(sc).translated((getWidth() - kW * sc) * 0.5f, (getHeight() - kH * sc) * 0.5f));
    g.setColour(col::bg);
    g.fillRect(0, 0, kW, kH);

    // status bar
    g.setFont(lift::ui::jost(600, 13.f, 0.06f));
    g.setColour(col::ink);
    g.drawText(String(kModeName[mode_]) + "  " + String(page_ + 1) + "/4", 10, 4, 160, 18, Justification::left);
    g.setColour(proc_.clockSlaved.load() ? col::ink : col::dim);
    g.drawText(String(proc_.tempoBpm.load(), 1) + " BPM", 170, 4, 90, 18, Justification::centred);
    g.setColour(col::dim);
    g.drawText("OCT " + String(octave_ >= 0 ? "+" : "") + String(octave_) + (shift_ ? "  SHIFT" : ""), 260, 4, 130, 18, Justification::centred);
    if (proc_.uiRecording.load()) { g.setColour(col::rec); g.fillEllipse(396.f, 8.f, 10.f, 10.f); }
    g.setColour(proc_.uiPlaying.load() ? col::ink : col::dim);
    g.drawText(proc_.uiPlaying.load() ? "PLAY" : "STOP", 410, 4, 60, 18, Justification::right);

    const Rectangle<float> body(8.f, 26.f, kW - 16.f, 186.f);
    switch (mode_) {
        case Tape: paintTape(g, body); break;
        case Synth: paintSynth(g, body); break;
        case Drum: paintDrum(g, body); break;
        case Mix: paintMix(g, body); break;
    }
    paintFoot(g, {0.f, 214.f, float(kW), 58.f});

    const auto now = Time::getMillisecondCounterHiRes();
    const String msg = now < flashUntil_ ? flash_ : status_;
    if (msg.isNotEmpty()) {
        g.setColour(col::bg.withAlpha(0.85f));
        g.fillRect(0.f, 190.f, float(kW), 22.f);
        g.setColour(col::ink);
        g.drawText(msg, 10, 190, kW - 20, 22, Justification::centredLeft);
    }
}

void MiniUi::paintTape(Graphics& g, Rectangle<float> r) {
    const int frames = jmax(1, proc_.uiFrames.load());
    const float head = float(proc_.uiPos.load() / frames);
    const float ls = proc_.uiLoopStart.load() / float(frames), le = proc_.uiLoopEnd.load() / float(frames);
    if (le > ls) { g.setColour(col::ink.withAlpha(0.08f)); g.fillRect(r.getX() + ls * r.getWidth(), r.getY(), (le - ls) * r.getWidth(), r.getHeight()); }
    const float h = r.getHeight() / 4.f;
    for (int t = 0; t < 4; ++t) {
        auto row = Rectangle<float>(r.getX(), r.getY() + t * h, r.getWidth(), h).reduced(0.f, 5.f);
        g.setColour(t == track_ ? col::knob[t] : col::dim);
        g.drawRect(row, t == track_ ? 2.f : 1.f);
        g.setFont(lift::ui::jost(600, 12.f, 0.06f));
        g.drawText("T" + String(t + 1) + (mute_[t] ? " M" : ""), row.removeFromLeft(36.f), Justification::centred);
        const float lvl = jlimit(0.f, 1.f, proc_.uiTrackLevel[t].load());
        g.setColour(col::knob[t].withAlpha(0.7f));
        g.fillRect(row.getX(), row.getCentreY() - lvl * row.getHeight() * 0.45f, 6.f, lvl * row.getHeight() * 0.9f);
    }
    g.setColour(proc_.uiRecording.load() ? col::rec : col::ink);
    const float x = r.getX() + 36.f + head * (r.getWidth() - 36.f);
    g.drawLine(x, r.getY(), x, r.getBottom(), 2.f);
}

void MiniUi::paintSynth(Graphics& g, Rectangle<float> r) {
    g.setColour(col::dim);
    g.drawHorizontalLine(int(r.getCentreY()), r.getX(), r.getRight());
    Path p;
    for (int i = 0; i < kW - 16; ++i) {
        const float y = r.getCentreY() - jlimit(-1.f, 1.f, scope_[i] * 1.5f) * r.getHeight() * 0.45f;
        if (i == 0) p.startNewSubPath(r.getX(), y); else p.lineTo(r.getX() + float(i), y);
    }
    g.setColour(col::knob[0]);
    g.strokePath(p, PathStrokeType(1.5f));
    for (int v = 0; v < lift::eng::kSynthVoices; ++v) {
        g.setColour(col::knob[0].withAlpha(0.2f + 0.8f * jlimit(0.f, 1.f, proc_.uiVoiceEnv[v].load())));
        g.fillEllipse(r.getRight() - 14.f * float(lift::eng::kSynthVoices - v), r.getY(), 9.f, 9.f);
    }
    if (clip_) { g.setColour(col::dim); g.setFont(lift::ui::jost(400, 12.f)); g.drawText("CLIP READY: DROP plays it on the keys", r.toNearestInt(), Justification::bottomLeft); }
}

void MiniUi::paintDrum(Graphics& g, Rectangle<float> r) {
    const float w = r.getWidth() / 12.f, h = r.getHeight() / 2.f;
    for (int k = 0; k < 24; ++k) {
        auto cell = Rectangle<float>(r.getX() + (k % 12) * w, r.getY() + (k < 12 ? h : 0.f), w, h).reduced(3.f);
        const float env = k < lift::eng::kDrumVoices ? jlimit(0.f, 1.f, proc_.uiDrumEnv[k].load()) : 0.f;
        g.setColour(col::knob[3].withAlpha(0.15f + 0.85f * env));
        g.fillRect(cell);
        g.setColour(col::dim);
        g.drawRect(cell, 1.f);
    }
    const int step = proc_.uiDrumStep.load();
    if (step >= 0) { g.setColour(col::ink); g.fillRect(r.getX() + (step % 16) * r.getWidth() / 16.f, r.getBottom() - 4.f, r.getWidth() / 16.f, 4.f); }
}

void MiniUi::paintMix(Graphics& g, Rectangle<float> r) {
    const float w = r.getWidth() / 5.f;
    for (int t = 0; t < 5; ++t) {
        auto strip = Rectangle<float>(r.getX() + t * w, r.getY(), w, r.getHeight()).reduced(12.f, 4.f);
        const float lvl = jlimit(0.f, 1.f, t < 4 ? proc_.uiTrackLevel[t].load() : proc_.uiMasterLevel.load());
        g.setColour(col::dim); g.drawRect(strip, 1.f);
        g.setColour(t < 4 ? (mute_[t] ? col::dim : col::knob[t]) : col::ink);
        g.fillRect(strip.withTop(strip.getBottom() - lvl * strip.getHeight()));
        g.setColour(col::ink); g.setFont(lift::ui::jost(600, 12.f, 0.06f));
        g.drawText(t < 4 ? "T" + String(t + 1) : String("MAIN"), strip.toNearestInt().withHeight(16), Justification::centred);
    }
}

void MiniUi::paintFoot(Graphics& g, Rectangle<float> r) {
    const int s = screenIndex();
    const float w = r.getWidth() / 4.f;
    for (int k = 0; k < 4; ++k) {
        auto cell = Rectangle<float>(r.getX() + k * w, r.getY(), w, r.getHeight()).reduced(6.f, 6.f);
        g.setColour(col::knob[k]);
        g.fillRect(cell.removeFromBottom(6.f).withWidth(cell.getWidth() * knob_[s][k]));
        g.setFont(lift::ui::jost(600, 12.f, 0.06f));
        g.drawText(kKnobNames[mode_][k], cell.removeFromTop(18.f).toNearestInt(), Justification::centredLeft);
        g.setColour(col::ink);
        g.setFont(lift::ui::mono(true, 14.f));
        g.drawText(String(roundToInt(knob_[s][k] * 100.f)), cell.toNearestInt(), Justification::centredLeft);
    }
}

}  // namespace mini
