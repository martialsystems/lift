// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#if defined(LIFT_RADIO) && LIFT_RADIO
#error "VST3 build has no radio"
#endif

int lift_vst3_entry() {
    return 0;
}
