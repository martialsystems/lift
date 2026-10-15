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

static const ScreenLayout& L = kScreen;
static const char* kModeName[] = {"SYNTH", "DRUM", "TAPE", "MIX", "RADIO"};
static const char* kRadioNames[4] = {"TUNE", "PRESET", "LEVEL", "MONO"};  // RADIO (sheet 4.5)
static const char* kSources[] = {"INTERNAL", "RADIO", "COMPUTER", "MASTER"};
static constexpr int kNumSources = 4;

static Font label(float scale = 1.f) { return lift::ui::jost(600, L.font * scale, 0.06f); }

MiniUi::MiniUi(lift::LiftProcessor& p, int fps) : proc_(p) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    for (auto& m : val_) for (auto& pg : m) for (auto& sd : pg) for (auto& k : sd) k = 0.5f;
    proc_.bipolarSpeed.store(true);           // the yellow knob runs reverse <-> stop <-> forward
    proc_.perTrackMix.store(true);            // MIX: one level and pan per track (sheet 4.4)
    val_[Tape][0][0][0] = 0.5f + 0.5f * (0.04f + 0.96f * 0.6f);  // 1x forward on the knob's curve
    val_[Tape][0][1][1] = 0.f;                // CHARACTER: DECK
    val_[Tape][0][1][3] = 0.f;                // LOOP: off
    for (int t = 0; t < 4; ++t) val_[Mix][0][0][t] = 0.72f;   // unity
    val_[Mix][2][0][0] = tempoToKnob(proc_.tempoBpm.load());
    proc_.signedSpeed.store(bipolarKnobToSpeed(val_[Tape][0][0][0]));
    proc_.setListener(this);
    startTimerHz(jlimit(5, 60, fps));
}
MiniUi::~MiniUi() {
    proc_.setListener(nullptr);
    proc_.send(lift::Cmd::NoteOff, -1);
}

void MiniUi::boot() {
    if (!proc_.reopenLast()) newProject();
}

// A new project: SYNTH, LOOM, 100 BPM, no loop (LIFT-Mini decisions)
void MiniUi::newProject() {
    mode_ = Synth;
    page_ = 0;
    proc_.synthEngine.store(lift::eng::LOOM);
    proc_.tempoBpm.store(100.0);
    val_[Mix][2][0][0] = tempoToKnob(100.0);
    val_[Tape][0][1][3] = 0.f;
    proc_.send(lift::Cmd::LoopSet, 0, 0);
    applyAll();
}

bool MiniUi::saveNow() {
    return proc_.saveSlot(projectSlot());   // saveState() asks flushState() for the blob
}

void MiniUi::applyAll() {
    for (int m = 0; m < 4; ++m)
        for (int p = 0; p < kPageCount[m]; ++p)
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < 4; ++k)
                    if (!(m == Tape && side == 1 && k == 3)) apply(m, p, side, k);   // the loop comes back with the tape
    for (int t = 0; t < 4; ++t) proc_.send(lift::Cmd::Mute, t, mute_[t] ? 1 : 0);
    proc_.send(lift::Cmd::Arm, track_);
}

String MiniUi::toJson() const {
    auto* o = new DynamicObject();
    o->setProperty("v", 1);
    o->setProperty("mode", int(mode_ == Radio ? Tape : mode_));
    o->setProperty("page", page_);
    o->setProperty("octave", octave_);
    o->setProperty("track", track_);
    o->setProperty("tempo", proc_.tempoBpm.load());
    o->setProperty("engine", proc_.synthEngine.load());
    Array<var> mutes, vals;
    for (bool m : mute_) mutes.add(m);
    for (auto& m : val_) for (auto& p : m) for (auto& sd : p) for (float k : sd) vals.add(k);
    o->setProperty("mutes", mutes);
    o->setProperty("vals", vals);
    return JSON::toString(var(o), true);
}

void MiniUi::fromJson(const String& text) {
    const var v = JSON::parse(text);
    if (!v.isObject()) return;
    mode_ = Mode(jlimit(0, 3, int(v.getProperty("mode", 0))));
    page_ = jlimit(0, kPageCount[mode_] - 1, int(v.getProperty("page", 0)));
    octave_ = jlimit(-3, 3, int(v.getProperty("octave", 0)));
    track_ = jlimit(0, 3, int(v.getProperty("track", 0)));
    proc_.tempoBpm.store(jlimit(kTempoMin, kTempoMax, double(v.getProperty("tempo", 100.0))));
    proc_.synthEngine.store(jlimit(0, int(lift::eng::kSynthEngines) - 1, int(v.getProperty("engine", 0))));
    if (auto* a = v.getProperty("mutes", var()).getArray())
        for (int t = 0; t < 4 && t < a->size(); ++t) mute_[t] = bool((*a)[t]);
    if (auto* a = v.getProperty("vals", var()).getArray()) {
        int i = 0;
        for (auto& m : val_) for (auto& p : m) for (auto& sd : p) for (float& k : sd)
            if (i < a->size()) k = jlimit(0.f, 1.f, float(double((*a)[i++])));
    }
}

void MiniUi::flushState() { proc_.setMiniState(toJson()); }

void MiniUi::stateLoaded(int) {
    const String blob = proc_.uiState().mini;
    if (blob.isEmpty()) { newProject(); return; }   // a full LIFT project: its tape, Mini defaults
    fromJson(blob);
    applyAll();
    repaint();
}


MiniUi::View MiniUi::view() const {
    View v{};
    v.mode = mode_;
    v.page = pg();
    v.pages = mode_ == Radio ? 1 : kPageCount[mode_];
    v.pageLabel = String(kModeName[mode_]) + (mode_ == Radio ? String() : "  " + String(pg() + 1) + "/" + String(kPageCount[mode_]));
    v.octave = octave_;
    v.track = track_;
    v.source = source_;
    v.sourceSel = sourceSel_;
    v.sourcesOpen = sourcesOpen_;
    v.shift = shift_.active();
    v.shiftLatched = shift_.latched();
    for (int i = 0; i < 4; ++i) {
        v.mute[i] = mute_[i];
        v.knob[i] = mode_ == Radio ? radioKnob_[i] : val_[mode_][pg()][0][i];
        v.knobS[i] = mode_ == Radio ? 0.f : val_[mode_][pg()][1][i];
    }
    v.toast = now() < flashUntil_ ? flash_ : status_;
    v.clip = clip_;
    return v;
}

void MiniUi::say(String s) { flash_ = std::move(s); flashUntil_ = now() + 1500; }

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
    if (b == Btn::Shift) {
        if (down) shift_.press(now());
        else if (shift_.release(now()) == ShiftState::Out::Cancel) {
            if (sourcesOpen_) sourcesOpen_ = false;  // back
            else flash_.clear();
        }
        return;
    }
    if (!down) return;
    const bool s = sh();
    if (b <= Btn::Mix) shift_.modeKey(); else shift_.used();
    switch (b) {
        case Btn::Synth: case Btn::Drum: case Btn::Tape: case Btn::Mix: {
            const auto m = Mode(int(b));
            if (m != mode_) releaseHeldNotes();   // the old mode's notes stop now
            page_ = (m == mode_) ? (page_ + 1) % kPageCount[int(m)] : 0;   // tap again: the next real page
            mode_ = m;
            sourcesOpen_ = false;
            break;
        }
        case Btn::OctDown: if (s) proc_.send(lift::Cmd::UndoDrop); else octave_ = jmax(-3, octave_ - 1); break;
        case Btn::OctUp: if (s) say("REDO: NOT IN THIS BUILD"); else octave_ = jmin(3, octave_ + 1); break;
        case Btn::PlayStop:
            if (s) proc_.send(lift::Cmd::Jump, 0, 0, 0.0);
            else proc_.send(lift::Cmd::Transport, proc_.uiPlaying.load() ? 0 : 1, 0);
            break;
        case Btn::Rec:
            if (s) { sourcesOpen_ = !sourcesOpen_; sourceSel_ = source_; break; }   // SHIFT + REC: sources
            proc_.send(lift::Cmd::Arm, track_);
            proc_.send(lift::Cmd::Transport, 1, proc_.overdubbing() ? 0 : 1);
            break;
        case Btn::Lift:
            clip_ = s ? proc_.keepTracks() : proc_.keepLast(proc_.liftBack.load());
            if (clip_) { proc_.addToPool(clip_); say("LIFT  " + String(clip_->frames() / 48000.0, 1) + " S"); }
            break;
        case Btn::Drop:
            if (!clip_) { say("NOTHING TO DROP: LIFT FIRST"); break; }
            if (mode_ == Tape || mode_ == Mix) { proc_.dropToLoop(clip_, track_); say("DROP  T" + String(track_ + 1)); }
            else if (mode_ == Synth) { proc_.setKeysClip(clip_); say("DROP  KEYS"); }
            else if (mode_ == Drum) { proc_.setSliceKit(clip_, 24); say("DROP  24 SLICES"); }
            else say("DROP: PICK SYNTH, DRUM OR TAPE");
            break;
        default: break;
    }
}

// SHIFT + keys, the keyboard as a menu (LIFT-Mini-Sheet section 6): white
// keys 1-8 are slots, black keys are tools. Tools not built yet say so.
void MiniUi::shiftKey(int k) {
    static constexpr int kWhite[12] = {1, 0, 2, 0, 3, 4, 0, 5, 0, 6, 0, 7};   // 1-based in the octave, 0 = black
    static constexpr int kBlack[12] = {0, 1, 0, 2, 0, 0, 3, 0, 4, 0, 5, 0};
    const int pc = k % 12, oct = k / 12;
    if (kWhite[pc] > 0) {
        const int w = oct * 7 + kWhite[pc];
        if (w <= 8) { proc_.saveSlotAsync(w, nullptr); say("SAVED SLOT " + String(w)); }
        return;
    }
    static const char* const kTools[10] = {
        "METRONOME", "COUNT-IN", "INPUT MONITOR", "SEQ TO TAPE SYNC", "SPLIT",
        "JACKS SETUP", "USB MODE", "BATTERY", "SPARE", "SPARE"};
    const int b = oct * 5 + kBlack[pc];   // 1..10
    say(String(kTools[b - 1]) + ": NOT IN THIS BUILD");
}

void MiniUi::releaseHeldNotes() {
    for (auto& n : sounding_)
        if (n >= 0) { proc_.send(lift::Cmd::NoteOff, n, 100); n = -1; }
}

void MiniUi::key(int k, bool down) {
    if (k < 0 || k >= 24) return;
    // key-up releases the note this key started, whatever the octave or mode
    // is now (a SYNTH note held into DRUM still stops on key-up)
    if (!down) {
        if (sounding_[k] >= 0) proc_.send(lift::Cmd::NoteOff, sounding_[k], 100);
        sounding_[k] = -1;
        return;
    }
    shift_.used();
    if (sh()) { shiftKey(k); return; }
    if (mode_ == Drum) {
        if (down) {
            if (proc_.sliceKitLoaded()) proc_.send(lift::Cmd::SliceHit, k, 110);
            else proc_.send(lift::Cmd::DrumHit, k % lift::eng::kDrumVoices, 110);
        }
        return;
    }
    if (sounding_[k] >= 0) proc_.send(lift::Cmd::NoteOff, sounding_[k], 100);
    sounding_[k] = 48 + octave_ * 12 + k;
    proc_.send(lift::Cmd::NoteOn, sounding_[k], 100);
}

void MiniUi::turn(int e, int detents) {
    const bool s = shift_.turn(e, now());
    if (sourcesOpen_) {
        if (e == 0) sourceSel_ = jlimit(0, kNumSources - 1, sourceSel_ + detents);   // every detent counts
        return;
    }
    if (mode_ == Radio) {
        auto* r = proc_.radio();
        const int n = r != nullptr ? int(r->list().stations.size()) : 0;
        if (e == 0 && n > 0) {          // TUNE: step through the station list
            const int st = ((jmax(0, r->station()) + detents) % n + n) % n;
            proc_.setRadioStation(st);
        } else if (e == 1 && r != nullptr) {  // PRESET 1-8
            radioKnob_[1] = jlimit(0.f, 7.f, radioKnob_[1] + float(detents));
            const int st = r->list().presets[size_t(radioKnob_[1])];
            if (st >= 0) proc_.setRadioStation(st);
        } else if (e >= 2 && r != nullptr) {
            radioKnob_[e] = jlimit(0.f, 1.f, radioKnob_[e] + 0.02f * float(detents));
            if (e == 2) r->level.store(radioKnob_[2] * 2.f);
            else r->mono.store(radioKnob_[3]);
        }
        return;
    }
    // SHIFT + turn = the knob's second parameter (sheet 4); SHIFT released
    // mid-turn keeps that encoder shifted briefly (ShiftState grace)
    const int side = s ? 1 : 0;
    if (pageDef(mode_, pg()).s[e][0] == 0 && side == 1) return;   // no SHIFT parameter here
    const double t = now();
    const double dt = t - lastTurn_[e];
    lastTurn_[e] = t;
    float& v = val(side, e);
    if (mode_ == Tape && side == 1 && (e == 1 || e == 3)) {   // CHARACTER, LOOP: one item per detent
        const int n = e == 1 ? 4 : 6;
        const int idx = jlimit(0, n - 1, roundToInt(v * float(n - 1)) + detents);
        v = float(idx) / float(n - 1);
    } else {
        v = jlimit(0.f, 1.f, v + kSlowStep * encoderAccel(dt, detents) * float(detents));
    }
    apply(mode_, pg(), side, e);
}

// One knob value to wherever it lives. Parameters the engine has no hook for
// yet are kept with the project (and the sound) and shown on screen.
void MiniUi::apply(int mode, int page, int side, int k) {
    const float v = val_[mode][page][side][k];
    switch (mode) {
        case Synth:
        case Drum:
            if (page == 0 && side == 0) proc_.setKnobValue(mode == Synth ? 1 : 2, k, v);
            else if (page == 1 && side == 0) proc_.fxKnobs[k].store(v);
            break;
        case Tape:
            if (side == 0) {
                if (k == 0) proc_.signedSpeed.store(bipolarKnobToSpeed(v));
                else proc_.setKnobValue(0, k, v);
            } else if (k == 1) {
                proc_.send(lift::Cmd::Character, roundToInt(v * 3.f));
            } else if (k == 3) {
                setLoopBars(kLoopBars[roundToInt(v * 5.f)]);
            }
            break;
        case Mix:
            if (page == 0) proc_.send(lift::Cmd::TrackMix, k, side, double(v));
            else if (page == 2 && side == 0 && k == 0) proc_.tempoBpm.store(std::round(knobToTempo(v)));
            break;
        default: break;
    }
}

// LOOP LENGTH: off, or N bars starting at the bar under the playhead
void MiniUi::setLoopBars(int bars) {
    if (bars <= 0) { proc_.send(lift::Cmd::LoopSet, 0, 0); return; }
    const double bar = 4.0 * 60.0 / proc_.tempoBpm.load() * 48000.0;
    const int start = int(std::floor(proc_.uiPos.load() / bar) * bar);
    proc_.send(lift::Cmd::LoopSet, start, start + int(std::lround(bar * bars)));
}

String MiniUi::valueText(int side, int k) const {
    const float v = val_[mode_][pg()][side][k];
    if (mode_ == Tape && side == 1 && k == 1) return kCharacterNames[roundToInt(v * 3.f)];
    if (mode_ == Tape && side == 1 && k == 3) {
        const int b = kLoopBars[roundToInt(v * 5.f)];
        return b == 0 ? String("OFF") : String(b) + (b == 1 ? " BAR" : " BARS");
    }
    if (mode_ == Mix && pg() == 2 && side == 0 && k == 0) return String(roundToInt(knobToTempo(v)));
    return String(roundToInt(v * 100.f));
}

void MiniUi::push(int e, bool down) {
    if (!down) return;
    shift_.used();
    if (sourcesOpen_) {
        if (e == 0) {
            source_ = sourceSel_;
            sourcesOpen_ = false;
            proc_.setComputerAudio(source_ == 2);
            if (source_ == 1) {
                releaseHeldNotes();
                mode_ = Radio;
                auto* r = proc_.radio();
                proc_.setRadioStation(r != nullptr && r->station() >= 0 ? r->station() : 0);
            } else {
                proc_.setRadioStation(-1);
                proc_.send(lift::Cmd::RecSource, source_ == 3 ? 2 : 0);
                if (source_ == 2) {
                    auto* c = proc_.computerAudio();
                    say(c == nullptr ? String("COMPUTER AUDIO: NOT AVAILABLE") : c->supported() ? String("COMPUTER AUDIO ON: LIFT KEEPS IT")
                                                                                       : c->status());
                }
            }
        }
        return;
    }
    if (mode_ == Tape) { track_ = e; proc_.send(lift::Cmd::Arm, e); }
    else if (mode_ == Mix) { mute_[e] = !mute_[e]; proc_.send(lift::Cmd::Mute, e, mute_[e] ? 1 : 0); }
    else if (sh()) {
        val(0, e) = 0.5f;
        apply(mode_, pg(), 0, e);
    }
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
        const int stride = jmax(1, n / 480);
        for (int i = 0; i < 480; ++i) scope_[i] = scopeBuf_[size_t(jmin(n - 1, i * stride))];
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

    // status bar: four equal fields
    auto st = L.status;
    const float q = st.getWidth() / 4.f;
    g.setFont(label(1.1f));
    g.setColour(col::ink);
    g.drawText(String(kModeName[mode_]) + (mode_ == Radio ? String() : "  " + String(pg() + 1) + "/" + String(kPageCount[mode_])), st.removeFromLeft(q), Justification::centredLeft);
    g.setColour(proc_.clockSlaved.load() ? col::ink : col::dim);
    g.drawText(String(proc_.tempoBpm.load(), 1) + " BPM", st.removeFromLeft(q), Justification::centred);
    g.setColour(col::dim);
    g.drawText("OCT " + String(octave_ >= 0 ? "+" : "") + String(octave_) + (shift_.latched() ? "  SHIFT LOCK" : sh() ? "  SHIFT" : ""),
               st.removeFromLeft(q), Justification::centred);
    if (proc_.uiRecording.load()) {
        g.setColour(col::rec);
        g.fillEllipse(Rectangle<float>(L.font * 0.8f, L.font * 0.8f).withCentre({st.getX() + L.font, st.getCentreY()}));
    }
    g.setColour(proc_.uiPlaying.load() ? col::ink : col::dim);
    g.drawText(proc_.uiPlaying.load() ? "PLAY" : "STOP", st, Justification::centredRight);

    const auto body = L.body;
    if (sourcesOpen_) paintSources(g, body);
    else switch (mode_) {
        case Tape: paintTape(g, body); break;
        case Synth: paintSynth(g, body); break;
        case Drum: paintDrum(g, body); break;
        case Mix: paintMix(g, body); break;
        case Radio: paintRadio(g, body); break;
    }
    paintFoot(g, L.foot);

    const String msg = now() < flashUntil_ ? flash_ : status_;
    if (msg.isNotEmpty()) {
        g.setColour(col::bg.withAlpha(0.85f));
        g.fillRect(L.toast);
        g.setColour(col::ink);
        g.setFont(label());
        g.drawText(msg, L.toast.reduced(L.pad + 4.f, 0.f), Justification::centredLeft);
    }
}

void MiniUi::paintTape(Graphics& g, Rectangle<float> r) {
    const int frames = jmax(1, proc_.uiFrames.load());
    const float head = float(proc_.uiPos.load() / frames);
    const float ls = proc_.uiLoopStart.load() / float(frames), le = proc_.uiLoopEnd.load() / float(frames);
    const float lab = L.font * 3.f;
    auto lane = r.withTrimmedLeft(lab);
    if (le > ls) { g.setColour(col::ink.withAlpha(0.08f)); g.fillRect(lane.getX() + ls * lane.getWidth(), r.getY(), (le - ls) * lane.getWidth(), r.getHeight()); }
    const float h = r.getHeight() / 4.f;
    for (int t = 0; t < 4; ++t) {
        auto row = Rectangle<float>(r.getX(), r.getY() + t * h, r.getWidth(), h).reduced(0.f, L.pad * 0.8f);
        g.setColour(t == track_ ? col::knob[t] : col::dim);
        g.drawRect(row, t == track_ ? 2.f * L.stroke : L.stroke);
        g.setFont(label());
        g.drawText("T" + String(t + 1) + (mute_[t] ? " M" : ""), row.removeFromLeft(lab), Justification::centred);
        const float lvl = jlimit(0.f, 1.f, proc_.uiTrackLevel[t].load());
        g.setColour(col::knob[t].withAlpha(0.7f));
        g.fillRect(row.getX(), row.getCentreY() - lvl * row.getHeight() * 0.45f, L.pad, lvl * row.getHeight() * 0.9f);
    }
    g.setColour(proc_.uiRecording.load() ? col::rec : col::ink);
    const float x = lane.getX() + head * lane.getWidth();
    g.drawLine(x, r.getY(), x, r.getBottom(), 2.f * L.stroke);
    // signed speed readout under the head
    const float sp = proc_.signedSpeed.load();
    g.setFont(lift::ui::mono(true, L.font));
    g.drawText(sp == 0.f ? String("STOP") : (sp < 0.f ? "<< " : ">> ") + String(std::abs(sp), 2) + "x",
               Rectangle<float>(x + 4.f, r.getY(), L.font * 7.f, L.font * 1.4f), Justification::centredLeft);
}

void MiniUi::paintSynth(Graphics& g, Rectangle<float> r) {
    g.setColour(col::dim);
    g.drawHorizontalLine(int(r.getCentreY()), r.getX(), r.getRight());
    Path p;
    for (int i = 0; i < 480; ++i) {
        const float x = r.getX() + r.getWidth() * float(i) / 479.f;
        const float y = r.getCentreY() - jlimit(-1.f, 1.f, scope_[i] * 1.5f) * r.getHeight() * 0.45f;
        if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
    }
    g.setColour(col::knob[0]);
    g.strokePath(p, PathStrokeType(1.5f * L.stroke));
    const float d = L.font * 0.75f;
    for (int v = 0; v < lift::eng::kSynthVoices; ++v) {
        g.setColour(col::knob[0].withAlpha(0.2f + 0.8f * jlimit(0.f, 1.f, proc_.uiVoiceEnv[v].load())));
        g.fillEllipse(r.getRight() - d * 1.5f * float(lift::eng::kSynthVoices - v), r.getY(), d, d);
    }
    if (clip_) { g.setColour(col::dim); g.setFont(lift::ui::jost(400, L.font)); g.drawText("CLIP READY: DROP plays it on the keys", r, Justification::bottomLeft); }
}

void MiniUi::paintDrum(Graphics& g, Rectangle<float> r) {
    const float w = r.getWidth() / 12.f, h = r.getHeight() / 2.f;
    for (int k = 0; k < 24; ++k) {
        auto cell = Rectangle<float>(r.getX() + (k % 12) * w, r.getY() + (k < 12 ? h : 0.f), w, h).reduced(L.pad * 0.5f);
        const float env = k < lift::eng::kDrumVoices ? jlimit(0.f, 1.f, proc_.uiDrumEnv[k].load()) : 0.f;
        g.setColour(col::knob[3].withAlpha(0.15f + 0.85f * env));
        g.fillRect(cell);
        g.setColour(col::dim);
        g.drawRect(cell, L.stroke);
    }
    const int step = proc_.uiDrumStep.load();
    if (step >= 0) { g.setColour(col::ink); g.fillRect(r.getX() + (step % 16) * r.getWidth() / 16.f, r.getBottom() - L.pad * 0.7f, r.getWidth() / 16.f, L.pad * 0.7f); }
}

void MiniUi::paintMix(Graphics& g, Rectangle<float> r) {
    const float w = r.getWidth() / 5.f;
    for (int t = 0; t < 5; ++t) {
        auto strip = Rectangle<float>(r.getX() + t * w, r.getY(), w, r.getHeight()).reduced(L.pad * 2.f, L.pad * 0.7f);
        const float lvl = jlimit(0.f, 1.f, t < 4 ? proc_.uiTrackLevel[t].load() : proc_.uiMasterLevel.load());
        g.setColour(col::dim); g.drawRect(strip, L.stroke);
        g.setColour(t < 4 ? (mute_[t] ? col::dim : col::knob[t]) : col::ink);
        g.fillRect(strip.withTop(strip.getBottom() - lvl * strip.getHeight()));
        g.setColour(col::ink); g.setFont(label());
        g.drawText(t < 4 ? "T" + String(t + 1) : String("MAIN"), strip.withHeight(L.font * 1.4f), Justification::centred);
    }
}

void MiniUi::paintRadio(Graphics& g, Rectangle<float> r) {
    auto* rad = proc_.radio();
    g.setFont(label(1.6f));
    g.setColour(col::ink);
    const int st = rad != nullptr ? rad->station() : -1;
    const String name = st >= 0 ? rad->list().stations[size_t(st)].name.toUpperCase() : String("RADIO OFF");
    g.drawText(name, r.removeFromTop(L.font * 2.4f), Justification::centredLeft);
    g.setFont(label());
    g.setColour(col::dim);
    g.drawText("INTERNET  " + (rad != nullptr ? rad->status() : String("OFF")), r.removeFromTop(L.font * 1.8f), Justification::centredLeft);
    // dial: one tick per station, the needle on the tuned one
    const int n = rad != nullptr ? int(rad->list().stations.size()) : 0;
    auto dial = r.removeFromTop(r.getHeight() * 0.5f).reduced(0.f, L.pad);
    g.setColour(col::dim);
    g.drawHorizontalLine(int(dial.getCentreY()), dial.getX(), dial.getRight());
    for (int i = 0; i < n; ++i) {
        const float x = dial.getX() + dial.getWidth() * (float(i) + 0.5f) / float(n);
        g.drawVerticalLine(int(x), dial.getCentreY() - L.pad, dial.getCentreY() + L.pad);
    }
    if (st >= 0 && n > 0) {
        const float x = dial.getX() + dial.getWidth() * (float(st) + 0.5f) / float(n);
        g.setColour(col::knob[0]);
        g.drawLine(x, dial.getY(), x, dial.getBottom(), 2.f * L.stroke);
    }
    // the 8 presets
    const float w = r.getWidth() / 8.f;
    for (int p = 0; p < 8; ++p) {
        auto cell = Rectangle<float>(r.getX() + p * w, r.getY(), w, r.getHeight() * 0.6f).reduced(L.pad * 0.5f);
        const int ps = rad != nullptr ? rad->list().presets[size_t(p)] : -1;
        g.setColour(ps >= 0 && ps == st ? col::knob[1] : col::dim);
        g.drawRect(cell, L.stroke);
        g.drawText(String(p + 1), cell, Justification::centred);
    }
}

void MiniUi::paintSources(Graphics& g, Rectangle<float> r) {
    g.setFont(label(1.3f));
    const float h = r.getHeight() / float(kNumSources + 1);
    g.setColour(col::dim);
    g.drawText("SOURCE  (YELLOW: PICK, PRESS: OPEN, SHIFT: BACK)", r.removeFromTop(h), Justification::centredLeft);
    for (int i = 0; i < kNumSources; ++i) {
        auto row = r.removeFromTop(h).reduced(0.f, L.pad * 0.5f);
        if (i == sourceSel_) { g.setColour(col::knob[0]); g.fillRect(row.removeFromLeft(L.pad)); }
        g.setColour(i == sourceSel_ ? col::ink : col::dim);
        g.drawText(String(kSources[i]) + (i == source_ ? "  *" : ""), row.withTrimmedLeft(L.pad * 2.f), Justification::centredLeft);
    }
}

void MiniUi::paintFoot(Graphics& g, Rectangle<float> r) {
    const float w = r.getWidth() / 4.f;
    const int side = mode_ != Radio && sh() ? 1 : 0;   // SHIFT shows the second parameters
    const auto& pd = pageDef(mode_ == Radio ? 0 : mode_, pg());
    for (int k = 0; k < 4; ++k) {
        auto cell = Rectangle<float>(r.getX() + k * w, r.getY(), w, r.getHeight()).reduced(L.pad, L.pad);
        float v = mode_ == Radio ? (k == 1 ? radioKnob_[1] / 7.f : k == 0 ? 0.f : radioKnob_[k]) : val_[mode_][pg()][side][k];
        String txt = mode_ == Radio ? String(roundToInt(v * 100.f)) : valueText(side, k);
        if (mode_ == Tape && k == 0 && side == 0) {
            const float sp = proc_.signedSpeed.load();
            txt = sp == 0.f ? String("STOP") : String(sp, 2) + "x";
        } else if (mode_ == Radio && k == 1) {
            txt = String(int(radioKnob_[1]) + 1);
        } else if (mode_ == Radio && k == 0) {
            auto* rad = proc_.radio();
            txt = rad != nullptr && rad->station() >= 0 ? String(rad->station() + 1) : String("-");
        }
        g.setColour(col::knob[k]);
        g.fillRect(cell.removeFromBottom(L.pad).withWidth(cell.getWidth() * v));
        g.setFont(label());
        g.drawText(mode_ == Radio ? kRadioNames[k] : side ? pd.s[k] : pd.a[k], cell.removeFromTop(L.font * 1.5f), Justification::centredLeft);
        g.setColour(col::ink);
        g.setFont(lift::ui::mono(true, L.big));
        g.drawText(txt, cell, Justification::centredLeft);
    }
}

}  // namespace mini
