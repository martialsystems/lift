// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
// Renders one LIFT drum voice, one hit, to a mono 32-bit float WAV, for
// tools/drum_ref_analysis.py (tuning against reference one-shots).
//   lift_drum_render <kit 0..1> <voice 0..13> <out.wav> [SHOGUN:PARAM=u ...]
//   lift_drum_render <kit> <voice> --get SHOGUN:PARAM ...   (prints the kit's values)
// Overrides are SHOGUN parameter ids ("SD:TDECAY=0.3"), applied over the kit.
#include "engine/drums.h"
#include "params.h"
#include "shogun.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s kit voice out.wav [ID=u ...]\n", argv[0]);
        return 2;
    }
    const int kit = std::atoi(argv[1]), voice = std::atoi(argv[2]);
    const double fs = 48000.0;
    auto d = std::make_unique<lift::eng::Drums>();
    d->prepare(fs);
    d->setKit(kit);
    if (std::strcmp(argv[3], "--get") == 0) {  // print the kit's values of the named params
        for (int i = 4; i < argc; ++i) {
            const int id = shogun::findParam(argv[i]);
            std::printf("%s=%.4f\n", argv[i], id >= 0 ? d->engine().param(id) : -1.0);
        }
        return 0;
    }
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        if (a.substr(0, eq) == "LIFT:BEND") {
            d->setBend(voice, static_cast<float>(std::atof(a.substr(eq + 1).c_str())));
            continue;
        }
        const int id = shogun::findParam(a.substr(0, eq).c_str());
        if (id < 0) {
            std::fprintf(stderr, "unknown param %s\n", a.substr(0, eq).c_str());
            return 2;
        }
        d->engine().setParamNow(id, std::atof(a.substr(eq + 1).c_str()));
    }
    float l, r;
    for (int i = 0; i < 4800; ++i) d->render(&l, &r, 1, nullptr, nullptr);
    d->hit(voice, 0.8f);
    const int n = static_cast<int>(2.5 * fs);
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        d->render(&l, &r, 1, nullptr, nullptr);
        x[static_cast<size_t>(i)] = 0.5f * (l + r);
    }
    FILE* f = std::fopen(argv[3], "wb");
    if (f == nullptr) return 1;
    auto u32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + 4u * static_cast<unsigned>(n));
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3);
    u16(1);
    u32(48000);
    u32(48000 * 4);
    u16(4);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(4u * static_cast<unsigned>(n));
    std::fwrite(x.data(), 4, x.size(), f);
    std::fclose(f);
    return 0;
}
