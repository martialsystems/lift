// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: pitch (JCS R4, DECIDED by the user). Header-only C++17, no deps.
//
//   V/OCT (rack standard)  0 V = C3 = 130.8127826502993 Hz = MIDI 48, 1 V per octave.   V = (note - 48)/12
//   HZ/V LIN (separate)    1 V = C3 = 130.8127826502993 Hz, linear: f = kC3Hz * V.   note = 48 + 12*log2(V)
//   Tuning knobs (fine +-100 cents, OCT/footage, A4 reference) move a voice's pitch; they never change what a
//   jack's volts mean (R4.6). So no function here takes a tuning except voiceHz(), which is the receiver side.
//
// Names and semantics match BUSHIDO's rack/PitchLaw.h (namespace rack::pitch) so it can be reduced to
//     namespace rack::pitch { using namespace jidai::jcs::pitch; }
//   Law { VOct = 0, HzvLin = 1 }, kC3Hz, kRefNote, kRail, kLinFloor, note(), hz(), midiNote(), quantize(), noteName()
// plus the rack extras: voltsForNote(), hzToVolts(), voiceHz(), roninHzvLinHz(), lin55ToVoct(), clampPitch().

#include <cmath>
#include <cstdio>

namespace jidai::jcs::pitch {

enum class Law { VOct = 0, HzvLin = 1 };

// MIDI 48 at A4 = 440 (JCS R4.1): exactly 440 * 2^(-21/12) = 55 * 2^(15/12), as a double. Never the rounded 130.8128.
inline constexpr double kC3Hz = 130.8127826502993;
inline constexpr double kLin55Octaves = 1.25;      // log2(kC3Hz / 55) exactly: C3 is 15 semitones above 55 Hz
inline constexpr double kRefNote = 48.0;
inline constexpr double kRail = 5.0;             // JCS R4.4
inline constexpr double kLinFloor = 1.0 / 32.0;  // HZ/V LIN quantize: below 2^-5 V a row is silent (BUSHIDO QUANT)
inline constexpr double kRoninLinFloor = 0.05;   // RONIN VCO:HZ/V input floor (Vco.cpp, S-03)
inline constexpr double kOld55Hz = 55.0;         // retired BUSHIDO/SHOGUN v2 reference, only for migration

// Fractional MIDI note for a voltage, or NaN when the law has no note there (HZ/V LIN at or below 0 V).
inline double note (Law law, double volts)
{
    if (law == Law::VOct)
        return kRefNote + 12.0 * volts;
    return volts > 0.0 ? kRefNote + 12.0 * std::log2 (volts) : std::nan ("");
}

// Volts for a (fractional) note under a law. HZ/V LIN: 2^((note-48)/12).
inline double voltsForNote (Law law, double midiNote)
{
    const double oct = (midiNote - kRefNote) / 12.0;
    return law == Law::VOct ? oct : std::exp2 (oct);
}

// Frequency at the receiver's home tuning (RONIN 8', fine centred; SHOGUN OCT 0, TUNE 0, A4 440).
inline double hz (Law law, double volts)
{
    if (law == Law::VOct)
        return kC3Hz * std::exp2 (volts);
    return volts > 0.0 ? kC3Hz * volts : 0.0;
}

inline double hzToVolts (Law law, double freq)
{
    if (freq <= 0.0)
        return law == Law::VOct ? -kRail : 0.0;
    return law == Law::VOct ? std::log2 (freq / kC3Hz) : freq / kC3Hz;
}

// JCS R4.5: nearest MIDI note from the step's TARGET volts, clamped 0..127; -1 when there is no note.
inline int midiNote (Law law, double volts)
{
    const double n = note (law, volts);
    if (std::isnan (n))
        return -1;
    const long r = std::lround (n);
    return (int) (r < 0 ? 0 : r > 127 ? 127 : r);
}

// Snap to the nearest semitone under the law, never past the +-5 V rail (same rule as BUSHIDO QUANT SEMI).
inline double quantize (Law law, double volts)
{
    if (law == Law::VOct)
    {
        double s = std::round (12.0 * volts);
        if (s / 12.0 > kRail) s -= 1.0;
        if (s / 12.0 < -kRail) s += 1.0;
        return s / 12.0;
    }
    if (volts < kLinFloor)
        return 0.0;
    double s = std::round (12.0 * std::log2 (volts));
    if (std::exp2 (s / 12.0) > kRail) s -= 1.0;
    return std::exp2 (s / 12.0);
}

// "C3", "F#4": C3 = 48, so C-1 = 0. "--" for no note.
inline const char* noteName (int midi, char* buf, int size)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (midi < 0) { std::snprintf (buf, (size_t) size, "--"); return buf; }
    std::snprintf (buf, (size_t) size, "%s%d", names[midi % 12], midi / 12 - 1);
    return buf;
}

// Receiver side only: a voice's frequency for V/OCT volts plus its own tuning. The volt law itself is fixed.
inline double voiceHz (double voctVolts, double fineCents = 0.0, double a4Hz = 440.0, int octaves = 0)
{
    return kC3Hz * (a4Hz / 440.0) * std::exp2 (voctVolts + (double) octaves + fineCents / 1200.0);
}

// RONIN's linear input as it plays at 8': f = kC3Hz * max(V, 0.05).
inline double roninHzvLinHz (double volts) { return kC3Hz * (volts > kRoninLinFloor ? volts : kRoninLinFloor); }

// Migration (SHOGUN v2.0/2.1 HZ/V, old BUSHIDO MIDI law): linear 1 V = 55 Hz -> V/OCT.
// V' = log2(V * 55 / C3) = log2(V) - 1.25 exactly (55 / C3 = 2^(-15/12)); no rounded constant enters.
// 1.1.2: V is floored at kLin55Floor = 1 mV first (SHOGUN spec 12.3, log2(max(V, 1e-3)) - 1.25): 0 V, negative and NaN
// inputs give log2(1e-3) - 1.25 = -11.216 V (was -5 V for V <= 0, and unbounded below 1 mV). Both are far below the
// lowest playable note; receivers clamp pitch to the rail as usual.
inline constexpr double kLin55Floor = 1.0e-3;
inline double lin55ToVoct (double oldVolts)
{
    return std::log2 (oldVolts > kLin55Floor ? oldVolts : kLin55Floor) - kLin55Octaves;
}

// JCS R4.4: pitch outputs stop hard at +-5 V and raise the over-range flag.
inline double clampPitch (double volts, bool& over)
{
    if (volts > kRail) { over = true; return kRail; }
    if (volts < -kRail) { over = true; return -kRail; }
    return volts;
}

} // namespace jidai::jcs::pitch
