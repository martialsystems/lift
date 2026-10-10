// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "engine/patch.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// Patch bay model. The panel owns the editable list on the message thread and
// publishes a fixed-size snapshot the audio thread can read without locks or
// allocation (seqlock). The snapshot carries the routing plan (feedback
// cables, run order: eng::planPatch, computed here on the message thread),
// which the audio thread's patch graph (engine/patch.h) plays.

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
    std::uint8_t pins[256] = {};  // matrix: [row * 16 + column], 0 none, 1 +100 %, 2 +50 %, 3 -100 %
    eng::PatchPlan plan;
};

class PatchBayModel {
public:
    // Message thread.
    void publish(const std::vector<Cord>& cords, const std::array<std::uint8_t, 256>& pins, int arm) {
        pins_ = pins;
        arm_ = arm;
        publish(cords);
    }
    void publish(const std::vector<Cord>& cords) {
        const int n = static_cast<int>(cords.size()) < kMaxCords ? static_cast<int>(cords.size()) : kMaxCords;
        int os[kMaxCords], is[kMaxCords];
        for (int k = 0; k < n; ++k) {
            os[k] = cords[static_cast<size_t>(k)].o;
            is[k] = cords[static_cast<size_t>(k)].i;
        }
        plan_ = eng::planPatch(os, is, n, pins_.data(), arm_);  // allocates: before the write window
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int k = 0; k < n; ++k) {
            snap_.cords[k] = cords[static_cast<size_t>(k)];
        }
        snap_.count = n;
        for (int k = 0; k < 256; ++k) {
            snap_.pins[k] = pins_[static_cast<size_t>(k)];
        }
        snap_.plan = plan_;
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

    // Version: changes on every publish (the audio thread re-reads only then).
    uint32_t version() const noexcept { return seq_.load(std::memory_order_acquire); }
    // Message thread: the plan last published (the BAY screen's feedback marks).
    const eng::PatchPlan& plan() const noexcept { return plan_; }

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
    eng::PatchPlan plan_;  // message-thread copy
    std::array<std::uint8_t, 256> pins_{};
    int arm_ = 0;
};

}  // namespace lift
