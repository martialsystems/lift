// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// Patch bay model. The panel owns the editable list on the message thread and
// publishes a fixed-size snapshot the audio thread can read without locks or
// allocation (seqlock). Routing the cords as audio/CV is not built yet.

namespace lift {

constexpr int kJacks = 16;
constexpr int kMaxCords = 64;

struct Cord {
    int o = 0;       // output jack 0..15
    int i = 0;       // input jack 0..15
    int c = 0;       // cable colour index (cosmetic)
    bool st = false; // stackable plug
    bool operator==(const Cord&) const = default;
};

struct PatchSnapshot {
    int count = 0;
    Cord cords[kMaxCords];
};

class PatchBayModel {
public:
    // Message thread.
    void publish(const std::vector<Cord>& cords) noexcept {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        const int n = static_cast<int>(cords.size()) < kMaxCords ? static_cast<int>(cords.size()) : kMaxCords;
        for (int k = 0; k < n; ++k) {
            snap_.cords[k] = cords[static_cast<size_t>(k)];
        }
        snap_.count = n;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    // Any thread, including audio. Returns false if a write kept racing.
    bool read(PatchSnapshot& out) const noexcept {
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1u) {
                continue;
            }
            out = snap_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == a) {
                return true;
            }
        }
        return false;
    }

    // Convenience for the engine: is output o patched into input i?
    static bool connected(const PatchSnapshot& s, int o, int i) noexcept {
        for (int k = 0; k < s.count; ++k) {
            if (s.cords[k].o == o && s.cords[k].i == i) {
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint32_t> seq_{0};
    PatchSnapshot snap_;
};

}  // namespace lift
