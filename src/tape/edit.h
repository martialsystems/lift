// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/transport.h"

void tape_lift(TapeRuntime& rt);
void tape_drop(TapeRuntime& rt, bool overdub);
// The DROP edge fade (2 ms ramps), for callers that drop in slices.
int tape_edge_len(int n) noexcept;
float tape_edge_gain(int i, int n, int edge) noexcept;
