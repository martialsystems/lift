// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: gate levels and the one trigger detector (JCS R2, R3, R3s). Header-only C++17, no deps.
//
//   jidai::jcs::kGateLow / kGateHigh           0 V / +5 V on every Gate-typed output (R2)
//   jidai::jcs::gateVolts (bool high)           0 or 5 V
//   jidai::jcs::triggerPulseSamples (sr)        max(1, round(0.001*sr)): a trigger lasts >= 1 ms (R2)
//   jidai::jcs::Schmitt                         V-trig input (R3): high when V > 1.0, low when V < 0.5;
//                                               process(v) returns Edge::Rising on the sample V first exceeds 1.0
//   jidai::jcs::StrigDetector                   S-trig input (R3s, RONIN EG TRIG only): held when V < 1.0,
//                                               released when V > 1.5; unpatched rest is +5 V (released)
//   jidai::jcs::strigVoltsFor (bool sourceHigh) R3s conversion for a Gate cable landing on an S-trig input:
//                                               high -> 0 V (held), low -> +5 V (released)
// State persists across blocks (keep one detector per input, or per cable for R3s conversion).

namespace jidai::jcs {

inline constexpr float kGateLow = 0.0f;       // JCS R2
inline constexpr float kGateHigh = 5.0f;      // JCS R2
inline constexpr float kTrigHigh = 1.0f;      // JCS R3: goes high above this
inline constexpr float kTrigLow = 0.5f;       // JCS R3: goes low below this
inline constexpr float kStrigHeld = 1.0f;     // JCS R3s: held below this
inline constexpr float kStrigRelease = 1.5f;  // JCS R3s: released above this
inline constexpr float kStrigRest = 5.0f;     // JCS R3s / R10: unpatched S-trig input rests released

constexpr float gateVolts (bool high) noexcept { return high ? kGateHigh : kGateLow; }

// JCS R3s: what a non-S-trig Gate source contributes on a cable into an S-trig input.
constexpr float strigVoltsFor (bool sourceHigh) noexcept { return sourceHigh ? 0.0f : 5.0f; }

// JCS R2: trigger pulses last at least 1 ms so they survive a 2-sample detector.
inline int triggerPulseSamples (double sampleRate) noexcept
{
    const double n = 0.001 * sampleRate;
    const long r = (long) (n + 0.5);
    return r < 1 ? 1 : (int) r;
}

enum class Edge { None, Rising, Falling };

// JCS R3: the single V-trig detector for every clock, gate, reset, run, step and S&H input.
struct Schmitt
{
    bool high = false;

    Edge process (float v) noexcept
    {
        if (! high && v > kTrigHigh)
        {
            high = true;
            return Edge::Rising;
        }
        if (high && v < kTrigLow)
        {
            high = false;
            return Edge::Falling;
        }
        return Edge::None;
    }
    bool rising (float v) noexcept { return process (v) == Edge::Rising; }
    void reset (bool isHigh = false) noexcept { high = isHigh; }
};

// JCS R3s: S-trig input (active low). Edge::Rising means "became held" (a trigger starts).
struct StrigDetector
{
    bool held = false;

    Edge process (float v) noexcept
    {
        if (! held && v < kStrigHeld)
        {
            held = true;
            return Edge::Rising;
        }
        if (held && v > kStrigRelease)
        {
            held = false;
            return Edge::Falling;
        }
        return Edge::None;
    }
    void reset (bool isHeld = false) noexcept { held = isHeld; }
};

} // namespace jidai::jcs
