// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/transport.h"

// Audio thread. No allocation, no disk, no socket. Buffers come from prepare.

void process_block(TapeRuntime& rt, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;
