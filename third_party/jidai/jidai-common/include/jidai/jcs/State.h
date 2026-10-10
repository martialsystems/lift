// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: versioned state and migration chains (JCS R7). Header-only C++17, no deps.
//
//   StateHeader { int format; std::string unit; }      every device state carries both
//   MigrationChain<State> chain (currentFormat);
//   chain.add (fromFormat, [] (State& s, std::vector<std::string>& notes) { ...; return true; });   vN -> vN+1
//   LoadResult r = chain.run (state, storedFormat);
//     r.status: Current | Migrated | FutureReadOnly | Failed
//     Unknown future versions load READ-ONLY with a warning instead of being rejected (R7).
//     A missing step (a gap in the chain) or a step returning false gives Failed and leaves `storedFormat` at the
//     last good version.

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jidai::jcs {

struct StateHeader
{
    int format = 0;
    std::string unit;
};

enum class LoadStatus { Current, Migrated, FutureReadOnly, Failed };

struct LoadResult
{
    LoadStatus status = LoadStatus::Current;
    int fromFormat = 0;
    int toFormat = 0;
    std::vector<std::string> notes;
    bool ok() const { return status != LoadStatus::Failed; }
    bool readOnly() const { return status == LoadStatus::FutureReadOnly; }
};

template <class State>
class MigrationChain
{
public:
    using Step = std::function<bool (State&, std::vector<std::string>& notes)>;

    explicit MigrationChain (int currentFormat) : current_ (currentFormat) {}
    int current() const { return current_; }

    MigrationChain& add (int fromFormat, Step step)
    {
        steps_[fromFormat] = std::move (step);
        return *this;
    }

    LoadResult run (State& state, int& format) const
    {
        LoadResult r;
        r.fromFormat = format;
        if (format > current_)
        {
            r.status = LoadStatus::FutureReadOnly;
            r.toFormat = format;
            r.notes.push_back ("saved by a newer version (format " + std::to_string (format) + "): loaded read-only");
            return r;
        }
        while (format < current_)
        {
            auto it = steps_.find (format);
            if (it == steps_.end() || ! it->second (state, r.notes))
            {
                r.status = LoadStatus::Failed;
                r.toFormat = format;
                r.notes.push_back ("no migration from format " + std::to_string (format));
                return r;
            }
            ++format;
            r.status = LoadStatus::Migrated;
        }
        r.toFormat = format;
        return r;
    }

private:
    int current_;
    std::map<int, Step> steps_;
};

} // namespace jidai::jcs
