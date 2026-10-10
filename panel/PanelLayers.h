// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// The panel's two child layers (see PanelRender.cpp): the display and the
// cable overlay. Neither takes the mouse; the panel handles all input.

#pragma once

#include "LiftPanel.h"
#include "LiftProcessor.h"

namespace lift {

class LiftPanel::Screen : public juce::Component {
public:
    explicit Screen(LiftPanel& p) : panel_(p) {
        setOpaque(true);
        setInterceptsMouseClicks(false, false);
    }
    void paint(juce::Graphics& g) override {
        ScopedPaintStats st;
        panel_.paintScreenLayer(g);
    }

private:
    LiftPanel& panel_;
};

class LiftPanel::Cables : public juce::Component {
public:
    explicit Cables(LiftPanel& p) : panel_(p) { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override {
        ScopedPaintStats st;
        panel_.paintCableLayer(g);
    }

private:
    LiftPanel& panel_;
};

}  // namespace lift
