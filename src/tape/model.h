// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/character.h"

#include <cstdint>

struct BiquadState {
    float z1;
    float z2;
};

struct BiquadCoeff {
    float b0, b1, b2, a1, a2;
};

struct ChannelFilter {
    float lp;
    float emph;
    float hissLp;
    float env;
    float dcX;
    float dcY;
    BiquadState bump;
};

struct CharacterCoeffs {
    float lpA;
    float emphA;
    float envA;
    float wowInc;
    float wowDepth;
    float flutterA;
    float flutterDepth;
    float hissA;
    float hissAmp;
    BiquadCoeff bump;
    float preEmph;
    float drive;
    float asym;
    float bias;
};

void fill_coeffs(const Character& row, CharacterCoeffs& dst) noexcept;
void reset_filter(ChannelFilter& st) noexcept;
float next_white(uint32_t& rng) noexcept;
float eco_record(ChannelFilter& st, float x, const CharacterCoeffs& c) noexcept;
float eco_play(ChannelFilter& st, float x, const CharacterCoeffs& c, uint32_t& rng) noexcept;
