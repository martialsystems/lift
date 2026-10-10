// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1 (JIDAI_Cross_Unit_Patching.md section 5), shared by BUSHIDO, RONIN, SHOGUN, ORIGAMI
// and the JIDAI RACK. Header-only C++17, no framework, no allocation in the per-sample helpers.
// Vendor the whole jidai-common/include/jidai folder; include this umbrella or the single headers:
//   jidai/jcs/Detect.h     R2 gate levels, R3 Schmitt 1.0/0.5 V, R3s S-trig 1.0/1.5 V
//   jidai/jcs/Volts.h      R1 volts and host x5 / x0.2, R4.4 rail clamp, R15 over-range LED, R16 hook (pending)
//   jidai/jcs/Pitch.h      R4 V/OCT 0 V = C3 = 130.8127826502993 Hz, HZ/V LIN, MIDI, quantize, lin55 migration
//   jidai/jcs/Roles.h      R14 role colours and glyphs, R4.3 badges
//   jidai/jcs/JackId.h     R6 canonical ids PREFIX#N/SECTION:LABEL, parser, alias tables
//   jidai/jcs/State.h      R7 format/unit header and migration chains
//   jidai/jcs/Graph.h      R9 feedback classification + run order, R11 path latency and skew
//   jidai/dsp/TripleShaper.h   the shared 3-stage wave shaper (SHOGUN WAVE + ORIGAMI), see its header
#include "jcs/Detect.h"
#include "jcs/Graph.h"
#include "jcs/JackId.h"
#include "jcs/Pitch.h"
#include "jcs/Roles.h"
#include "jcs/State.h"
#include "jcs/Volts.h"

namespace jidai::jcs {
inline constexpr int kStandardMajor = 1;
inline constexpr int kStandardMinor = 1;
inline constexpr const char* kStandardName = "JCS v1.1";
}
