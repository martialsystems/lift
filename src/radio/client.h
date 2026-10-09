// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Stream client. Tune, read, and capture into the session.

struct RadioRequest {
    const char* url;
};

int lift_radio_tune(const RadioRequest* request);
int lift_radio_read(float* dst, int n);
int lift_radio_capture(float* dst, int n);
