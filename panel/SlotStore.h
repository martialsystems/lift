// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include <juce_core/juce_core.h>

namespace lift {

// 999 save slots (001..999) as files in one folder:
//   slot-001.lift ... slot-999.lift   one complete saved state each
//   last-slot.txt                     the slot saved or loaded last
// Every write goes to a temporary file in the same folder that is then
// renamed over the target, so a crash mid-save never leaves a half slot.
class SlotStore {
public:
    static constexpr int kFirst = 1;
    static constexpr int kLast = 999;

    // <user app data>/Martial Systems/LIFT. On macOS that is
    // ~/Library/Application Support/Martial Systems/LIFT, on Windows
    // %APPDATA%\Martial Systems\LIFT, on Linux ~/.config/Martial Systems/LIFT.
    static juce::File defaultFolder();

    explicit SlotStore(juce::File folder = defaultFolder()) : folder_(std::move(folder)) {}

    static bool valid(int slot) noexcept { return slot >= kFirst && slot <= kLast; }
    const juce::File& folder() const noexcept { return folder_; }
    juce::File slotFile(int slot) const;  // invalid slot -> empty File
    bool exists(int slot) const;
    bool write(int slot, const juce::MemoryBlock& data) const;
    bool read(int slot, juce::MemoryBlock& data) const;
    juce::Time modified(int slot) const;

    int lastSlot() const;  // 0 = none
    bool setLastSlot(int slot) const;

private:
    bool writeAtomic(const juce::File& target, const void* data, size_t size) const;
    juce::File folder_;
};

}  // namespace lift
