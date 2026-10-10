// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: signal roles, colours and glyphs (JCS R14, R4.3). Header-only C++17, no deps.
//
//   enum class Role { STrig, Audio, VOct, GateClk, HzvLin, CV }   ordered by luminance (dark -> light)
//   roleInfo (Role) -> { name, rgb (0xRRGGBB), glyph (UTF-8), luminance }
//   roleArgb (Role)                 0xFFRRGGBB, ready for juce::Colour
//   roleFromName ("V/OCT", &role)   parse a role name
//   cableBadge (sourceRole, destRole)   R4.3 / R14 warnings: PitchLaw (V/OCT <-> HZ/V LIN, shown as the not-equal
//                                       badge), AudioIntoClock (AUDIO -> GATE/CLK input), GateToStrig (converted by
//                                       the graph, R3s, shown as "gate -> s-trig" in the cable list)
// A cable's colour is its SOURCE jack's role unless the user overrides it; the glyph always shows the role.
// Colour never changes sound.

#include <cstdint>
#include <cstring>

namespace jidai::jcs {

enum class Role : std::uint8_t { STrig = 0, Audio, VOct, GateClk, HzvLin, CV };
inline constexpr int kRoleCount = 6;

struct RoleInfo
{
    const char* name;
    std::uint32_t rgb;
    const char* glyph;      // UTF-8
    double luminance;       // WCAG relative luminance, as tabled in JCS R14
};

inline const RoleInfo& roleInfo (Role r) noexcept
{
    static const RoleInfo table[kRoleCount] = {
        { "S-TRIG",   0xb129b1, "\xe2\x8a\x94", 0.141 },  // ⊔
        { "AUDIO",    0xe53b2f, "\xe2\x88\xbf", 0.200 },  // ∿
        { "V/OCT",    0x6590f3, "\xe2\x99\xaa", 0.292 },  // ♪
        { "GATE/CLK", 0x2ec554, "\xe2\x8a\x93", 0.412 },  // ⊓
        { "HZ/V LIN", 0x5cd5ed, "\xc6\x92",     0.560 },  // ƒ
        { "CV",       0xf7e77d, "\xe2\x89\x88", 0.784 },  // ≈
    };
    const int i = (int) r;
    return table[i >= 0 && i < kRoleCount ? i : (int) Role::CV];
}

constexpr std::uint32_t argb (std::uint32_t rgb) noexcept { return 0xff000000u | rgb; }
inline std::uint32_t roleArgb (Role r) noexcept { return argb (roleInfo (r).rgb); }

inline bool roleFromName (const char* name, Role& out) noexcept
{
    for (int i = 0; i < kRoleCount; ++i)
        if (std::strcmp (roleInfo ((Role) i).name, name) == 0)
        {
            out = (Role) i;
            return true;
        }
    return false;
}

enum class Badge { None, PitchLaw, AudioIntoClock, GateToStrig };

inline Badge cableBadge (Role source, Role dest) noexcept
{
    if ((source == Role::VOct && dest == Role::HzvLin) || (source == Role::HzvLin && dest == Role::VOct))
        return Badge::PitchLaw;                       // JCS R4.3: a warning, not a refusal
    if (source == Role::Audio && dest == Role::GateClk)
        return Badge::AudioIntoClock;                 // JIDAI_RACK_Redesign 3.9
    if (source == Role::GateClk && dest == Role::STrig)
        return Badge::GateToStrig;                    // JCS R3s, converted per cable
    return Badge::None;
}

inline const char* badgeText (Badge b) noexcept
{
    switch (b)
    {
        case Badge::PitchLaw: return "\xe2\x89\xa0 linear Hz/V into V/OCT: not octaves";
        case Badge::AudioIntoClock: return "\xe2\x89\xa0 audio into a clock input";
        case Badge::GateToStrig: return "\xe2\x8a\x93\xe2\x86\x92\xe2\x8a\x94 gate converted to S-trig";
        case Badge::None: break;
    }
    return "";
}

} // namespace jidai::jcs
