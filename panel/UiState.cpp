// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "UiState.h"

#include <cstring>

#include "PanelData.h"

namespace lift {

namespace {

int clampInt(const juce::var& v, int lo, int hi, int fallback) {
    if (v.isVoid()) {
        return fallback;
    }
    return juce::jlimit(lo, hi, static_cast<int>(v));
}

juce::String joinInts(const int* x, int n) {
    juce::StringArray a;
    for (int k = 0; k < n; ++k) {
        a.add(juce::String(x[k]));
    }
    return a.joinIntoString(" ");
}

juce::StringArray split(const juce::var& v) {
    return juce::StringArray::fromTokens(v.toString(), " ", "");
}

bool inSet(int v, std::initializer_list<int> s) {
    for (int x : s) {
        if (x == v) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace {
// v2 jack names (before the v3.1 jack list and matrix)
const char* const kV2Outs[16] = {"A PITCH", "A GATE", "B PITCH", "B GATE", "DRUM", "CLOCK", "RESET", "RADIO",
                                 "LFO",     "ENV",    "S&H",     "VCA",    "SLEW", "QUANT", "HEAD 1", "HEAD 2"};
const char* const kV2Ins[16] = {"PITCH",  "GATE", "FM IDX", "CUTOFF", "SLICE", "FX MAC", "CLK IN", "RST IN",
                                "VCA IN", "VCA",  "SLEW",   "QUANT",  "S&H",   "SPEED",  "REVERSE", "BIAS"};
int findName(const ui::JackDef* list, const char* n) {
    for (int k = 0; k < 16; ++k) {
        if (std::strcmp(list[k].n, n) == 0) {
            return k;
        }
    }
    return -1;
}
}  // namespace

// A v2 cable, by name, onto the v3.1 bay: a jack-to-jack cable if both ends
// still exist, else a +100 % pin if the source became a matrix row and the
// destination a column. False = it has no v3.1 equivalent (dropped).
bool UiState::remapV2(Cord& c, std::array<std::uint8_t, 256>& pins) {
    const char* on = kV2Outs[c.o];
    const char* in = kV2Ins[c.i];
    const int o = findName(ui::OUTS, on), i = findName(ui::INS, in);
    if (o >= 0 && i >= 0) {
        c.o = o;
        c.i = i;
        return true;
    }
    const int r = findName(ui::MXR, on), col = findName(ui::MXC, in);
    if (r >= 0 && col >= 0) {
        pins[static_cast<size_t>(r * 16 + col)] = 1;
    }
    return false;
}

UiState::UiState() {
    for (int m = 0; m < 5; ++m) {
        for (int i = 0; i < 4; ++i) {
            enc[static_cast<size_t>(m)][static_cast<size_t>(i)] = ui::ENC_DEFAULT[m][i];
        }
    }
    // Opening patch: the keyboard into the synth and the clock into CLK IN
    // (what the normals do anyway, made visible), no pins. The v3.1
    // prototype also opens with HEAD 1 -> SPEED and MIX L -> AUDIO L, which
    // bend a fresh loop's pitch; those stay out of the default.
    cords = {{3, 0, 2, false}, {0, 2, 1, false}, {6, 5, 1, false}};
    learn.fill(-1);
    for (int k = 0; k < eng::kKits; ++k) {
        for (int v = 0; v < eng::kDrumVoices; ++v) {
            drumPat[static_cast<size_t>(k)][static_cast<size_t>(v)] = eng::kitInfo(k).pattern[v];
        }
    }
    for (int v = 0; v < eng::kDrumVoices; ++v) {
        const eng::DrumKnobs d = eng::kitDefaultKnobs(0, v);
        drumKnobs[static_cast<size_t>(v)] = {d.pitch, d.choke, d.decay};
    }
    for (int t = 0; t < eng::kFxTypes; ++t) {
        for (int k = 0; k < 4; ++k) {
            fxKnobs[static_cast<size_t>(t)][static_cast<size_t>(k)] = eng::kFxDefaults[t][k];
        }
    }
}

juce::ValueTree UiState::toTree() const {
    juce::ValueTree t("LiftPanel");
    t.setProperty("mode", mode, nullptr);
    t.setProperty("bay", bay, nullptr);
    t.setProperty("fx", fx, nullptr);
    t.setProperty("sel", joinInts(sel.data(), 5), nullptr);
    juce::StringArray e;
    for (const auto& row : enc) {
        for (float v : row) {
            e.add(juce::String(static_cast<double>(v), 12));  // locale independent; parses back to the same float
        }
    }
    t.setProperty("enc", e.joinIntoString(" "), nullptr);
    t.setProperty("loop", loop, nullptr);
    t.setProperty("loopIn", loopIn, nullptr);
    t.setProperty("loopOut", loopOut, nullptr);
    t.setProperty("marks", joinInts(marks.data(), 4), nullptr);
    t.setProperty("nextMark", nextMark, nullptr);
    t.setProperty("arm", arm, nullptr);
    int m[4];
    for (int k = 0; k < 4; ++k) {
        m[k] = mutes[static_cast<size_t>(k)] ? 1 : 0;
    }
    t.setProperty("mutes", joinInts(m, 4), nullptr);
    t.setProperty("oct", oct, nullptr);
    t.setProperty("transpose", transpose, nullptr);
    t.setProperty("seqDiv", seqDiv, nullptr);
    t.setProperty("drumDiv", drumDiv, nullptr);
    t.setProperty("drumLen", drumLen, nullptr);
    t.setProperty("swing", swing, nullptr);
    t.setProperty("recSource", recSource, nullptr);
    t.setProperty("character", character, nullptr);
    t.setProperty("cassette", cassette, nullptr);
    t.setProperty("color", color, nullptr);
    t.setProperty("stack", stack, nullptr);
    juce::ValueTree cs("Cords");
    for (const auto& c : cords) {
        juce::ValueTree ct("Cord");
        ct.setProperty("o", c.o, nullptr);
        ct.setProperty("i", c.i, nullptr);
        ct.setProperty("c", c.c, nullptr);
        ct.setProperty("st", c.st, nullptr);
        cs.appendChild(ct, nullptr);
    }
    t.appendChild(cs, nullptr);
    t.setProperty("jacks", "v31", nullptr);
    {
        juce::StringArray pa;
        for (int k = 0; k < 256; ++k) {
            if (pins[static_cast<size_t>(k)] != 0) {
                pa.add(juce::String(k) + ":" + juce::String(static_cast<int>(pins[static_cast<size_t>(k)])));
            }
        }
        t.setProperty("pins", pa.joinIntoString(" "), nullptr);
    }
    t.setProperty("recJackLifts", recJackLifts, nullptr);
    juce::StringArray l;
    for (int cc = 0; cc < 128; ++cc) {
        if (learn[static_cast<size_t>(cc)] >= 0) {
            l.add(juce::String(cc) + ":" + juce::String(learn[static_cast<size_t>(cc)]));
        }
    }
    t.setProperty("learn", l.joinIntoString(" "), nullptr);
    juce::StringArray pat;
    for (const auto& kit : drumPat) {
        for (uint32_t m : kit) {
            pat.add(juce::String::toHexString(static_cast<juce::int64>(m)));
        }
    }
    t.setProperty("drumPat", pat.joinIntoString(" "), nullptr);
    t.setProperty("drumVoice", drumVoice, nullptr);
    juce::StringArray dk;
    for (const auto& v : drumKnobs) {
        for (float x : v) {
            dk.add(juce::String(static_cast<double>(x), 6));
        }
    }
    t.setProperty("drumKnobs", dk.joinIntoString(" "), nullptr);
    t.setProperty("fxType", fxType, nullptr);
    juce::StringArray fk;
    for (const auto& v : fxKnobs) {
        for (float x : v) {
            fk.add(juce::String(static_cast<double>(x), 6));
        }
    }
    t.setProperty("fxKnobs", fk.joinIntoString(" "), nullptr);
    return t;
}

UiState UiState::fromTree(const juce::ValueTree& t) {
    UiState s;
    if (!t.isValid()) {
        return s;
    }
    s.mode = clampInt(t["mode"], 0, 4, s.mode);
    s.bay = t.getProperty("bay", s.bay);
    s.fx = t.getProperty("fx", s.fx);
    const auto sel = split(t["sel"]);
    for (int k = 0; k < 5 && k < sel.size(); ++k) {
        s.sel[static_cast<size_t>(k)] = juce::jlimit(0, 7, sel[k].getIntValue());
    }
    const auto e = split(t["enc"]);
    if (e.size() == 20) {
        for (int k = 0; k < 20; ++k) {
            s.enc[static_cast<size_t>(k / 4)][static_cast<size_t>(k % 4)] = juce::jlimit(0.f, 1.f, static_cast<float>(e[k].getDoubleValue()));
        }
    }
    s.loop = t.getProperty("loop", s.loop);
    s.loopIn = clampInt(t["loopIn"], 0, 1 << 30, s.loopIn);
    s.loopOut = clampInt(t["loopOut"], 0, 1 << 30, s.loopOut);
    const auto mk = split(t["marks"]);
    for (int k = 0; k < 4 && k < mk.size(); ++k) {
        s.marks[static_cast<size_t>(k)] = juce::jmax(0, mk[k].getIntValue());
    }
    s.nextMark = clampInt(t["nextMark"], 0, 3, s.nextMark);
    s.arm = clampInt(t["arm"], 0, 3, s.arm);
    const auto mu = split(t["mutes"]);
    for (int k = 0; k < 4 && k < mu.size(); ++k) {
        s.mutes[static_cast<size_t>(k)] = mu[k].getIntValue() != 0;
    }
    s.oct = clampInt(t["oct"], -3, 3, s.oct);
    s.transpose = clampInt(t["transpose"], -2, 2, s.transpose);
    const int sd = clampInt(t["seqDiv"], 0, 64, s.seqDiv);
    s.seqDiv = inSet(sd, {4, 8, 12, 16, 32}) ? sd : s.seqDiv;
    const int dd = clampInt(t["drumDiv"], 0, 64, s.drumDiv);
    s.drumDiv = inSet(dd, {4, 8, 12, 16, 32}) ? dd : s.drumDiv;
    const int dl = clampInt(t["drumLen"], 0, 64, s.drumLen);
    s.drumLen = inSet(dl, {4, 8, 12, 16, 24, 32}) ? dl : s.drumLen;
    s.swing = clampInt(t["swing"], 0, 60, s.swing);
    s.recSource = clampInt(t["recSource"], 0, 2, s.recSource);
    s.character = clampInt(t["character"], 0, 3, s.character);
    s.cassette = t.hasProperty("cassette") ? static_cast<bool>(t["cassette"]) : s.cassette;
    s.color = clampInt(t["color"], 0, 5, s.color);
    s.stack = t.getProperty("stack", s.stack);
    const bool v31 = t["jacks"].toString() == "v31";
    const auto cs = t.getChildWithName("Cords");
    if (cs.isValid()) {
        s.cords.clear();
        if (!v31) {
            s.pins.fill(0);  // a v2 state: its routing is rebuilt below
        }
        for (const auto& ct : cs) {
            if (static_cast<int>(s.cords.size()) >= kMaxCords) {
                break;
            }
            Cord c;
            c.o = clampInt(ct["o"], 0, kJacks - 1, 0);
            c.i = clampInt(ct["i"], 0, kJacks - 1, 0);
            c.c = clampInt(ct["c"], 0, 5, 0);
            c.st = ct.getProperty("st", false);
            if (!v31 && !remapV2(c, s.pins)) {
                continue;
            }
            c.c = ui::cableCloth(c.o);
            s.cords.push_back(c);
        }
    }
    if (v31) {
        s.pins.fill(0);
        for (const auto& pair : split(t["pins"])) {
            const int k = pair.upToFirstOccurrenceOf(":", false, false).getIntValue();
            const int v = pair.fromFirstOccurrenceOf(":", false, false).getIntValue();
            if (k >= 0 && k < 256 && v >= 0 && v <= 3) {
                s.pins[static_cast<size_t>(k)] = static_cast<std::uint8_t>(v);
            }
        }
    }
    s.recJackLifts = t.getProperty("recJackLifts", s.recJackLifts);
    for (const auto& pair : split(t["learn"])) {
        const int cc = pair.upToFirstOccurrenceOf(":", false, false).getIntValue();
        const int tg = pair.fromFirstOccurrenceOf(":", false, false).getIntValue();
        if (cc >= 0 && cc < 128 && tg >= 0 && tg < kKnobTargets) {
            s.learn[static_cast<size_t>(cc)] = tg;
        }
    }
    const auto pat = split(t["drumPat"]);
    if (pat.size() == eng::kKits * eng::kDrumVoices) {
        for (int k = 0; k < pat.size(); ++k) {
            s.drumPat[static_cast<size_t>(k / eng::kDrumVoices)][static_cast<size_t>(k % eng::kDrumVoices)] =
                static_cast<uint32_t>(pat[k].getHexValue64() & 0xffffffff);
        }
    }
    s.drumVoice = clampInt(t["drumVoice"], 0, eng::kDrumVoices - 1, s.drumVoice);
    const auto dk = split(t["drumKnobs"]);
    if (dk.size() == eng::kDrumVoices * 3) {
        for (int k = 0; k < dk.size(); ++k) {
            s.drumKnobs[static_cast<size_t>(k / 3)][static_cast<size_t>(k % 3)] =
                juce::jlimit(0.f, 1.f, static_cast<float>(dk[k].getDoubleValue()));
        }
    }
    s.fxType = clampInt(t["fxType"], 0, eng::kFxTypes - 1, s.fxType);
    const auto fk = split(t["fxKnobs"]);
    if (fk.size() == eng::kFxTypes * 4) {
        for (int k = 0; k < fk.size(); ++k) {
            s.fxKnobs[static_cast<size_t>(k / 4)][static_cast<size_t>(k % 4)] =
                juce::jlimit(0.f, 1.f, static_cast<float>(fk[k].getDoubleValue()));
        }
    }
    return s;
}

}  // namespace lift
