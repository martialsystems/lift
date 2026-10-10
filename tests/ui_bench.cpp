// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// UI frame-time bench: the real editor in a real window (Xvfb on Linux) at a
// 2x backing scale. Each frame injects the scenario's input, steps the panel's
// frame clock and then flushes the window's pending repaints synchronously,
// timing all of it. Run: lift_panel_check --ui-bench [scale]

#include "LiftPanel.h"
#include "LiftProcessor.h"
#include "PanelData.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

struct Stats {
    std::vector<double> ms;
    void add(double v) { ms.push_back(v); }
    void print(const char* name) const {
        auto s = ms;
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (double v : s) {
            sum += v;
        }
        const double avg = sum / static_cast<double>(juce::jmax<size_t>(1, s.size()));
        const double p95 = s.empty() ? 0.0 : s[static_cast<size_t>(0.95 * static_cast<double>(s.size() - 1))];
        const double mx = s.empty() ? 0.0 : s.back();
        int over = 0;
        for (double v : s) {
            over += v > 16.7 ? 1 : 0;
        }
        std::printf("  %-26s avg %8.2f ms  p95 %8.2f ms  max %8.2f ms  frames over 16.7 ms: %d/%zu\n", name, avg, p95, mx,
                    over, s.size());
    }
};

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> p, juce::Point<float> down, bool dragged, bool button) {
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto mods = button ? juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys();
    return juce::MouseEvent(src, p, mods, 1.f, 0.f, 0.f, 0.f, 0.f, &c, &c, juce::Time::getCurrentTime(), down,
                            juce::Time::getCurrentTime(), 1, dragged);
}

}  // namespace

int runUiBench(float backingScale) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::Desktop::getInstance().setGlobalScaleFactor(backingScale);
    lift::LiftProcessor proc;
    proc.prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc.createEditor());
    // 100 %: the prototype's native size
    ed->setSize(lift::LiftPanel::kW, lift::LiftPanel::kH);
    ed->addToDesktop(0);
    ed->setVisible(true);
    auto* panel = dynamic_cast<lift::LiftPanel*>(ed->getChildComponent(0));
    auto* peer = ed->getPeer();
    if (panel == nullptr || peer == nullptr) {
        std::printf("ui bench: no panel or no window\n");
        return 1;
    }
    // panel-local position of a canvas point (the panel may be drawn at a scale)
    auto at = [&](float cx, float cy) {
        const float s = static_cast<float>(panel->getWidth()) / static_cast<float>(lift::LiftPanel::kW);
        return juce::Point<float>(cx * s, cy * s);
    };
    auto audio = [&](double secs) {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        for (int n = 0; n < static_cast<int>(secs * 48000.0 / 512.0) + 1; ++n) {
            midi.clear();
            proc.processBlock(buf, midi);
        }
    };
    juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
    std::printf("ui bench: window %d x %d, backing scale %.1f\n", ed->getWidth(), ed->getHeight(), backingScale);

    auto run = [&](const char* name, int frames, const std::function<void(int)>& input) {
        Stats st;
        for (int f = 0; f < frames; ++f) {
            audio(1.0 / 60.0);
            const double p0 = lift::paintStatsMs().load();
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            input(f);
            const double inMs = juce::Time::getMillisecondCounterHiRes() - t0;
            // one display frame: the app's own timers / vblank callbacks run and paint
            juce::MessageManager::getInstance()->runDispatchLoopUntil(16);
            st.add(inMs + (lift::paintStatsMs().load() - p0));
        }
        st.print(name);
    };
    run("idle (stopped)", 60, [](int) {});
    panel->act("play");
    run("idle (tape playing)", 60, [](int) {});
    panel->act("stop");
    run("settle", 20, [](int) {});

    // cable drag: pick up the cable on OUT 1 and drag it around the bay
    const auto j = at(lift::ui::kDevX + lift::ui::jx(0), lift::ui::kDevY + lift::ui::jy('o'));
    panel->mouseDown(mouse(*panel, j, j, false, true));
    run("cable drag", 40, [&](int f) {
        const float a = static_cast<float>(f) * 0.07f;
        const auto p = at(lift::ui::kDevX + 600.f + 420.f * std::cos(a), lift::ui::kDevY + 250.f + 160.f * std::sin(a * 1.3f));
        panel->mouseDrag(mouse(*panel, p, j, true, true));
    });
    panel->mouseUp(mouse(*panel, j, j, false, false));
    run("settle", 20, [](int) {});

    // button presses: a mode pad every 12 frames, the screen changes view
    run("pad press + screen update", 48, [&](int f) {
        if (f % 12 == 0) {
            const int pad = (f / 12) % 2 == 0 ? 0 : 2;  // SYNTH / TAPE
            const auto r = lift::ui::padRect(pad);
            const auto p = at(lift::ui::kDevX + r.getCentreX(), lift::ui::kDevY + r.getCentreY());
            panel->mouseDown(mouse(*panel, p, p, false, true));
            panel->mouseUp(mouse(*panel, p, p, false, false));
        }
    });

    // knob drag (SPEED)
    const auto kc = lift::ui::knobCentre(0);
    const auto k0 = at(lift::ui::kDevX + kc.x, lift::ui::kDevY + kc.y);
    panel->mouseDown(mouse(*panel, k0, k0, false, true));
    run("knob drag", 30, [&](int f) {
        const auto p = k0.translated(0.f, -60.f * std::sin(static_cast<float>(f) * 0.08f));
        panel->mouseDrag(mouse(*panel, p, k0, true, true));
    });
    panel->mouseUp(mouse(*panel, k0, k0, false, false));
    ed->removeFromDesktop();
    return 0;
}
