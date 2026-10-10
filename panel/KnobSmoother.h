// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Knob smoothing on the audio thread: a one-pole toward the target with
// double state. Taken from RONIN (Source/Modular/Smoothing.h, KnobSmoother):
// the first target after prepare() is taken at once, so a fresh instance and a
// recalled state start on their values instead of ramping up from zero. LIFT
// adds advance(n), which steps n samples at once (control-rate updates every
// 32 samples; the steps are far below audibility).

#include <cmath>

namespace lift {

class KnobSmoother {
public:
    explicit KnobSmoother(float initial = 0.f, double seconds = 0.02) noexcept
        : value_(static_cast<double>(initial)), target_(initial), seconds_(seconds) {}

    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate > 1.0 ? sampleRate : 48000.0;
        coeff_ = -std::expm1(-1.0 / (seconds_ * rate_));
        value_ = static_cast<double>(target_);
        fresh_ = true;
    }
    void setTarget(float target) noexcept {
        target_ = target;
        if (fresh_) {
            value_ = static_cast<double>(target);
        }
    }
    void snap(float value) noexcept {
        target_ = value;
        value_ = static_cast<double>(value);
    }
    float next() noexcept {
        fresh_ = false;
        value_ += (static_cast<double>(target_) - value_) * coeff_;
        settle();
        return static_cast<float>(value_);
    }
    // n samples in one step (exact for a one-pole)
    float advance(int n) noexcept {
        fresh_ = false;
        const double a = -std::expm1(-static_cast<double>(n) / (seconds_ * rate_));
        value_ += (static_cast<double>(target_) - value_) * a;
        settle();
        return static_cast<float>(value_);
    }
    float target() const noexcept { return target_; }
    float current() const noexcept { return static_cast<float>(value_); }
    bool moving() const noexcept { return value_ != static_cast<double>(target_); }

private:
    void settle() noexcept {
        if (std::fabs(static_cast<double>(target_) - value_) < 1.0e-6) {
            value_ = static_cast<double>(target_);
        }
    }
    double value_;
    float target_;
    double seconds_;
    double rate_ = 48000.0;
    double coeff_ = 1.0;
    bool fresh_ = true;
};

}  // namespace lift
