// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "tape/character.h"

#include <cstring>

namespace {

constexpr Character kRows[kCharacterCount] = {
    {"Deck", 0.35f, 0.00020f, 6.f, 0.00005f, 80.f, 1.5f, 0.7f, -78.f, 18000.f, 0.20f, 1.8f, 0.05f},
    {"Pocket", 0.80f, 0.00120f, 14.f, 0.00050f, 110.f, 6.0f, 1.1f, -54.f, 12000.f, 0.08f, 2.4f, 0.08f},
    {"Shed", 1.60f, 0.00220f, 9.f, 0.00020f, 90.f, 5.0f, 1.0f, -64.f, 14000.f, 0.12f, 2.0f, 0.07f},
    {"Cap", 0.15f, 0.00005f, 4.f, 0.00004f, 80.f, 0.0f, 0.7f, -80.f, 10000.f, 0.00f, 1.3f, 0.02f},
};

}  // namespace

const Character& character_row(int index) {
    if (index < 0 || index >= kCharacterCount) {
        return kRows[0];
    }
    return kRows[index];
}

int character_index(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int i = 0; i < kCharacterCount; ++i) {
        if (std::strcmp(name, kRows[i].name) == 0) {
            return i;
        }
    }
    return -1;
}
