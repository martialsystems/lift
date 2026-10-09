// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "tape/engine.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

// Off the audio thread. Builds the 4x oversampler for the record chain.

void tape_engine_prepare(TapeEngine& e, double sampleRate) {
    tape_engine_release(e);
    e.sampleRate = sampleRate > 0.0 ? sampleRate : static_cast<double>(kSampleRate);
    e.os = new juce::dsp::Oversampling<float>(2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                              true, true);
    e.os->initProcessing(static_cast<size_t>(kTapeMaxBlock));
    e.latency = static_cast<int>(std::lround(e.os->getLatencyInSamples()));
    tape_engine_set_params(e, e.p);
    tape_engine_reset(e);
}

void tape_engine_release(TapeEngine& e) {
    delete e.os;
    e.os = nullptr;
    e.latency = 0;
}
