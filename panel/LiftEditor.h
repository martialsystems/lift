// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "LiftPanel.h"
#include "WindowSizes.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace lift {

// Hosts the panel at one of the fixed sizes (WindowSizes): the panel is laid
// out at that size and draws itself at that scale (no component transform,
// so its caches are made at the real device resolution). Right-click on the
// panel offers the sizes; the standalone also has them in its Options menu.
class LiftEditor : public juce::AudioProcessorEditor {
public:
    explicit LiftEditor(LiftProcessor& p) : juce::AudioProcessorEditor(p), panel_(p) {
        addAndMakeVisible(panel_);
        setResizable(false, false);
        panel_.addSizeItems = [this](juce::PopupMenu& m) { addSizeItems(m); };
        setSizeIndex(WindowSizes::load(LiftPanel::kW, LiftPanel::kH), false);
    }

    int sizeIndex() const { return sizeIdx_; }
    void setSizeIndex(int idx, bool remember) {
        sizeIdx_ = juce::jlimit(0, WindowSizes::kCount - 1, idx);
        const auto s = WindowSizes::size(sizeIdx_, LiftPanel::kW, LiftPanel::kH);
        setSize(s.x, s.y);
        if (remember) {
            WindowSizes::save(sizeIdx_);
        }
    }
    void addSizeItems(juce::PopupMenu& m) {
        juce::Component::SafePointer<LiftEditor> self(this);
        for (int i = 0; i < WindowSizes::kCount; ++i) {
            const auto s = WindowSizes::size(i, LiftPanel::kW, LiftPanel::kH);
            m.addItem("Window size " + juce::String(WindowSizes::kPercent[i]) + " %  (" + juce::String(s.x) + " x " +
                          juce::String(s.y) + ")",
                      true, i == sizeIdx_, [self, i] {
                          if (self != nullptr) {
                              self->setSizeIndex(i, true);
                          }
                      });
        }
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xffc2bdb3)); }
    void resized() override { panel_.setBounds(getLocalBounds()); }

    LiftPanel& panel() { return panel_; }

private:
    LiftPanel panel_;
    int sizeIdx_ = WindowSizes::kDefault;
};

}  // namespace lift
