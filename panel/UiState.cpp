// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "UiState.h"

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

UiState::UiState() {
    for (int m = 0; m < 5; ++m) {
        for (int i = 0; i < 4; ++i) {
            enc[static_cast<size_t>(m)][static_cast<size_t>(i)] = ui::ENC_DEFAULT[m][i];
        }
    }
    cords = {{0, 0, 0, false}, {1, 1, 1, false}, {8, 3, 2, false}, {15, 2, 3, false}, {9, 9, 0, false}, {5, 12, 1, false}};
    learn.fill(-1);
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
    juce::StringArray l;
    for (int cc = 0; cc < 128; ++cc) {
        if (learn[static_cast<size_t>(cc)] >= 0) {
            l.add(juce::String(cc) + ":" + juce::String(learn[static_cast<size_t>(cc)]));
        }
    }
    t.setProperty("learn", l.joinIntoString(" "), nullptr);
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
    s.color = clampInt(t["color"], 0, 5, s.color);
    s.stack = t.getProperty("stack", s.stack);
    const auto cs = t.getChildWithName("Cords");
    if (cs.isValid()) {
        s.cords.clear();
        for (const auto& ct : cs) {
            if (static_cast<int>(s.cords.size()) >= kMaxCords) {
                break;
            }
            Cord c;
            c.o = clampInt(ct["o"], 0, kJacks - 1, 0);
            c.i = clampInt(ct["i"], 0, kJacks - 1, 0);
            c.c = clampInt(ct["c"], 0, 5, 0);
            c.st = ct.getProperty("st", false);
            s.cords.push_back(c);
        }
    }
    for (const auto& pair : split(t["learn"])) {
        const int cc = pair.upToFirstOccurrenceOf(":", false, false).getIntValue();
        const int tg = pair.fromFirstOccurrenceOf(":", false, false).getIntValue();
        if (cc >= 0 && cc < 128 && tg >= 0 && tg < kKnobTargets) {
            s.learn[static_cast<size_t>(cc)] = tg;
        }
    }
    return s;
}

}  // namespace lift
