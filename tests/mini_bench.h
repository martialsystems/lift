// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
// Audio-callback worst case while LIFT / DROP / undo run (tests/mini_check.cpp).
#pragma once
#include "LiftProcessor.h"
#include "tape/transport.h"
#include <cstdio>
#include <random>

namespace minibench {

struct Result { const char* name; double worstMs; };

// Fill all four tracks with noise, loop the whole 60 s tape, play, then
// time every 256-sample callback for 40 blocks after each command.
inline std::vector<Result> run(double* budgetMs) {
    using lift::Cmd;
    lift::LiftProcessor p;
    p.clearDrumPatterns();
    p.prepareToPlay(48000.0, 256);
    auto& rt = p.runtime();
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-0.1f, 0.1f);
    for (int t = 0; t < 4; ++t)
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < rt.frames; ++i) rt.ch[t][c][i] = u(rng);
    p.send(Cmd::LoopSet, 0, rt.frames);
    p.send(Cmd::Transport, 1, 0);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    auto block = [&] {
        buf.clear();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        p.processBlock(buf, midi);
        midi.clear();
        return juce::Time::getMillisecondCounterHiRes() - t0;
    };
    for (int i = 0; i < 50; ++i) block();
    auto timeAfter = [&](auto issue) {
        issue();
        double worst = 0;
        for (int i = 0; i < 400; ++i) worst = std::max(worst, block());
        return worst;
    };
    std::vector<Result> out;
    double idle = 0;
    for (int i = 0; i < 400; ++i) idle = std::max(idle, block());
    out.push_back({"idle (playing)", idle});
    out.push_back({"LIFT (60 s loop)", timeAfter([&] { p.send(Cmd::Lift); })});
    out.push_back({"DROP (60 s clip)", timeAfter([&] { p.send(Cmd::Drop); })});
    out.push_back({"UNDO DROP", timeAfter([&] { p.send(Cmd::UndoDrop); })});
    out.push_back({"SHIFT+LIFT (4 tracks)", timeAfter([&] { p.send(Cmd::LiftAll); })});
    auto clip = p.makeClip(std::vector<float>(48000 * 30, 0.05f), std::vector<float>(48000 * 30, 0.05f), true);
    out.push_back({"DROP clip to loop (30 s)", timeAfter([&] { p.dropToLoop(clip, 1); })});
    out.push_back({"UNDO (clip drop)", timeAfter([&] { p.send(Cmd::UndoAudio); })});
    out.push_back({"LIFT x3 + DROP x3 burst", timeAfter([&] {
        for (int k = 0; k < 3; ++k) { p.send(Cmd::Lift); p.send(Cmd::Drop); }
    })});
    *budgetMs = 256.0 / 48.0;
    return out;
}

}  // namespace minibench
