// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/prepare.h"
#include "pool/pool.h"
#include "ui/panel.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_fails = 0;

void check(bool ok, const char* file, int line, const char* text) {
    if (!ok) {
        std::cerr << "FAIL " << file << ":" << line << " " << text << "\n";
        ++g_fails;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), __FILE__, __LINE__, #cond)

Rgb at(const std::vector<uint8_t>& px, int x, int y) {
    const uint8_t* p = px.data() + (static_cast<size_t>(y * kPanelW + x) * 4u);
    return Rgb{p[0], p[1], p[2]};
}

bool eq(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

void le16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 255u));
    b.push_back(static_cast<unsigned char>((v >> 8) & 255u));
}

void le32(std::vector<unsigned char>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 255u));
    }
}

void write_wav(const std::filesystem::path& path) {
    std::vector<unsigned char> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    le32(b, 36u + 8u * 2u);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le32(b, 16);
    le16(b, 1);
    le16(b, 2);
    le32(b, 48000);
    le32(b, 48000u * 4u);
    le16(b, 4);
    le16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    le32(b, 8);
    for (int i = 0; i < 2; ++i) {
        le16(b, 16000);
        le16(b, static_cast<uint16_t>(-8000));
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

PanelDrawIn blank() {
    PanelUi ui;
    panel_ui_init(ui);
    PanelMeters meters{};
    meters.frames = 1000;
    meters.pos = 500.0;
    return panel_compose(ui, meters, 0.0);
}

std::vector<uint8_t> draw_of(PanelDrawIn in) {
    std::vector<uint8_t> px(static_cast<size_t>(kPanelW * kPanelH * 4));
    panel_draw(in, px.data(), kPanelW, kPanelH);
    return px;
}

void test_face_and_panel() {
    CHECK(std::fabs(panel_ghost_degrees(0.5, 0)) < 1e-6);
    CHECK(std::fabs(panel_ghost_degrees(0.0, 1)) < 1e-4);
    CHECK(std::fabs(panel_ghost_degrees(1.0, 1)) < 1e-4);
    CHECK(std::fabs(panel_ghost_degrees(0.5, 1) - kGhostTurn) < 1e-3);
    CHECK(std::fabs(panel_ghost_degrees(1.5, 1) - kGhostTurn) < 1e-3);

    PanelLayout layout{};
    panel_layout(layout);
    const int cx = layout.mascot.x + layout.mascot.w / 2;
    const int cy = layout.mascot.y + layout.mascot.h / 2;
    const int slotX = cx - 15;
    const int slotY = cy - 14;
    const int gapX = cx;
    const int gapY = cy - 14;

    PanelDrawIn base = blank();
    const std::vector<uint8_t> magenta = draw_of(base);
    CHECK(eq(at(magenta, cx, cy), kFaceMagenta));
    CHECK(eq(at(magenta, slotX, slotY), kFaceSlot));
    CHECK(eq(at(magenta, gapX, gapY), kFaceMagenta));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            CHECK(eq(at(magenta, slotX + dx, slotY + dy), kFaceSlot));
        }
    }

    base.baseColor = 1;
    const std::vector<uint8_t> lime = draw_of(base);
    base.baseColor = 2;
    const std::vector<uint8_t> cyan = draw_of(base);
    base.baseColor = 0;
    base.recording = 1;
    const std::vector<uint8_t> violet = draw_of(base);
    int swapped = 0;
    for (int y = layout.mascot.y; y < layout.mascot.y + layout.mascot.h; ++y) {
        for (int x = layout.mascot.x; x < layout.mascot.x + layout.mascot.w; ++x) {
            const Rgb m = at(magenta, x, y);
            const Rgb l = at(lime, x, y);
            const Rgb c = at(cyan, x, y);
            const Rgb v = at(violet, x, y);
            if (eq(m, kFaceMagenta)) {
                CHECK(eq(l, kFaceLime));
                CHECK(eq(c, kFaceCyan));
                CHECK(eq(v, kFaceViolet));
                ++swapped;
            } else if (eq(m, kFaceSlot)) {
                CHECK(eq(l, kFaceSlot));
                CHECK(eq(c, kFaceSlot));
                CHECK(eq(v, kFaceSlot));
            } else {
                CHECK(eq(l, m));
                CHECK(eq(c, m));
                CHECK(eq(v, m));
            }
        }
    }
    CHECK(swapped > 1000);

    base = blank();
    base.playing = 1;
    base.seconds = 0.5;
    const std::vector<uint8_t> play = draw_of(base);
    CHECK(eq(at(play, cx, cy), kFaceMagenta));
    CHECK(eq(at(play, slotX, slotY), kFaceSlot));
    int ghost = 0;
    for (int y = layout.mascot.y; y < layout.mascot.y + layout.mascot.h; ++y) {
        for (int x = layout.mascot.x; x < layout.mascot.x + layout.mascot.w; ++x) {
            const Rgb was = at(magenta, x, y);
            const Rgb now = at(play, x, y);
            if (eq(was, now)) {
                continue;
            }
            const Rgb expect{face_mix(was.r, kFaceMagenta.r), face_mix(was.g, kFaceMagenta.g),
                             face_mix(was.b, kFaceMagenta.b)};
            CHECK(eq(now, expect));
            ++ghost;
        }
    }
    CHECK(ghost > 20);

    base.recording = 1;
    const std::vector<uint8_t> recPlay = draw_of(base);
    CHECK(eq(at(recPlay, cx, cy), kFaceViolet));
    CHECK(eq(at(recPlay, slotX, slotY), kFaceSlot));
    int sameGhost = 0;
    for (int y = layout.mascot.y; y < layout.mascot.y + layout.mascot.h; ++y) {
        for (int x = layout.mascot.x; x < layout.mascot.x + layout.mascot.w; ++x) {
            const Rgb was = at(magenta, x, y);
            const Rgb moved = at(play, x, y);
            if (eq(was, moved)) {
                continue;
            }
            const Rgb expect{face_mix(was.r, kFaceViolet.r), face_mix(was.g, kFaceViolet.g),
                             face_mix(was.b, kFaceViolet.b)};
            CHECK(eq(at(recPlay, x, y), expect));
            ++sameGhost;
        }
    }
    CHECK(sameGhost == ghost);

    base = blank();
    base.arm = 0;
    const std::vector<uint8_t> arm0 = draw_of(base);
    const int bar0x = layout.tracks[0].x + 2;
    const int bar0y = layout.tracks[0].y + layout.tracks[0].h / 2;
    const int bar1x = layout.tracks[1].x + 2;
    const int bar1y = layout.tracks[1].y + layout.tracks[1].h / 2;
    CHECK(eq(at(arm0, bar0x, bar0y), kFaceMagenta));
    CHECK(!eq(at(arm0, bar1x, bar1y), kFaceMagenta));
    base.arm = 1;
    const std::vector<uint8_t> arm1 = draw_of(base);
    CHECK(!eq(at(arm1, bar0x, bar0y), kFaceMagenta));
    CHECK(eq(at(arm1, bar1x, bar1y), kFaceMagenta));

    const int hx = panel_playhead_x(layout.tracks[0], 500.0, 1000);
    CHECK(eq(at(arm0, hx, layout.tracks[0].y + 8), Rgb{242, 242, 244}));

    PanelUi ui;
    panel_ui_init(ui);
    std::snprintf(ui.keys[0].name, sizeof ui.keys[0].name, "take.wav");
    ui.keys[0].used = 1;
    PanelMeters meters{};
    meters.frames = 1000;
    const std::vector<uint8_t> keyed = draw_of(panel_compose(ui, meters, 0.0));
    int mx = 0;
    int my = 0;
    panel_key_mark(layout.keys[0], mx, my);
    CHECK(eq(at(keyed, mx + 2, my + 2), kFaceMagenta));
    panel_key_mark(layout.keys[1], mx, my);
    CHECK(!eq(at(keyed, mx + 2, my + 2), kFaceMagenta));
    int ink = 0;
    const PanelRect& key = layout.keys[0];
    for (int y = key.y; y < key.y + key.h; ++y) {
        for (int x = key.x; x < key.x + key.w; ++x) {
            if (eq(at(keyed, x, y), Rgb{22, 22, 28})) {
                ++ink;
            }
        }
    }
    CHECK(ink > 10);

    const int tx = layout.tracks[1].x + layout.tracks[1].w / 2;
    const int ty = layout.tracks[1].y + layout.tracks[1].h / 2;
    const PanelHit track = panel_hit(tx, ty);
    CHECK(track.kind == PanelHitKind::Track);
    CHECK(track.index == 1);
    const PanelHit lift = panel_hit(layout.lift.x + 4, layout.lift.y + 4);
    CHECK(lift.kind == PanelHitKind::Lift);
    const PanelHit keyHit = panel_hit(layout.keys[0].x + 8, layout.keys[0].y + layout.keys[0].h - 8);
    CHECK(keyHit.kind == PanelHitKind::Key);
    CHECK(keyHit.index == 0);
    const PanelHit mode = panel_hit(layout.modes[0].x + 8, layout.modes[0].y + 8);
    CHECK(mode.kind == PanelHitKind::None);

    panel_write_ppm("/tmp/lift-panel-rest.ppm", magenta.data(), kPanelW, kPanelH);
    base = blank();
    base.playing = 1;
    base.seconds = 0.5;
    const std::vector<uint8_t> shotPlay = draw_of(base);
    panel_write_ppm("/tmp/lift-panel-play.ppm", shotPlay.data(), kPanelW, kPanelH);
    panel_write_ppm("/tmp/lift-panel-key.ppm", keyed.data(), kPanelW, kPanelH);
}

void test_transport_and_pool() {
    TapeRuntime rt;
    transport_init(rt);
    prepare_tracks(rt, 2048);
    panel_audio_bind(&rt);
    PanelUi ui;
    panel_ui_init(ui);

    panel_ui_click(ui, PanelHit{PanelHitKind::Track, 1});
    panel_ui_click(ui, PanelHit{PanelHitKind::Rec, 0});
    std::vector<float> in(600, 0.35f);
    std::vector<float> out(600, 0.f);
    panel_audio_block(in.data(), in.data(), out.data(), out.data(), 600);
    PanelMeters meters{};
    panel_audio_meters(meters);
    CHECK(meters.arm == 1);
    CHECK(meters.recording == 1);
    CHECK(meters.playing == 1);
    CHECK(rt.pos == 600.0);
    CHECK(std::fabs(rt.ch[1][0][10]) < 1e-6f || std::fabs(rt.ch[1][0][300]) > 0.05f);
    CHECK(std::fabs(rt.ch[1][0][300]) > 0.05f);
    CHECK(std::fabs(rt.ch[0][0][300]) < 1e-6f);

    panel_ui_click(ui, PanelHit{PanelHitKind::Stop, 0});
    panel_ui_click(ui, PanelHit{PanelHitKind::Lift, 0});
    PanelCmd cmd{};
    cmd.act = PanelAct::Seek;
    cmd.pos = 1000;
    panel_audio_push(cmd);
    CHECK(ui.overdub == 0);
    panel_ui_click(ui, PanelHit{PanelHitKind::Drop, 0});
    std::vector<float> quiet(16, 0.f);
    panel_audio_block(quiet.data(), quiet.data(), quiet.data(), quiet.data(), 16);
    CHECK(std::fabs(rt.ch[1][0][1300] - rt.ch[1][0][300]) < 1e-5f);
    panel_audio_meters(meters);
    CHECK(meters.recording == 0);
    CHECK(meters.playing == 0);

    const float mid = rt.ch[1][0][300];
    cmd = {};
    cmd.act = PanelAct::Seek;
    cmd.pos = 0;
    panel_audio_push(cmd);
    panel_ui_click(ui, PanelHit{PanelHitKind::Over, 0});
    CHECK(ui.overdub == 1);
    panel_ui_click(ui, PanelHit{PanelHitKind::Drop, 0});
    panel_audio_block(quiet.data(), quiet.data(), quiet.data(), quiet.data(), 16);
    CHECK(std::fabs(rt.ch[1][0][300] - (mid + mid)) < 1e-4f);

    panel_ui_click(ui, PanelHit{PanelHitKind::Rev, 0});
    panel_audio_block(nullptr, nullptr, nullptr, nullptr, 0);
    panel_audio_meters(meters);
    CHECK(meters.reverse == 1);
    panel_ui_click(ui, PanelHit{PanelHitKind::Stop, 0});
    panel_audio_block(nullptr, nullptr, nullptr, nullptr, 0);
    panel_audio_meters(meters);
    CHECK(meters.reverse == 0);
    panel_ui_click(ui, PanelHit{PanelHitKind::Key, 0});
    CHECK(ui.selected == 0);
    panel_ui_click(ui, PanelHit{PanelHitKind::None, 0});
    CHECK(meters.playing == 0);

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("lift-window-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "take.wav");
    char reason[128] = {};
    PoolItem item{};
    CHECK(pool_import(dir.string().c_str(), (dir / "take.wav").string().c_str(), &item, reason, 128) == PoolStatus::Ok);
    CHECK(panel_ui_load_pool(ui, dir.string().c_str()) == 1);
    CHECK(ui.keys[0].used == 1);
    CHECK(std::strcmp(ui.keys[0].name, "take.wav") == 0);
    CHECK(std::strcmp(ui.keys[0].hash, item.hash) == 0);
    CHECK(ui.keys[1].used == 0);
    const std::filesystem::path empty = dir / "empty";
    std::filesystem::create_directories(empty);
    PanelUi none;
    panel_ui_init(none);
    CHECK(panel_ui_load_pool(none, empty.string().c_str()) == 0);
    std::filesystem::remove_all(dir);

    release_tracks(rt);
}

}  // namespace

int run_window_suite(const char* only) {
    const bool all = only == nullptr || only[0] == '\0';
    if (!all && std::strcmp(only, "window") != 0) {
        return 0;
    }
    const int before = g_fails;
    test_face_and_panel();
    test_transport_and_pool();
    return g_fails - before;
}
