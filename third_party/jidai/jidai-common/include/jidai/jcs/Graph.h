// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// Jidai Cable Standard v1.1: graph rules, framework-free (JCS R9 feedback, R11 path latency). Header-only C++17.
// Nodes are whatever the caller schedules (modules, units, devices), numbered 0..n-1.
//
//   classifyFeedback (n, fixedEdges, cablesOldestFirst) -> std::vector<char>   (R9)
//       Walk cables oldest -> newest. A cable is feedback when its destination already reaches its source over
//       the edges kept so far (fixed edges count as kept from the start), or when it is a self-patch.
//       EVERY feedback cable is delayed exactly one sample; all others are zero-delay.
//   runOrder (n, fixedEdges, cables, feedback) -> std::vector<int>             (R9.4)
//       One topological order over fixed + non-feedback edges. Each node appears exactly once, so no node ever
//       runs twice per sample.
//   pathLatency (latency, audioEdges) -> std::vector<int>                       (R11.4)
//       P(d) = L_d + max over audio edges a -> d of P(a), over non-feedback edges; P = L with no audio inputs.
//   arrivalSkew (P, audioEdges) -> std::vector<int>                             (R11.6)
//       per edge: how many samples earlier it arrives than the latest audio input of the same destination
//       (the delta-n badge goes on edges with skew > 0). The rack never inserts hidden delays into cables.
// Fixed edges are in-device ordering constraints (signal-free or internal); they are never classified feedback.

#include <algorithm>
#include <vector>

namespace jidai::jcs {

struct GraphEdge
{
    int from = -1;
    int to = -1;
};

namespace detail {
inline bool reaches (const std::vector<std::vector<int>>& next, int from, int target)
{
    if (from == target)
        return true;
    std::vector<char> seen (next.size(), 0);
    std::vector<int> queue { from };
    seen[(size_t) from] = 1;
    for (size_t head = 0; head < queue.size(); ++head)
        for (int nx : next[(size_t) queue[head]])
        {
            if (nx == target)
                return true;
            if (! seen[(size_t) nx])
            {
                seen[(size_t) nx] = 1;
                queue.push_back (nx);
            }
        }
    return false;
}
inline bool inRange (int n, const GraphEdge& e) { return e.from >= 0 && e.from < n && e.to >= 0 && e.to < n; }
}

inline std::vector<char> classifyFeedback (int n, const std::vector<GraphEdge>& fixedEdges, const std::vector<GraphEdge>& cablesOldestFirst)
{
    std::vector<std::vector<int>> kept ((size_t) (n > 0 ? n : 0));
    for (auto& e : fixedEdges)
        if (detail::inRange (n, e) && e.from != e.to)
            kept[(size_t) e.from].push_back (e.to);
    std::vector<char> feedback (cablesOldestFirst.size(), 0);
    for (size_t i = 0; i < cablesOldestFirst.size(); ++i)
    {
        const auto& e = cablesOldestFirst[i];
        if (! detail::inRange (n, e))
            continue;
        if (e.from == e.to || detail::reaches (kept, e.to, e.from))
            feedback[i] = 1;
        else
            kept[(size_t) e.from].push_back (e.to);
    }
    return feedback;
}

inline std::vector<int> runOrder (int n, const std::vector<GraphEdge>& fixedEdges, const std::vector<GraphEdge>& cables,
                                  const std::vector<char>& feedback)
{
    std::vector<std::vector<int>> adj ((size_t) (n > 0 ? n : 0));
    std::vector<int> indeg ((size_t) (n > 0 ? n : 0), 0);
    auto addEdge = [&] (const GraphEdge& e)
    {
        if (! detail::inRange (n, e) || e.from == e.to)
            return;
        auto& l = adj[(size_t) e.from];
        if (std::find (l.begin(), l.end(), e.to) != l.end())
            return;
        l.push_back (e.to);
        ++indeg[(size_t) e.to];
    };
    for (auto& e : fixedEdges)
        addEdge (e);
    for (size_t i = 0; i < cables.size(); ++i)
        if (i >= feedback.size() || ! feedback[i])
            addEdge (cables[i]);
    std::vector<int> order;
    std::vector<int> ready;
    for (int u = 0; u < n; ++u)
        if (indeg[(size_t) u] == 0)
            ready.push_back (u);
    // Lowest index first, so the order is deterministic and follows rack position when there is a free choice.
    while (! ready.empty())
    {
        auto it = std::min_element (ready.begin(), ready.end());
        const int u = *it;
        ready.erase (it);
        order.push_back (u);
        for (int d : adj[(size_t) u])
            if (--indeg[(size_t) d] == 0)
                ready.push_back (d);
    }
    if ((int) order.size() < n)   // only possible if fixed edges form a cycle (a device bug): still run each once
    {
        std::vector<char> placed ((size_t) n, 0);
        for (int u : order) placed[(size_t) u] = 1;
        for (int u = 0; u < n; ++u)
            if (! placed[(size_t) u]) order.push_back (u);
    }
    return order;
}

inline std::vector<int> pathLatency (const std::vector<int>& latency, const std::vector<GraphEdge>& audioEdges)
{
    const int n = (int) latency.size();
    std::vector<char> fb = classifyFeedback (n, {}, audioEdges);   // ignore any loop edge (R11.4)
    std::vector<GraphEdge> forward;
    for (size_t i = 0; i < audioEdges.size(); ++i)
        if (! fb[i] && detail::inRange (n, audioEdges[i]))
            forward.push_back (audioEdges[i]);
    const auto order = runOrder (n, {}, forward, std::vector<char> (forward.size(), 0));
    std::vector<int> P (latency);
    std::vector<int> bestIn ((size_t) n, 0);
    std::vector<std::vector<int>> incoming ((size_t) n);
    for (auto& e : forward)
        incoming[(size_t) e.to].push_back (e.from);
    for (int u : order)
    {
        int m = 0;
        for (int a : incoming[(size_t) u])
            m = std::max (m, P[(size_t) a]);
        P[(size_t) u] = latency[(size_t) u] + m;
    }
    return P;
}

inline std::vector<int> arrivalSkew (const std::vector<int>& P, const std::vector<GraphEdge>& audioEdges)
{
    const int n = (int) P.size();
    std::vector<int> latest ((size_t) n, 0);
    for (auto& e : audioEdges)
        if (detail::inRange (n, e) && e.from != e.to)
            latest[(size_t) e.to] = std::max (latest[(size_t) e.to], P[(size_t) e.from]);
    std::vector<int> skew (audioEdges.size(), 0);
    for (size_t i = 0; i < audioEdges.size(); ++i)
        if (detail::inRange (n, audioEdges[i]) && audioEdges[i].from != audioEdges[i].to)
            skew[i] = latest[(size_t) audioEdges[i].to] - P[(size_t) audioEdges[i].from];
    return skew;
}

} // namespace jidai::jcs
