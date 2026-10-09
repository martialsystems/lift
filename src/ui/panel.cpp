// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "ui/panel.h"

#include "names/catalog.h"
#include "pool/pool.h"

#include <cstdio>
#include <cstring>

void panel_ui_init(PanelUi& ui) {
    std::memset(&ui, 0, sizeof ui);
}

int panel_ui_load_pool(PanelUi& ui, const char* project) {
    if (project == nullptr) {
        return -1;
    }
    PoolItem items[kPanelKeys];
    const int count = pool_load(project, items, kPanelKeys);
    if (count < 0) {
        return -1;
    }
    for (int i = 0; i < kPanelKeys; ++i) {
        ui.keys[i] = {};
    }
    const int n = count < kPanelKeys ? count : kPanelKeys;
    for (int i = 0; i < n; ++i) {
        std::snprintf(ui.keys[i].name, sizeof ui.keys[i].name, "%s", items[i].name);
        std::snprintf(ui.keys[i].hash, sizeof ui.keys[i].hash, "%s", items[i].hash);
        ui.keys[i].frames = items[i].frames;
        ui.keys[i].used = 1;
    }
    return n;
}

void panel_ui_click(PanelUi& ui, PanelHit hit) {
    PanelCmd cmd{};
    bool push = true;
    switch (hit.kind) {
    case PanelHitKind::Track:
        cmd.act = PanelAct::Arm;
        cmd.track = hit.index;
        break;
    case PanelHitKind::Lift:
        cmd.act = PanelAct::Lift;
        break;
    case PanelHitKind::Drop:
        cmd.act = PanelAct::Drop;
        cmd.flag = ui.overdub ? 1 : 0;
        break;
    case PanelHitKind::Rec:
        cmd.act = PanelAct::Print;
        cmd.flag = 1;
        break;
    case PanelHitKind::Play:
        cmd.act = PanelAct::Play;
        break;
    case PanelHitKind::Stop:
        cmd.act = PanelAct::Stop;
        break;
    case PanelHitKind::Rev:
        cmd.act = PanelAct::Rev;
        break;
    case PanelHitKind::Over:
        ui.overdub = ui.overdub ? 0 : 1;
        push = false;
        break;
    case PanelHitKind::Key:
        if (hit.index >= 0 && hit.index < kPanelKeys) {
            ui.selected = hit.index;
        }
        push = false;
        break;
    case PanelHitKind::None:
        push = false;
        break;
    }
    if (push) {
        panel_audio_push(cmd);
    }
}

PanelDrawIn panel_compose(const PanelUi& ui, const PanelMeters& meters, double seconds) {
    PanelDrawIn in{};
    in.baseColor = ui.baseColor;
    in.playing = meters.playing;
    in.recording = meters.recording;
    in.reverse = meters.reverse;
    in.overdub = ui.overdub;
    in.arm = meters.arm;
    in.pos = meters.pos;
    in.seconds = seconds;
    in.frames = meters.frames;
    for (int t = 0; t < 4; ++t) {
        in.peak[t] = meters.peak[t];
    }
    for (int i = 0; i < kPanelKeys; ++i) {
        in.keyUsed[i] = ui.keys[i].used;
        std::snprintf(in.keyName[i], sizeof in.keyName[i], "%s", ui.keys[i].name);
    }
    in.character = catalog_character(meters.character);
    return in;
}
