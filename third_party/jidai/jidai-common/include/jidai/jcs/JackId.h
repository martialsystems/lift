// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: canonical jack ids and alias tables (JCS R6). Header-only C++17, no deps.
//
//   Global form   PREFIX#N/SECTION:LABEL     e.g. "RONIN#1/VCO:HZ/V", "ORIGAMI#2/VC:VC 1"
//   File forms    PREFIX/SECTION:LABEL       first instance of that kind (kept rebinding)
//                 SECTION:LABEL              bare, binds to the device that saved it
//   parseJackId (text) -> std::optional<JackId>      JackId::global(), JackId::local()
//   formatJackId (prefix, n, section, label)
//   isKnownPrefix ("SHOGUN")    BUSHIDO, RONIN, SHOGUN, ORIGAMI, RACK
//   AliasTable                  per-device {old local id -> canonical local id + input law (AliasConversion)},
//                               applied on load before binding; refuses to reuse an id for a different jack.
//                               resolve (id) -> {canonical id, conversion}; conversion.convert (volts)
//
// Character rules (as enforced here): PREFIX is A-Z, 0-9, '-' or '_', starting with a letter. SECTION and LABEL
// are non-empty, contain no lowercase ASCII letters, no '#' and no ':'. SECTION has no '/'. LABEL MAY contain
// '/' (canonical ids such as VCO:HZ/V and INPUTS:START/STOP already do) and may contain non-ASCII UTF-8 (RONIN's
// "EG 2:OUT \u2212"). The split is at the FIRST '/' before the first ':'. This relaxes the "no '/'" wording of R6 to
// match the ids that R6 itself lists as canonical.

#include "Pitch.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jidai::jcs {

enum class JackForm { Global, FirstInstance, Bare };

struct JackId
{
    std::string prefix;   // empty for Bare
    int number = 0;       // N >= 1 for Global, 0 otherwise
    std::string section;
    std::string label;
    JackForm form = JackForm::Bare;

    std::string local() const { return section + ":" + label; }
    std::string global() const { return prefix + "#" + std::to_string (number) + "/" + local(); }   // Global only
    std::string text() const
    {
        if (form == JackForm::Global) return global();
        if (form == JackForm::FirstInstance) return prefix + "/" + local();
        return local();
    }
};

inline constexpr const char* kKnownPrefixes[] = { "BUSHIDO", "RONIN", "SHOGUN", "ORIGAMI", "RACK" };

inline bool isKnownPrefix (std::string_view p)
{
    for (auto* k : kKnownPrefixes)
        if (p == k)
            return true;
    return false;
}

namespace detail {
inline bool validPrefix (std::string_view p)
{
    if (p.empty() || p[0] < 'A' || p[0] > 'Z')
        return false;
    for (char c : p)
        if (! ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}
inline bool validPart (std::string_view s, bool allowSlash)
{
    if (s.empty() || s.front() == ' ' || s.back() == ' ')
        return false;
    for (char c : s)
    {
        if (c >= 'a' && c <= 'z') return false;
        if (c == '#' || c == ':') return false;
        if (c == '/' && ! allowSlash) return false;
        if ((unsigned char) c < 0x20) return false;
    }
    return true;
}
}

inline bool isValidSection (std::string_view s) { return detail::validPart (s, false); }
inline bool isValidLabel (std::string_view s) { return detail::validPart (s, true); }
inline bool isValidLocalId (std::string_view s)
{
    const auto colon = s.find (':');
    return colon != std::string_view::npos && isValidSection (s.substr (0, colon)) && isValidLabel (s.substr (colon + 1));
}

inline std::optional<JackId> parseJackId (std::string_view s)
{
    JackId id;
    const auto colon = s.find (':');
    if (colon == std::string_view::npos)
        return std::nullopt;
    const auto slash = s.find ('/');
    std::string_view rest = s;
    if (slash != std::string_view::npos && slash < colon)
    {
        std::string_view head = s.substr (0, slash);
        rest = s.substr (slash + 1);
        const auto hash = head.find ('#');
        if (hash == std::string_view::npos)
        {
            id.form = JackForm::FirstInstance;
            id.prefix = std::string (head);
        }
        else
        {
            id.form = JackForm::Global;
            id.prefix = std::string (head.substr (0, hash));
            const auto num = head.substr (hash + 1);
            if (num.empty() || num.size() > 6 || num[0] == '0')
                return std::nullopt;
            int n = 0;
            for (char c : num)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;
                n = n * 10 + (c - '0');
            }
            id.number = n;
        }
        if (! detail::validPrefix (id.prefix))
            return std::nullopt;
    }
    const auto c2 = rest.find (':');
    id.section = std::string (rest.substr (0, c2));
    id.label = std::string (rest.substr (c2 + 1));
    if (! isValidSection (id.section) || ! isValidLabel (id.label))
        return std::nullopt;
    return id;
}

inline std::string formatJackId (std::string_view prefix, int number, std::string_view section, std::string_view label)
{
    return std::string (prefix) + "#" + std::to_string (number) + "/" + std::string (section) + ":" + std::string (label);
}

// Input law of an alias (R6 + R4 migrations): how the volts arriving on a cable bound to the OLD jack convert for the
// canonical jack. A unit declares its renames and their conversions in one place, its AliasTable.
//   Identity      V' = V
//   Lin55ToVoct   V' = log2(V) - 1.25  (old linear 1 V = 55 Hz input -> V/OCT, e.g. SHOGUN LEAD/BASS:HZ/V -> :NOTE);
//                 V floored at 1 mV (log2(max(V, 1e-3)) - 1.25 = -11.216 V at 0 V), exactly pitch::lin55ToVoct
//   HzvLinToVoct  V' = log2(V)         (HZ/V LIN, 1 V = C3 -> V/OCT, 0 V = C3); V <= 0 -> -5 V
//   VoctToHzvLin  V' = 2^V             (V/OCT -> HZ/V LIN)
//   Scale         V' = scale * V + offset
enum class AliasLaw { Identity, Lin55ToVoct, HzvLinToVoct, VoctToHzvLin, Scale };

struct AliasConversion
{
    AliasLaw law = AliasLaw::Identity;
    double scale = 1.0, offset = 0.0;      // AliasLaw::Scale only

    bool identity() const noexcept { return law == AliasLaw::Identity; }
    double convert (double v) const noexcept
    {
        switch (law)
        {
            case AliasLaw::Identity: return v;
            case AliasLaw::Lin55ToVoct: return pitch::lin55ToVoct (v);
            case AliasLaw::HzvLinToVoct: return v > 0.0 ? std::log2 (v) : -pitch::kRail;
            case AliasLaw::VoctToHzvLin: return std::exp2 (v);
            case AliasLaw::Scale: return scale * v + offset;
        }
        return v;
    }
    static AliasConversion linear (double scale, double offset = 0.0) noexcept { return { AliasLaw::Scale, scale, offset }; }
};

// Per-device alias table (R6): old local id -> canonical local id, each with its input law. One hop only (an old id is
// never a target and a target never an old id), and an id is never reused for a different jack.
class AliasTable
{
public:
    struct Resolved
    {
        std::string id;                   // canonical local id (the input id when no alias applies)
        AliasConversion conversion;       // identity when no alias applies
        bool aliased = false;
    };

    // False when `oldId` already maps elsewhere (or with a different law), when either id is malformed, or when the
    // ids would chain.
    bool add (const std::string& oldId, const std::string& canonical, AliasConversion conversion = {})
    {
        if (! isValidLocalId (oldId) || ! isValidLocalId (canonical) || oldId == canonical)
            return false;
        for (auto& e : map_)
        {
            if (e.oldId == oldId)
                return e.canonical == canonical && e.conversion.law == conversion.law
                       && sameValue (e.conversion.scale, conversion.scale) && sameValue (e.conversion.offset, conversion.offset);
            if (e.canonical == oldId) return false;
            if (e.oldId == canonical) return false;
        }
        map_.push_back ({ oldId, canonical, conversion });
        return true;
    }
    bool add (const std::string& oldId, const std::string& canonical, AliasLaw law) { return add (oldId, canonical, AliasConversion { law }); }

    // The canonical id and the conversion for a stored id; unknown ids pass through with the identity law.
    Resolved resolve (const std::string& id) const
    {
        for (auto& e : map_)
            if (e.oldId == id)
                return { e.canonical, e.conversion, true };
        return { id, {}, false };
    }
    std::string apply (const std::string& id) const { return resolve (id).id; }
    size_t size() const { return map_.size(); }

private:
    struct Entry { std::string oldId, canonical; AliasConversion conversion; };
    static bool sameValue (double a, double b) noexcept { return ! (a < b) && ! (b < a); }
    std::vector<Entry> map_;
};

} // namespace jidai::jcs
