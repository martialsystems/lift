// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "ui/draw.h"

#include <cstdio>
#include <cstring>

namespace {

struct Glyph {
    const char* row[7];
};

Glyph letter(char c) {
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    Glyph g{};
    switch (c) {
    case 'A': g = {{".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}}; break;
    case 'B': g = {{"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}}; break;
    case 'C': g = {{".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}}; break;
    case 'D': g = {{"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}}; break;
    case 'E': g = {{"#####", "#....", "#....", "####.", "#....", "#....", "#####"}}; break;
    case 'F': g = {{"#####", "#....", "#....", "####.", "#....", "#....", "#...."}}; break;
    case 'G': g = {{".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}}; break;
    case 'H': g = {{"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}}; break;
    case 'I': g = {{"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}}; break;
    case 'J': g = {{"..###", "...#.", "...#.", "...#.", "#..#.", "#..#.", ".##.."}}; break;
    case 'K': g = {{"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}}; break;
    case 'L': g = {{"#....", "#....", "#....", "#....", "#....", "#....", "#####"}}; break;
    case 'M': g = {{"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}}; break;
    case 'N': g = {{"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}}; break;
    case 'O': g = {{".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}; break;
    case 'P': g = {{"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}}; break;
    case 'Q': g = {{".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}}; break;
    case 'R': g = {{"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}}; break;
    case 'S': g = {{".####", "#....", "#....", ".###.", "....#", "....#", "####."}}; break;
    case 'T': g = {{"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}}; break;
    case 'U': g = {{"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}; break;
    case 'V': g = {{"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}}; break;
    case 'W': g = {{"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}}; break;
    case 'X': g = {{"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}}; break;
    case 'Y': g = {{"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}}; break;
    case 'Z': g = {{"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}}; break;
    case '0': g = {{".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}}; break;
    case '1': g = {{"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}}; break;
    case '2': g = {{".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}}; break;
    case '3': g = {{".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}}; break;
    case '4': g = {{"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}}; break;
    case '5': g = {{"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}}; break;
    case '6': g = {{".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}}; break;
    case '7': g = {{"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}}; break;
    case '8': g = {{".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}}; break;
    case '9': g = {{".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}}; break;
    case '.': g = {{".....", ".....", ".....", ".....", ".....", "..#..", "..#.."}}; break;
    case '-': g = {{".....", ".....", ".....", "#####", ".....", ".....", "....."}}; break;
    case '_': g = {{".....", ".....", ".....", ".....", ".....", ".....", "#####"}}; break;
    case ' ': g = {{".....", ".....", ".....", ".....", ".....", ".....", "....."}}; break;
    default: break;
    }
    return g;
}

bool inside(const PanelRect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void put(uint8_t* rgba, int w, int h, int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    uint8_t* p = rgba + (static_cast<size_t>(y * w + x) * 4u);
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = 255;
}

void blend(uint8_t* rgba, int w, int h, int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    uint8_t* p = rgba + (static_cast<size_t>(y * w + x) * 4u);
    p[0] = face_mix(p[0], c.r);
    p[1] = face_mix(p[1], c.g);
    p[2] = face_mix(p[2], c.b);
    p[3] = 255;
}

void fill(uint8_t* rgba, int w, int h, PanelRect r, Rgb c) {
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            put(rgba, w, h, x, y, c);
        }
    }
}

void fill_circle(uint8_t* rgba, int w, int h, int cx, int cy, int radius, Rgb c) {
    const int r2 = radius * radius;
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                put(rgba, w, h, x, y, c);
            }
        }
    }
}

void text(uint8_t* rgba, int w, int h, int x, int y, const char* s, Rgb ink, int scale, PanelRect clip) {
    if (s == nullptr || scale < 1) {
        return;
    }
    int cx = x;
    for (int i = 0; s[i] != '\0'; ++i) {
        const Glyph g = letter(s[i]);
        if (g.row[0] != nullptr) {
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (g.row[row][col] != '#') {
                        continue;
                    }
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            const int px = cx + col * scale + sx;
                            const int py = y + row * scale + sy;
                            if (inside(clip, px, py)) {
                                put(rgba, w, h, px, py, ink);
                            }
                        }
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

int text_width(const char* s, int scale) {
    int n = 0;
    if (s != nullptr) {
        while (s[n] != '\0') {
            ++n;
        }
    }
    if (n <= 0) {
        return 0;
    }
    return n * 6 * scale;
}

void label(uint8_t* rgba, int w, int h, PanelRect r, const char* s, Rgb ink, int scale) {
    const int tw = text_width(s, scale);
    const int th = 7 * scale;
    const int x = r.x + (r.w - tw) / 2;
    const int y = r.y + (r.h - th) / 2;
    text(rgba, w, h, x, y, s, ink, scale, r);
}

bool white_key(int index) {
    const int white[14] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23};
    for (int i = 0; i < 14; ++i) {
        if (white[i] == index) {
            return true;
        }
    }
    return false;
}

void layout_keys(PanelLayout& layout) {
    const int whiteW = 64;
    const int whiteH = 78;
    const int originX = 32;
    const int originY = 384;
    const int white[14] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23};
    for (int i = 0; i < 14; ++i) {
        layout.keys[white[i]] = {originX + i * whiteW, originY, whiteW, whiteH};
    }
    const int host[10] = {0, 1, 3, 4, 5, 7, 8, 10, 11, 12};
    const int black[10] = {1, 3, 6, 8, 10, 13, 15, 18, 20, 22};
    const int blackW = 36;
    const int blackH = 46;
    for (int i = 0; i < 10; ++i) {
        const PanelRect& base = layout.keys[white[host[i]]];
        layout.keys[black[i]] = {base.x + whiteW - blackW / 2, originY, blackW, blackH};
    }
}

void draw_face(uint8_t* rgba, int w, int h, const PanelLayout& layout, const PanelDrawIn& in) {
    const int cx = layout.mascot.x + layout.mascot.w / 2;
    const int cy = layout.mascot.y + layout.mascot.h / 2;
    const int colorIndex = in.recording ? 3 : in.baseColor;
    const Rgb color = face_color(colorIndex);
    const float turn = panel_ghost_degrees(in.seconds, in.playing);
    for (int y = layout.mascot.y; y < layout.mascot.y + layout.mascot.h; ++y) {
        for (int x = layout.mascot.x; x < layout.mascot.x + layout.mascot.w; ++x) {
            const float lx = static_cast<float>(x - cx);
            const float ly = static_cast<float>(cy - y);
            float rx = 0.f;
            float ry = 0.f;
            face_rotate(lx, ly, -turn, rx, ry);
            if (face_in_body(rx, ry)) {
                blend(rgba, w, h, x, y, color);
            }
            if (!face_in_body(lx, ly)) {
                continue;
            }
            put(rgba, w, h, x, y, color);
            if (face_in_slot(lx, ly)) {
                put(rgba, w, h, x, y, kFaceSlot);
            }
        }
    }
    if (in.character != nullptr && in.character[0] != '\0') {
        PanelRect line = layout.mascot;
        line.y = layout.mascot.y + layout.mascot.h - 18;
        line.h = 16;
        label(rgba, w, h, line, in.character, Rgb{210, 210, 216}, 1);
    }
}

void draw_keys(uint8_t* rgba, int w, int h, const PanelLayout& layout, const PanelDrawIn& in) {
    const Rgb white{228, 228, 234};
    const Rgb black{28, 28, 34};
    const Rgb ink{22, 22, 28};
    for (int i = 0; i < kPanelKeys; ++i) {
        if (white_key(i)) {
            fill(rgba, w, h, layout.keys[i], white);
        }
    }
    for (int i = 0; i < kPanelKeys; ++i) {
        if (!white_key(i)) {
            fill(rgba, w, h, layout.keys[i], black);
        }
    }
    for (int i = 0; i < kPanelKeys; ++i) {
        if (!in.keyUsed[i]) {
            continue;
        }
        int mx = 0;
        int my = 0;
        panel_key_mark(layout.keys[i], mx, my);
        fill(rgba, w, h, PanelRect{mx, my, 5, 5}, kFaceMagenta);
        if (!white_key(i)) {
            continue;
        }
        char name[32];
        int n = 0;
        while (in.keyName[i][n] != '\0' && n < 8) {
            name[n] = in.keyName[i][n];
            ++n;
        }
        name[n] = '\0';
        const PanelRect& key = layout.keys[i];
        text(rgba, w, h, key.x + 6, key.y + key.h - 22, name, ink, 1, key);
    }
}

}  // namespace

void panel_layout(PanelLayout& layout) {
    layout.screen = {16, 16, 928, 248};
    layout.mascot = {28, 32, 200, 200};
    int y = 36;
    for (int t = 0; t < 4; ++t) {
        layout.tracks[t] = {240, y, 688, 46};
        y += 54;
    }
    const int encX[4] = {78, 318, 558, 798};
    for (int i = 0; i < 4; ++i) {
        layout.enc[i] = {encX[i], 272, 56, 56};
    }
    int x = 24;
    for (int i = 0; i < 5; ++i) {
        layout.modes[i] = {x, 340, 176, 34};
        x += 184;
    }
    layout_keys(layout);
    const int by = 488;
    layout.lift = {24, by, 120, 40};
    layout.drop = {152, by, 120, 40};
    layout.rec = {280, by, 120, 40};
    layout.play = {408, by, 120, 40};
    layout.stop = {536, by, 120, 40};
    layout.rev = {664, by, 120, 40};
    layout.over = {792, by, 144, 40};
}

int panel_playhead_x(const PanelRect& track, double pos, int frames) {
    if (frames <= 1) {
        return track.x;
    }
    double t = pos / static_cast<double>(frames - 1);
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    return track.x + static_cast<int>(t * static_cast<double>(track.w - 1));
}

void panel_key_mark(const PanelRect& key, int& x, int& y) {
    x = key.x + key.w / 2 - 2;
    y = key.y + key.h - 10;
}

PanelHit panel_hit(int x, int y) {
    PanelLayout layout{};
    panel_layout(layout);
    PanelHit hit{PanelHitKind::None, 0};
    const int black[10] = {1, 3, 6, 8, 10, 13, 15, 18, 20, 22};
    for (int i = 0; i < 10; ++i) {
        if (inside(layout.keys[black[i]], x, y)) {
            hit.kind = PanelHitKind::Key;
            hit.index = black[i];
            return hit;
        }
    }
    for (int i = 0; i < kPanelKeys; ++i) {
        if (white_key(i) && inside(layout.keys[i], x, y)) {
            hit.kind = PanelHitKind::Key;
            hit.index = i;
            return hit;
        }
    }
    for (int t = 0; t < 4; ++t) {
        if (inside(layout.tracks[t], x, y)) {
            hit.kind = PanelHitKind::Track;
            hit.index = t;
            return hit;
        }
    }
    if (inside(layout.lift, x, y)) {
        hit.kind = PanelHitKind::Lift;
        return hit;
    }
    if (inside(layout.drop, x, y)) {
        hit.kind = PanelHitKind::Drop;
        return hit;
    }
    if (inside(layout.rec, x, y)) {
        hit.kind = PanelHitKind::Rec;
        return hit;
    }
    if (inside(layout.play, x, y)) {
        hit.kind = PanelHitKind::Play;
        return hit;
    }
    if (inside(layout.stop, x, y)) {
        hit.kind = PanelHitKind::Stop;
        return hit;
    }
    if (inside(layout.rev, x, y)) {
        hit.kind = PanelHitKind::Rev;
        return hit;
    }
    if (inside(layout.over, x, y)) {
        hit.kind = PanelHitKind::Over;
        return hit;
    }
    return hit;
}

void panel_draw(const PanelDrawIn& in, uint8_t* rgba, int w, int h) {
    if (rgba == nullptr || w <= 0 || h <= 0) {
        return;
    }
    PanelLayout layout{};
    panel_layout(layout);
    const Rgb ground = kFaceGround;
    const Rgb well{18, 18, 24};
    const Rgb lane{28, 28, 36};
    const Rgb armedLane{42, 28, 48};
    const Rgb button{36, 36, 44};
    const Rgb lit{48, 36, 56};
    const Rgb ink{232, 232, 236};
    const Rgb dim{150, 150, 158};
    const Rgb encColor[4] = {Rgb{226, 59, 59}, Rgb{59, 111, 226}, Rgb{196, 138, 42}, Rgb{242, 242, 244}};
    for (int i = 0; i < w * h; ++i) {
        rgba[i * 4] = ground.r;
        rgba[i * 4 + 1] = ground.g;
        rgba[i * 4 + 2] = ground.b;
        rgba[i * 4 + 3] = 255;
    }
    fill(rgba, w, h, layout.screen, well);
    for (int t = 0; t < 4; ++t) {
        fill(rgba, w, h, layout.tracks[t], t == in.arm ? armedLane : lane);
        if (t == in.arm) {
            fill(rgba, w, h, PanelRect{layout.tracks[t].x, layout.tracks[t].y, 5, layout.tracks[t].h}, kFaceMagenta);
        }
        char num[2] = {static_cast<char>('1' + t), '\0'};
        text(rgba, w, h, layout.tracks[t].x + 12, layout.tracks[t].y + 16, num, ink, 2, layout.tracks[t]);
        const float peak = in.peak[t] < 0.f ? 0.f : (in.peak[t] > 1.f ? 1.f : in.peak[t]);
        const int pw = static_cast<int>(peak * static_cast<float>(layout.tracks[t].w - 48));
        if (pw > 0) {
            fill(rgba, w, h, PanelRect{layout.tracks[t].x + 40, layout.tracks[t].y + layout.tracks[t].h - 10, pw, 4},
                 Rgb{90, 78, 98});
        }
        if (in.frames > 1) {
            const int hx = panel_playhead_x(layout.tracks[t], in.pos, in.frames);
            for (int yy = layout.tracks[t].y + 4; yy < layout.tracks[t].y + layout.tracks[t].h - 4; ++yy) {
                put(rgba, w, h, hx, yy, Rgb{242, 242, 244});
            }
        }
    }
    draw_face(rgba, w, h, layout, in);
    for (int i = 0; i < 4; ++i) {
        const int cx = layout.enc[i].x + layout.enc[i].w / 2;
        const int cy = layout.enc[i].y + layout.enc[i].h / 2;
        fill_circle(rgba, w, h, cx, cy, 26, encColor[i]);
        fill_circle(rgba, w, h, cx, cy - 16, 3, ground);
    }
    const char* modes[5] = {"Synth", "Drum", "Tape", "Mix", "In"};
    for (int i = 0; i < 5; ++i) {
        const bool tape = i == 2;
        fill(rgba, w, h, layout.modes[i], tape ? lit : button);
        label(rgba, w, h, layout.modes[i], modes[i], tape ? kFaceMagenta : dim, 2);
    }
    draw_keys(rgba, w, h, layout, in);
    const char* names[7] = {"Lift", "Drop", "Rec", "Play", "Stop", "Rev", in.overdub ? "Over" : "Replace"};
    const PanelRect buttons[7] = {layout.lift, layout.drop, layout.rec, layout.play, layout.stop, layout.rev, layout.over};
    const int hot[7] = {0, 0, in.recording, in.playing, 0, in.reverse, in.overdub};
    for (int i = 0; i < 7; ++i) {
        Rgb fillColor = button;
        Rgb word = ink;
        if (i == 2 && hot[i]) {
            fillColor = Rgb{58, 36, 96};
            word = kFaceViolet;
        } else if (hot[i]) {
            fillColor = lit;
            word = kFaceMagenta;
        }
        fill(rgba, w, h, buttons[i], fillColor);
        label(rgba, w, h, buttons[i], names[i], word, 2);
    }
}

int panel_write_ppm(const char* path, const uint8_t* rgba, int w, int h) {
    if (path == nullptr || rgba == nullptr || w <= 0 || h <= 0) {
        return 1;
    }
    FILE* out = std::fopen(path, "wb");
    if (out == nullptr) {
        return 1;
    }
    std::fprintf(out, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char rgb[3] = {rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]};
        if (std::fwrite(rgb, 1, 3, out) != 3) {
            std::fclose(out);
            return 1;
        }
    }
    std::fclose(out);
    return 0;
}
