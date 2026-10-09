// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "radio/client.h"

int lift_standalone_entry() {
    RadioRequest request{};
    request.url = "";
    return lift_radio_tune(&request);
}
