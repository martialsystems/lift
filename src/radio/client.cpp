// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "radio/client.h"

namespace {

constexpr int kNotReady = 1;

}

int lift_radio_tune(const RadioRequest* request) {
    if (request == nullptr || request->url == nullptr || request->url[0] == '\0') {
        return kNotReady;
    }
    return kNotReady;
}

int lift_radio_read(float* dst, int n) {
    if (dst == nullptr || n <= 0) {
        return kNotReady;
    }
    for (int i = 0; i < n; ++i) {
        dst[i] = 0.f;
    }
    return kNotReady;
}

int lift_radio_capture(float* dst, int n) {
    return lift_radio_read(dst, n);
}
