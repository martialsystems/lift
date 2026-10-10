// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/transport.h"

// Audio thread. No allocation, no disk, no socket. Buffers come from prepare.
//
// One block runs in three steps so the patch graph can sit between them
// (src/engine/patch.h: N_TRANSPORT, N_H1..N_H4, N_MIX):
//   tape_transport  advances the shared transport (motor glide, loop wrap,
//                   tape stop) and keeps the block's positions; EOC pulses
//   tape_head       reads one loop for the block at the transport position
//                   plus the loop's own offset (SPEED / REVERSE / SCRUB of the
//                   armed loop move only that loop)
//   tape_mix        records the armed loop, mixes the loops' heads (fader,
//                   shelves, balance), runs the playback chain, metronome
// process_block() is the three with nothing patched (n any size).

// Returns the samples the transport ran (0 when stopped). n <= kTapeMaxBlock.
// eoc (optional): 5 V for 2 ms each time the loop wraps.
int tape_transport(TapeRuntime& rt, int n, float* eoc) noexcept;
// Loop t for the block tape_transport ran. speedCv: volts, 1 V = 1 semitone
// (null = none); rev: play this loop backwards; scrubCv: +-5 V = +-1 beat
// nudge (null = none). Writes rt.headL/R[t] and, when non-null, the mono head
// (+-1 full scale, before the fader).
void tape_head(TapeRuntime& rt, int t, const float* speedCv, bool rev, const float* scrubCv, float* mono) noexcept;
// Record + mix for the block. sendBus (optional): post-fader dry sum of the loops.
void tape_mix(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, float* sendBus, int n) noexcept;

void process_block(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;
