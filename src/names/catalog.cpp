// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "names/catalog.h"

namespace {

const char* const kEngines[] = {
    "Loom", "Bend", "Fold", "Ratio", "Wire", "Swarm", "Spool", "Spare"};
const char* const kEffects[] = {
    "Drip", "Echo", "Crush", "Tilt", "Accent", "Chorus", "Handset", "Space"};
const char* const kSequencers[] = {"Steps", "Latch"};
const char* const kCharacters[] = {"Deck", "Pocket", "Shed", "Cap"};
const char* const kDrums[] = {"Tap"};
const char* const kGlyphs[] = {
    "Loom.saw", "Loom.detune", "Loom.cutoff", "Loom.envelope",
    "Bend.carrier", "Bend.amount", "Bend.skew", "Bend.tone",
    "Fold.fold", "Fold.symmetry", "Fold.offset", "Fold.tone",
    "Ratio.algorithm", "Ratio.ratios", "Ratio.index", "Ratio.feedback",
    "Wire.pitch", "Wire.damp", "Wire.position", "Wire.decay",
    "Swarm.count", "Swarm.spread", "Swarm.tone", "Swarm.decay",
    "Spool.start", "Spool.length", "Spool.tune", "Spool.level",
    "Spare.one", "Spare.two", "Spare.three", "Spare.four",
    "Tap.pitch", "Tap.tone", "Tap.decay", "Tap.snap",
    "Drip.decay", "Drip.size", "Drip.tone", "Drip.mix",
    "Echo.time", "Echo.feedback", "Echo.tone", "Echo.mix",
    "Crush.rate", "Crush.bits", "Crush.tone", "Crush.mix",
    "Tilt.tilt", "Tilt.drive", "Tilt.trim", "Tilt.mix",
    "Accent.attack", "Accent.decay", "Accent.amount", "Accent.mix",
    "Chorus.rate", "Chorus.depth", "Chorus.tone", "Chorus.mix",
    "Handset.band", "Handset.grit", "Handset.level", "Handset.mix",
    "Space.decay", "Space.size", "Space.damp", "Space.mix",
    "Steps.note", "Steps.gate", "Steps.velocity", "Steps.swing",
    "Latch.rate", "Latch.range", "Latch.gate", "Latch.swing"};

template <typename T, int N>
int count_of(const T (&)[N]) {
    return N;
}

template <typename T, int N>
const char* at(const T (&table)[N], int index) {
    if (index < 0 || index >= N) {
        return "";
    }
    return table[index];
}

}  // namespace

int catalog_engine_count() { return count_of(kEngines); }
const char* catalog_engine(int index) { return at(kEngines, index); }
int catalog_effect_count() { return count_of(kEffects); }
const char* catalog_effect(int index) { return at(kEffects, index); }
int catalog_sequencer_count() { return count_of(kSequencers); }
const char* catalog_sequencer(int index) { return at(kSequencers, index); }
int catalog_character_count() { return count_of(kCharacters); }
const char* catalog_character(int index) { return at(kCharacters, index); }
int catalog_drum_count() { return count_of(kDrums); }
const char* catalog_drum(int index) { return at(kDrums, index); }
int catalog_glyph_count() { return count_of(kGlyphs); }
const char* catalog_glyph(int index) { return at(kGlyphs, index); }
