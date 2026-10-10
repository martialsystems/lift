// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Save slots, plug-in state, reopen-last, sample-rate pitch and MIDI checks
// for the JUCE app. Called from panel_check.cpp.

#include "LiftPanel.h"
#include "LiftProcessor.h"
#include "SlotStore.h"
#include "UiState.h"
#include "tape/transport.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

using Check = std::function<void(bool, const juce::String&)>;

// Runs `seconds` of audio at the processor's rate, feeding the MIDI events
// (absolute sample time, message) that fall in each block. Returns the left output.
struct Feed {
    lift::LiftProcessor& p;
    double sr;
    juce::int64 now = 0;
    std::vector<std::pair<juce::int64, juce::MidiMessage>> events;
    std::vector<juce::MidiMessage> out;
    std::vector<float> left;
    void run(double seconds, bool keep = false) {
        const juce::int64 end = now + static_cast<juce::int64>(seconds * sr);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        while (now < end) {
            buf.clear();
            midi.clear();
            for (const auto& e : events) {
                if (e.first >= now && e.first < now + 512) {
                    midi.addEvent(e.second, static_cast<int>(e.first - now));
                }
            }
            p.processBlock(buf, midi);
            for (const auto meta : midi) {
                out.push_back(meta.getMessage());
            }
            if (keep) {
                left.insert(left.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
            }
            now += 512;
        }
    }
    void at(double secondsFromNow, const juce::MidiMessage& m) {
        events.emplace_back(now + static_cast<juce::int64>(secondsFromNow * sr), m);
    }
};

// Frequency from rising zero crossings (linear interpolation between samples).
double zeroCrossHz(const std::vector<float>& x, size_t from, double sr) {
    double first = -1.0, last = -1.0;
    int n = 0;
    for (size_t i = from + 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.f && x[i] >= 0.f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0.0) {
                first = t;
            } else {
                ++n;
            }
            last = t;
        }
    }
    return n > 0 ? n * sr / (last - first) : 0.0;
}

double goertzel(const std::vector<float>& x, size_t from, double sr, double hz) {
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < x.size(); ++i) {
        const double ph = 2.0 * juce::MathConstants<double>::pi * hz * static_cast<double>(i) / sr;
        re += x[i] * std::cos(ph);
        im += x[i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size() - from);
}

void fillTape(TapeRuntime& rt) {
    for (int t = 0; t < kTrackCount; ++t) {
        for (int c = 0; c < 2; ++c) {
            float* x = rt.ch[t][c];
            const int len = 48000 * (t + 1) + c * 100;  // different lengths: trimming must keep them exact
            for (int i = 0; i < len; ++i) {
                x[i] = 0.3f * std::sin(0.001f * static_cast<float>(i) * static_cast<float>(t + 1 + c));
            }
        }
    }
    for (int i = 0; i < 1000; ++i) {
        rt.clip[0][i] = 0.1f;
        rt.clip[1][i] = -0.1f;
    }
    rt.clipFrames = 1000;
    rt.pos = 12345.0;
}

bool sameTape(TapeRuntime& a, TapeRuntime& b) {
    for (int t = 0; t < kTrackCount; ++t) {
        for (int c = 0; c < 2; ++c) {
            if (std::memcmp(a.ch[t][c], b.ch[t][c], sizeof(float) * static_cast<size_t>(a.frames)) != 0) {
                return false;
            }
        }
    }
    return a.clipFrames == b.clipFrames &&
           std::memcmp(a.clip[0], b.clip[0], sizeof(float) * static_cast<size_t>(a.clipFrames)) == 0 &&
           std::memcmp(a.clip[1], b.clip[1], sizeof(float) * static_cast<size_t>(a.clipFrames)) == 0;
}

// A state with every field away from its default.
lift::UiState oddState() {
    lift::UiState s;
    s.mode = 1;
    s.bay = true;
    s.fx = false;
    s.sel = {{3, 5, 1, 2, 1}};
    for (int m = 0; m < 5; ++m) {
        for (int i = 0; i < 4; ++i) {
            s.enc[static_cast<size_t>(m)][static_cast<size_t>(i)] = 0.05f + 0.045f * static_cast<float>(m * 4 + i);
        }
    }
    s.loop = true;
    s.loopIn = 24000;
    s.loopOut = 120000;
    s.marks = {{1000, 2000, 3000, 4000}};
    s.nextMark = 2;
    s.arm = 2;
    s.mutes = {{true, false, false, true}};
    s.oct = -2;
    s.transpose = 1;
    s.seqDiv = 12;
    s.drumDiv = 32;
    s.drumLen = 24;
    s.swing = 45;
    s.recSource = 2;
    s.character = 3;
    s.color = 4;
    s.stack = true;
    // cable colours follow the source's type (A PITCH yellow = 2, DRUM red = 1)
    s.cords = {{3, 4, 2, true}, {7, 1, 1, false}, {3, 9, 2, true}};
    s.pins[0 * 16 + 4] = 1;
    s.pins[11 * 16 + 11] = 3;  // PULSE -> REC, inverted
    s.recJackLifts = true;
    s.learn[50] = 9;
    s.learn[51] = 17;
    return s;
}

// Hand-made slot bytes at another sample rate (what an older or foreign save looks like).
juce::MemoryBlock blobAt(int rate, double hz, double seconds) {
    juce::MemoryOutputStream out;
    out.write("LIFTSLOT", 8);
    out.writeInt(1);
    const juce::String xml = lift::UiState().toTree().toXmlString();
    out.writeInt(static_cast<int>(xml.getNumBytesAsUTF8()));
    out.write(xml.toRawUTF8(), xml.getNumBytesAsUTF8());
    const int n = static_cast<int>(rate * seconds);
    out.writeInt(rate);
    out.writeInt(n);
    out.writeDouble(static_cast<double>(rate));  // playhead at 1 s
    out.writeInt(4);
    for (int t = 0; t < 4; ++t) {
        for (int c = 0; c < 2; ++c) {
            out.writeInt(t == 0 ? n : 0);
            for (int i = 0; t == 0 && i < n; ++i) {
                out.writeFloat(0.4f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / rate)));
            }
        }
    }
    out.writeInt(0);
    return out.getMemoryBlock();
}

}  // namespace

void runStateChecks(const Check& check) {
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("lift-slots-check-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();

    std::printf("-- save slots\n");
    {
        lift::SlotStore st(folder);
        check(!lift::SlotStore::valid(0) && lift::SlotStore::valid(1) && lift::SlotStore::valid(999) &&
                  !lift::SlotStore::valid(1000) && !lift::SlotStore::valid(-5),
              "slots are 001..999 (0 and 1000 rejected)");
        juce::MemoryBlock b("x", 1);
        check(!st.write(0, b) && !st.write(1000, b) && st.slotFile(1000) == juce::File(),
              "writing slot 0 or 1000 fails without touching the disk");
        check(st.write(999, b) && st.exists(999) && st.slotFile(999).getFileName() == "slot-999.lift", "slot 999 writes");
        check(st.write(1, b) && st.slotFile(1).getFileName() == "slot-001.lift", "slot 001 writes");
        st.slotFile(999).deleteFile();
        st.slotFile(1).deleteFile();
    }
    const lift::UiState odd = oddState();
    {
        lift::LiftProcessor a;
        a.setSlotFolder(folder);
        check(!a.reopenLast() && a.currentSlot() == 0, "nothing to reopen in an empty folder");
        a.prepareToPlay(48000.0, 512);
        fillTape(a.runtime());
        a.setUiState(odd);
        check(!a.saveSlot(0) && !a.saveSlot(1000), "saveSlot rejects 0 and 1000");
        check(a.loadSlot(1000) == lift::LiftProcessor::LoadResult::BadSlot &&
                  a.loadSlot(500) == lift::LiftProcessor::LoadResult::Empty,
              "loadSlot: 1000 is no slot, an unused slot is empty");
        check(a.saveSlot(7) && a.currentSlot() == 7, "save to slot 007");
        juce::Array<juce::File> files;
        folder.findChildFiles(files, juce::File::findFiles, false, "*");
        juce::StringArray names;
        for (const auto& f : files) {
            names.add(f.getFileName());
        }
        names.sort(false);
        check(names.joinIntoString(",") == "last-slot.txt,slot-007.lift",
              "atomic write leaves only the slot and last-slot.txt (" + names.joinIntoString(",") + ")");

        lift::LiftProcessor b;
        b.setSlotFolder(folder);
        b.prepareToPlay(48000.0, 512);
        {
            lift::LiftPanel pb(b);
            check(b.loadSlot(7) == lift::LiftProcessor::LoadResult::Ok, "load slot 007 into a fresh instance");
            TapeRuntime& rb = b.runtime();
            if (!(b.uiState() == odd)) {
                std::printf("GOT %s\nWANT %s\n", b.uiState().toTree().toXmlString().toRawUTF8(), odd.toTree().toXmlString().toRawUTF8());
            }
            check(b.uiState() == odd, "every panel setting round-trips (knobs on all screens, pads, arm/mute, shift "
                                      "settings, patch cables/colours/stackables, engine/kit, MIDI learn)");
            check(pb.captureUi() == odd, "the open panel shows the loaded state");
            check(sameTape(a.runtime(), rb), "tape audio of all four tracks and the LIFT clip round-trip sample-exact");
            check(rb.pos == 12345.0 && rb.loopStart == 24000 && rb.loopEnd == 120000 && rb.arm == 2 && rb.mute[0] &&
                      rb.mute[3] && !rb.mute[2] && rb.character == 3 && !rb.playing,
                  "engine gets playhead, loop points, arm, mutes and character; transport stopped");
            lift::PatchSnapshot snap;
            b.patch.read(snap);
            check(snap.count == 3 && snap.cords[2].st && snap.cords[0].c == 2 && snap.pins[11 * 16 + 11] == 3, "patch model republished (cables and pins)");
            check(std::abs(b.speed.load() - 0.25f * std::pow(16.f, odd.enc[2][0])) < 1e-5f && b.drumKit.load() == 5 &&
                      b.synthEngine.load() == 3 && b.drumSwing.load() == 45,
                  "knob and shift-layer models reach the engine atomics");
        }
        // corrupt copies are refused and leave the state alone
        juce::MemoryBlock blob;
        a.slots().read(7, blob);
        juce::MemoryBlock newer(blob);
        static_cast<char*>(newer.getData())[8] = 99;
        check(b.loadState(newer.getData(), newer.getSize(), 0) == lift::LiftProcessor::LoadResult::NewerVersion,
              "a save from a newer format version is refused");
        check(b.loadState(blob.getData(), blob.getSize() / 2, 0) == lift::LiftProcessor::LoadResult::BadData &&
                  b.uiState() == odd,
              "a truncated save is refused and the state stays");

        // plug-in state uses the same bytes
        juce::MemoryBlock ps;
        a.getStateInformation(ps);
        lift::LiftProcessor c;
        c.prepareToPlay(48000.0, 512);
        c.setStateInformation(ps.getData(), static_cast<int>(ps.getSize()));
        check(ps.getSize() > 1000 && c.uiState() == odd && sameTape(a.runtime(), c.runtime()),
              "getStateInformation / setStateInformation round-trip everything");
    }
    std::printf("-- reopen last\n");
    {
        lift::LiftProcessor d;
        d.setSlotFolder(folder);
        check(d.reopenLast() && d.currentSlot() == 7 && d.uiState() == odd, "a new instance reopens slot 007 (last saved)");
        d.prepareToPlay(48000.0, 512);
        lift::UiState s2;
        s2.mode = 4;
        d.setUiState(s2);
        check(d.saveSlot(12), "save slot 012");
        lift::LiftProcessor e;
        e.setSlotFolder(folder);
        check(e.reopenLast() && e.currentSlot() == 12 && e.uiState().mode == 4, "then the next launch reopens 012");
        check(e.loadSlot(7) == lift::LiftProcessor::LoadResult::Ok && lift::SlotStore(folder).lastSlot() == 7,
              "loading a slot makes it the one reopened next");
    }

    std::printf("-- sample rate\n");
    for (double sr : {44100.0, 96000.0, 48000.0}) {
        lift::LiftProcessor p;
        p.prepareToPlay(sr, 512);
        p.clearDrumPatterns();
        TapeRuntime& rt = p.runtime();
        for (int i = 0; i < rt.frames; ++i) {  // 1 kHz recorded at the 48 kHz tape rate
            rt.ch[0][0][i] = rt.ch[0][1][i] = 0.4f * std::sin(2.f * juce::MathConstants<float>::pi * 1000.f * i / 48000.f);
        }
        p.send(lift::Cmd::Transport, 1, 0);
        Feed f{p, sr};
        f.run(2.0, true);
        const double hz = zeroCrossHz(f.left, static_cast<size_t>(0.5 * sr), sr);
        const double tapeSecs = rt.pos / 48000.0;
        std::printf("  %.1f kHz: tape tone %.2f Hz, %.3f s of tape in 2 s\n", sr / 1000.0, hz, tapeSecs);
        check(std::abs(hz - 1000.0) < 5.0, juce::String(sr / 1000.0, 1) + " kHz: tape plays at pitch (1 kHz stays 1 kHz)");
        check(std::abs(tapeSecs - 2.0) < 0.02, juce::String(sr / 1000.0, 1) + " kHz: tape runs at real time");
        // the placeholder voice: A4 must come out at 440 Hz, not 440 * 48000 / sr
        lift::LiftProcessor q;
        q.prepareToPlay(sr, 512);
        Feed g{q, sr};
        g.at(0.0, juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)));
        g.run(1.0, true);
        const double right = goertzel(g.left, static_cast<size_t>(0.2 * sr), sr, 440.0);
        const double wrong = sr == 48000.0 ? 0.0 : goertzel(g.left, static_cast<size_t>(0.2 * sr), sr, 440.0 * 48000.0 / sr);
        check(right > 0.02 && right > 4.0 * wrong, juce::String(sr / 1000.0, 1) + " kHz: synth A4 sounds at 440 Hz");
    }
    {
        // a 96 kHz save loads into the 48 kHz tape resampled (pitch and length kept)
        lift::LiftProcessor p;
        p.prepareToPlay(44100.0, 512);
        const auto blob = blobAt(96000, 1000.0, 3.0);
        check(p.loadState(blob.getData(), blob.getSize(), 0) == lift::LiftProcessor::LoadResult::Ok, "load a 96 kHz save");
        TapeRuntime& rt = p.runtime();
        std::vector<float> t1(rt.ch[0][0], rt.ch[0][0] + 48000 * 3);
        const double hz = zeroCrossHz(t1, 1000, 48000.0);
        int len = rt.frames;
        while (len > 0 && rt.ch[0][0][len - 1] == 0.f) {
            --len;
        }
        std::printf("  96 kHz save on tape: %.2f Hz, %d frames, playhead %.0f\n", hz, len, rt.pos);
        check(std::abs(hz - 1000.0) < 1.0 && std::abs(len - 144000) < 8 && std::abs(rt.pos - 48000.0) < 1.0,
              "96 kHz tape audio is resampled to the 48 kHz tape on load");
    }

    std::printf("-- MIDI\n");
    {
        lift::LiftProcessor p;
        p.setSlotFolder(folder);
        p.prepareToPlay(48000.0, 512);
        Feed f{p, 48000.0};
        f.at(0.0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)));
        f.run(0.3, true);
        const auto& v = p.instrument().synth;
        check(v.noteGate(60) && std::abs(v.noteVel(60) - 1.f) < 1e-3f, "note on: pitch and velocity 127");
        double loud = 0.0;
        for (size_t i = 4800; i < f.left.size(); ++i) {
            loud = juce::jmax(loud, static_cast<double>(std::abs(f.left[i])));
        }
        f.left.clear();
        f.at(0.0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(20)));
        f.run(0.3, true);
        double soft = 0.0;
        for (size_t i = 4800; i < f.left.size(); ++i) {
            soft = juce::jmax(soft, static_cast<double>(std::abs(f.left[i])));
        }
        check(std::abs(v.noteVel(60) - 20.f / 127.f) < 1e-3f && soft < 0.6 * loud, "velocity 20 plays softer than 127");
        f.at(0.0, juce::MidiMessage::pitchWheel(1, 16383));
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 1, 127));
        f.run(0.02);
        check(std::abs(v.bend() - 2.f) < 0.01f && std::abs(v.mod() - 1.f) < 1e-3f, "pitch bend (+2 st) and mod wheel");
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 64, 127));
        f.at(0.001, juce::MidiMessage::noteOff(1, 60));
        f.run(0.05);
        const bool held = v.noteGate(60);
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 64, 0));
        f.run(0.02);
        check(held && !v.noteGate(60), "sustain pedal holds the note until it lifts");
        f.at(0.0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90)));
        f.at(0.001, juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(90)));
        f.at(0.002, juce::MidiMessage::noteOff(1, 64));
        f.run(0.02);
        check(v.noteGate(60) && !v.noteGate(64), "poly: releasing the top note keeps the held one sounding");
        f.at(0.0, juce::MidiMessage::noteOff(1, 60));
        f.run(0.02);
        check(!v.anyGate(), "note off");

        // clock in: 24 ppqn at 132 BPM, then start / stop / continue
        const double per = 60.0 / (132.0 * 24.0);
        for (int k = 0; k < 96; ++k) {
            f.at(k * per, juce::MidiMessage::midiClock());
        }
        f.at(0.2, juce::MidiMessage::midiStart());
        const int serial0 = p.transportSerial.load();
        f.run(96 * per + 0.01);
        std::printf("  clock in: %.2f BPM\n", p.tempoBpm.load());
        check(std::abs(p.tempoBpm.load() - 132.0) < 0.5 && p.clockSlaved.load(), "MIDI clock sets the tempo (132 BPM)");
        TapeRuntime& rt = p.runtime();
        check(rt.playing && p.transportSerial.load() == serial0 + 1 && p.midiPlay.load(), "MIDI start runs the tape");
        const double startPos = rt.pos;
        check(startPos > 0.0 && startPos < 48000.0 * 2.0, "from the loop start");
        f.at(0.0, juce::MidiMessage::midiStop());
        f.run(1.0);
        const double stopPos = rt.pos;
        check(!rt.playing && !p.midiPlay.load(), "MIDI stop stops it");
        check(!p.clockSlaved.load(), "no clock for 0.5 s: back to the internal tempo");
        f.at(0.0, juce::MidiMessage::midiContinue());
        f.run(0.5);
        check(rt.playing && rt.pos > stopPos, "MIDI continue carries on from where it stopped");
        f.at(0.0, juce::MidiMessage::midiStop());
        f.run(1.0);

        // clock out while the tape runs on the internal clock
        f.out.clear();
        p.send(lift::Cmd::Transport, 1, 0);
        p.send(lift::Cmd::NoteOn, 62, 99);
        f.run(1.0);
        int clocks = 0, starts = 0, notes = 0;
        for (const auto& m : f.out) {
            clocks += m.isMidiClock() ? 1 : 0;
            starts += m.isMidiStart() ? 1 : 0;
            notes += (m.isNoteOn() && m.getNoteNumber() == 62 && m.getVelocity() == 99) ? 1 : 0;
        }
        std::printf("  clock out: %d clocks, %d starts in 1 s at %.1f BPM\n", clocks, starts, p.tempoBpm.load());
        check(std::abs(clocks - 24.0 * p.tempoBpm.load() / 60.0) <= 1.5 && starts == 1, "MIDI clock and start go out");
        check(notes == 1, "panel keyboard notes go out as MIDI");
        p.send(lift::Cmd::Stop);
        p.send(lift::Cmd::NoteOff, 62);
        f.run(0.5);

        // CC map and learn (no editor: the processor's state takes them)
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 24, 127));  // TAPE SPEED
        f.run(0.02);
        p.drainMidiEvents();
        check(std::abs(p.speed.load() - 4.f) < 1e-3f && std::abs(p.uiState().enc[2][0] - 1.f) < 1e-6f,
              "CC 24 turns TAPE SPEED");
        p.learnTarget.store(2 * 4 + 1);  // TAPE BIAS
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 70, 64));
        f.run(0.02);
        p.drainMidiEvents();
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 70, 0));
        f.run(0.02);
        p.drainMidiEvents();
        check(p.uiState().learn[70] == 9 && p.bias.load() == 0.f && p.learnTarget.load() == -1,
              "MIDI learn binds CC 70 to TAPE BIAS");

        // program change loads a slot; bank select reaches past 128
        f.at(0.0, juce::MidiMessage::programChange(1, 11));  // slot 12
        f.run(0.02);
        p.drainMidiEvents();
        check(p.currentSlot() == 12 && p.uiState().mode == 4, "program change 11 loads slot 012");
        f.at(0.0, juce::MidiMessage::programChange(1, 6));  // slot 7
        f.run(0.02);
        p.drainMidiEvents();
        check(p.currentSlot() == 7 && p.uiState() == odd, "program change 6 loads slot 007");
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 0, 3));
        f.at(0.001, juce::MidiMessage::programChange(1, 99));  // 3 * 128 + 99 + 1 = 484: empty
        f.run(0.02);
        p.drainMidiEvents();
        check(p.currentSlot() == 7, "bank 3 / program 99 is slot 484: empty, nothing changes");
    }
    {
        // with the editor open, CCs move the panel's knobs
        lift::LiftProcessor p;
        p.prepareToPlay(48000.0, 512);
        lift::LiftPanel panel(p);
        Feed f{p, 48000.0};
        f.at(0.0, juce::MidiMessage::controllerEvent(1, 21, 0));  // knob 2 of the screen on show (TAPE BIAS)
        f.run(0.02);
        p.drainMidiEvents();
        panel.advance(1.0 / 60.0);
        check(panel.enc(2, 1) == 0.f && p.bias.load() == 0.f && p.uiState().enc[2][1] == 0.f,
              "CC 21 turns the second knob of the open screen, panel and engine together");
    }
    folder.deleteRecursively();
}
