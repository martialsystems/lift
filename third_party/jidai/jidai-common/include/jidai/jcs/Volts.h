// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: volts, the host boundary, the rail and the over-range LED (JCS R1, R4.4, R15, R16 hook).
// Header-only C++17, no deps.
//
//   kNominal = 5 V                       audio +-5 V, bipolar CV +-5 V, unipolar CV 0..+5 V (R1); the graph never clamps
//   hostToVolts (x) = 5x, voltsToHost (v) = 0.2v       host float <-> volts on every device (R1)
//   clampRail (v, &over)                 hard +-5 V rail for pitch sources (R4.4): returns +-5 exactly and sets over
//   OverRangeLed                         R15: lit while |V| > 5.5 V has lasted more than 10 ms
//   PitchRailFlag                        R4.4 + R15: rail clamp whose flag latches until the next note
//   AudioLevel / r16BoundaryGain()       R16 HOOK ONLY (pending user decision): always returns 1.0

#include <cmath>

namespace jidai::jcs {

inline constexpr float kNominal = 5.0f;          // JCS R1
inline constexpr float kRail = 5.0f;             // JCS R4.4: pitch sources stop here
inline constexpr float kOverRangeVolts = 5.5f;   // JCS R15
inline constexpr double kOverRangeSeconds = 0.010; // JCS R15

constexpr float hostToVolts (float x) noexcept { return 5.0f * x; }
constexpr float voltsToHost (float v) noexcept { return 0.2f * v; }

// JCS R4.4: a hard rail. Returns v unchanged inside +-5 V, exactly +-5 V outside, and sets `over` (never clears it).
inline float clampRail (float v, bool& over) noexcept
{
    if (v > kRail) { over = true; return kRail; }
    if (v < -kRail) { over = true; return -kRail; }
    if (std::isnan (v)) { over = true; return 0.0f; }   // NaN never reaches a cable
    return v;
}

// JCS R15: lit when |V| > 5.5 V for more than 10 ms; goes dark on the first sample back inside.
class OverRangeLed
{
public:
    void prepare (double sampleRate) noexcept
    {
        limit_ = (long) std::ceil (kOverRangeSeconds * (sampleRate > 0.0 ? sampleRate : 48000.0));
        count_ = 0;
    }
    bool process (float v) noexcept
    {
        if (std::fabs (v) > kOverRangeVolts) { if (count_ <= limit_) ++count_; }
        else count_ = 0;
        return lit();
    }
    bool lit() const noexcept { return count_ > limit_; }
    void reset() noexcept { count_ = 0; }

private:
    long limit_ = 480;
    long count_ = 0;
};

// JCS R4.4 + R15: pitch outputs clamp at +-5 V and latch their flag until the next note.
struct PitchRailFlag
{
    bool latched = false;
    float process (float v) noexcept { return clampRail (v, latched); }
    void noteOn() noexcept { latched = false; }
};

// ---- JCS R16 (OPTIONAL, PENDING USER DECISION) --------------------------------------------------------------
// Foreign signal levels at the rack boundary. NOT IMPLEMENTED on purpose: the user has not decided whether a
// +-2.5 V-convention device joins the rack, nor whether the scaling is fixed or switchable.
// The hook is here so a device can declare its level; r16BoundaryGain() is identity for every level until the
// decision lands. When it does, the proposal is: ModularHalfLevel audio inputs x0.5, audio outputs x2; CV/gates unscaled.
enum class AudioLevel { Jidai5V, ModularHalfLevel };   // ModularHalfLevel: a ±2.5 V audio device; neutral name, no third-party names in code
inline constexpr bool kR16Enabled = false;
constexpr float r16BoundaryGain (AudioLevel, bool /*isAudioRole*/, bool /*isInput*/) noexcept { return 1.0f; }

} // namespace jidai::jcs
