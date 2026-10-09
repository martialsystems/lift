// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "ui/audio.h"
#include "ui/draw.h"

struct PanelKey {
    char name[32];
    char hash[65];
    int frames;
    int used;
};

struct PanelUi {
    PanelKey keys[kPanelKeys];
    int overdub;
    int baseColor;
    int selected;
};

void panel_ui_init(PanelUi& ui);
int panel_ui_load_pool(PanelUi& ui, const char* project);
void panel_ui_click(PanelUi& ui, PanelHit hit);
PanelDrawIn panel_compose(const PanelUi& ui, const PanelMeters& meters, double seconds);
