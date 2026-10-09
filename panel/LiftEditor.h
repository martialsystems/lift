// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "LiftPanel.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace lift {

// Hosts the panel and scales it to the window (the prototype scales the face
// to the page width; here it fits the window, keeping the aspect ratio).
class LiftEditor : public juce::AudioProcessorEditor {
public:
    explicit LiftEditor(LiftProcessor& p) : juce::AudioProcessorEditor(p), panel_(p) {
        addAndMakeVisible(panel_);
        setResizable(true, true);
        setResizeLimits(LiftPanel::kW / 4, LiftPanel::kH / 4, LiftPanel::kW * 2, LiftPanel::kH * 2);
        if (auto* c = getConstrainer()) {
            c->setFixedAspectRatio(static_cast<double>(LiftPanel::kW) / LiftPanel::kH);
        }
        setSize(LiftPanel::kW * 4 / 5, LiftPanel::kH * 4 / 5);
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xffc2bdb3)); }

    void resized() override {
        const float s = juce::jmin(static_cast<float>(getWidth()) / LiftPanel::kW,
                                   static_cast<float>(getHeight()) / LiftPanel::kH);
        const float ox = (static_cast<float>(getWidth()) - LiftPanel::kW * s) * 0.5f;
        const float oy = (static_cast<float>(getHeight()) - LiftPanel::kH * s) * 0.5f;
        panel_.setBounds(0, 0, LiftPanel::kW, LiftPanel::kH);
        panel_.setTransform(juce::AffineTransform::scale(s).translated(ox, oy));
    }

private:
    LiftPanel panel_;
};

}  // namespace lift
