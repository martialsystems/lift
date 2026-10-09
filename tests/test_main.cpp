// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "audio/prepare.h"
#include "audio/process_block.h"
#include "names/catalog.h"
#include "project/project.h"
#include "tape/character.h"
#include "tape/edit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

void solo(TapeRuntime& rt, int arm) {
    for (int t = 0; t < kTrackCount; ++t) {
        rt.mute[t] = t != arm;
        rt.fader[t] = 1.f;
    }
    rt.arm = arm;
}

void clear_track(TapeRuntime& rt, int track) {
    for (int i = 0; i < rt.frames; ++i) {
        rt.ch[track][0][i] = 0.f;
        rt.ch[track][1][i] = 0.f;
    }
}

void render(TapeRuntime& rt, const std::vector<float>& in, std::vector<float>& out, bool record) {
    out.assign(in.size(), 0.f);
    std::vector<float> outR(in.size(), 0.f);
    rt.pos = 0.0;
    rt.recording = record;
    rt.playing = true;
    rt.reverse = false;
    process_block(rt, in.data(), in.data(), out.data(), outR.data(), static_cast<int>(in.size()));
    rt.recording = false;
    rt.playing = false;
}

void bands(const std::vector<float>& x, int n, double& low, double& high) {
    low = 0.0;
    high = 0.0;
    const double sr = static_cast<double>(kSampleRate);
    for (int k = 1; k < n / 2; ++k) {
        const double freq = k * sr / static_cast<double>(n);
        double re = 0.0;
        double im = 0.0;
        for (int i = 0; i < n; ++i) {
            const double a = -2.0 * 3.14159265358979323846 * k * i / static_cast<double>(n);
            re += static_cast<double>(x[static_cast<size_t>(i)]) * std::cos(a);
            im += static_cast<double>(x[static_cast<size_t>(i)]) * std::sin(a);
        }
        const double mag = re * re + im * im;
        if (freq >= 200.0 && freq < 2000.0) {
            low += mag;
        }
        if (freq >= 8000.0 && freq < 16000.0) {
            high += mag;
        }
    }
}

double crest(const std::vector<float>& x, int n) {
    double peak = 0.0;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double a = std::fabs(static_cast<double>(x[static_cast<size_t>(i)]));
        if (a > peak) {
            peak = a;
        }
        sum += a * a;
    }
    const double rms = std::sqrt(sum / static_cast<double>(n));
    if (rms <= 1e-12) {
        return 0.0;
    }
    return peak / rms;
}

std::vector<float> make_hit(int n) {
    std::vector<float> hit(static_cast<size_t>(n), 0.f);
    uint32_t rng = 99u;
    for (int i = 0; i < n; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const float white = static_cast<float>(static_cast<int32_t>(rng)) / 2147483648.f;
        const float env = i < 1400 ? std::exp(-static_cast<float>(i) / 220.f) : 0.f;
        hit[static_cast<size_t>(i)] = white * env * 1.3f;
    }
    return hit;
}

void test_names() {
    CHECK(catalog_engine_count() == 8);
    CHECK(std::strcmp(catalog_engine(0), "Loom") == 0);
    CHECK(std::strcmp(catalog_engine(1), "Bend") == 0);
    CHECK(std::strcmp(catalog_engine(7), "Spare") == 0);
    CHECK(catalog_effect_count() == 8);
    CHECK(std::strcmp(catalog_effect(0), "Drip") == 0);
    CHECK(std::strcmp(catalog_effect(7), "Space") == 0);
    CHECK(catalog_sequencer_count() == 2);
    CHECK(std::strcmp(catalog_sequencer(0), "Steps") == 0);
    CHECK(std::strcmp(catalog_sequencer(1), "Latch") == 0);
    CHECK(catalog_character_count() == 4);
    CHECK(std::strcmp(catalog_drum(0), "Tap") == 0);
    CHECK(catalog_glyph_count() == 76);
    for (int i = 0; i < catalog_character_count(); ++i) {
        CHECK(std::strcmp(catalog_character(i), character_row(i).name) == 0);
    }
    const Character& deck = character_row(0);
    const Character& pocket = character_row(1);
    const Character& shed = character_row(2);
    const Character& cap = character_row(3);
    CHECK(pocket.lowpassHz == 12000.f);
    CHECK(pocket.flutterDepth > deck.flutterDepth);
    CHECK(pocket.hissDb > deck.hissDb);
    CHECK(pocket.bumpDb > deck.bumpDb);
    CHECK(shed.wowDepth > pocket.wowDepth);
    CHECK(shed.lowpassHz < deck.lowpassHz);
    CHECK(shed.lowpassHz > pocket.lowpassHz);
    CHECK(cap.bumpDb == 0.f);
    CHECK(cap.preEmph == 0.f);
    CHECK(cap.wowDepth < deck.wowDepth);
    CHECK(cap.lowpassHz < pocket.lowpassHz);
    CHECK(kDefaultFrames == kSampleRate * 6 * 60);
    CHECK(kRecordRate == 1.f);
}

void test_version() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lift-version-probe";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "project.json");
        out << "{ \"version\": 2, \"tempo\": 999 }\n";
    }
    ProjectInfo info;
    project_info_init(info);
    info.tempo = 42.f;
    const LoadStatus status = project_read_info(dir.string().c_str(), info);
    CHECK(status == LoadStatus::NewerVersion);
    CHECK(info.tempo == 42.f);
    CHECK(info.version == kProjectVersion);
    fs::remove_all(dir);
}

void test_roundtrip() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lift-roundtrip";
    fs::remove_all(dir);
    TapeRuntime rt;
    transport_init(rt);
    constexpr int n = 2048;
    prepare_tracks(rt, n);
    solo(rt, 0);
    transport_bind_character(rt, 1);
    for (int i = 0; i < n; ++i) {
        rt.ch[0][0][i] = (i == 10) ? 0.75f : 0.01f * static_cast<float>(i % 17);
        rt.ch[0][1][i] = -rt.ch[0][0][i];
    }
    ProjectInfo info;
    project_info_init(info);
    info.frames = n;
    info.tempo = 96.f;
    info.tapeIndex = 3;
    info.character = 1;
    std::snprintf(info.uiMode, sizeof info.uiMode, "bay");
    CHECK(project_write(dir.string().c_str(), info, rt) == LoadStatus::Ok);
    const float kept = rt.ch[0][0][10];
    release_tracks(rt);
    ProjectInfo loaded;
    project_info_init(loaded);
    loaded.tempo = 1.f;
    CHECK(project_read_info(dir.string().c_str(), loaded) == LoadStatus::Ok);
    CHECK(loaded.tempo == 96.f);
    CHECK(loaded.tapeIndex == 3);
    CHECK(loaded.character == 1);
    CHECK(std::strcmp(loaded.uiMode, "bay") == 0);
    TapeRuntime again;
    transport_init(again);
    CHECK(project_read_audio(dir.string().c_str(), loaded, again) == LoadStatus::Ok);
    CHECK(again.ch[0][0][10] == kept);
    CHECK(again.ch[0][1][10] == -kept);
    release_tracks(again);
    fs::remove_all(dir);
}

void test_record_rate() {
    TapeRuntime rt;
    transport_init(rt);
    constexpr int n = 128;
    prepare_tracks(rt, n);
    solo(rt, 0);
    clear_track(rt, 0);
    transport_set_varispeed(rt, 8.f);
    CHECK(rt.varispeed == kMaxVarispeed);
    transport_set_varispeed(rt, 0.01f);
    CHECK(rt.varispeed == kMinVarispeed);
    transport_set_varispeed(rt, 4.f);
    rt.reverse = true;
    std::vector<float> in(static_cast<size_t>(n), 0.f);
    in[10] = 1.f;
    std::vector<float> outL(static_cast<size_t>(n), 0.f);
    std::vector<float> outR(static_cast<size_t>(n), 0.f);
    rt.pos = 0.0;
    rt.recording = true;
    rt.playing = true;
    process_block(rt, in.data(), in.data(), outL.data(), outR.data(), n);
    CHECK(rt.pos == static_cast<double>(n));
    CHECK(std::fabs(rt.ch[0][0][10]) > std::fabs(rt.ch[0][0][40]) * 2.f);
    CHECK(std::fabs(rt.ch[0][0][10]) > 0.05f);
    release_tracks(rt);
}

void test_reverse_and_scrub() {
    TapeRuntime rt;
    transport_init(rt);
    prepare_tracks(rt, 512);
    rt.ch[0][0][3] = 0.4f;
    transport_rev(rt);
    CHECK(rt.reverse);
    transport_rev(rt);
    CHECK(!rt.reverse);
    transport_rev(rt);
    transport_stop(rt);
    CHECK(!rt.reverse);
    CHECK(!rt.playing);
    CHECK(!rt.recording);
    transport_scrub(rt, 12.5);
    CHECK(rt.pos == 12.5);
    CHECK(rt.ch[0][0][3] == 0.4f);
    rt.playing = true;
    transport_scrub(rt, 5.0);
    CHECK(rt.pos == 12.5);
    release_tracks(rt);
}

void test_lift_drop() {
    TapeRuntime rt;
    transport_init(rt);
    constexpr int n = 2000;
    prepare_tracks(rt, n);
    solo(rt, 0);
    for (int i = 0; i < n; ++i) {
        rt.ch[0][0][i] = 0.5f;
        rt.ch[0][1][i] = 0.5f;
    }
    for (int i = 0; i < 1000; ++i) {
        rt.ch[0][0][i] = 0.25f;
        rt.ch[0][1][i] = 0.25f;
    }
    rt.loopStart = 0;
    rt.loopEnd = 1000;
    tape_lift(rt);
    CHECK(rt.clipFrames == 1000);
    for (int i = 0; i < n; ++i) {
        rt.ch[0][0][i] = 0.5f;
        rt.ch[0][1][i] = 0.5f;
    }
    rt.pos = 400;
    tape_drop(rt, true);
    CHECK(std::fabs(rt.ch[0][0][400 + 500] - 0.75f) < 1e-5f);
    CHECK(rt.ch[0][0][401] > 0.5f);
    CHECK(rt.ch[0][0][401] < 0.55f);
    for (int i = 0; i < n; ++i) {
        rt.ch[0][0][i] = 0.5f;
        rt.ch[0][1][i] = 0.5f;
    }
    rt.pos = 800;
    tape_drop(rt, false);
    CHECK(std::fabs(rt.ch[0][0][800 + 500] - 0.25f) < 1e-5f);
    CHECK(rt.ch[0][0][800] > 0.4f);
    release_tracks(rt);
}

void test_metronome() {
    TapeRuntime rt;
    transport_init(rt);
    constexpr int n = kSampleRate;
    prepare_tracks(rt, n);
    solo(rt, 0);
    clear_track(rt, 0);
    rt.metronome = true;
    rt.resampleInput = false;
    rt.metroPeriod = kSampleRate / 2;
    rt.metroCounter = 100;
    std::vector<float> in(static_cast<size_t>(n), 0.f);
    std::vector<float> outL(static_cast<size_t>(n), 0.f);
    std::vector<float> outR(static_cast<size_t>(n), 0.f);
    rt.pos = 0.0;
    rt.recording = true;
    rt.playing = true;
    process_block(rt, in.data(), in.data(), outL.data(), outR.data(), n);
    float trackPeak = 0.f;
    float outPeak = 0.f;
    for (int i = 0; i < n; ++i) {
        trackPeak = std::max(trackPeak, std::fabs(rt.ch[0][0][i]));
        outPeak = std::max(outPeak, std::fabs(outL[static_cast<size_t>(i)]));
    }
    CHECK(trackPeak < 1e-3f);
    CHECK(outPeak > 0.2f);
    release_tracks(rt);
}

float max_step(const std::vector<float>& x) {
    float step = 0.f;
    for (size_t i = 1; i < x.size(); ++i) {
        step = std::max(step, std::fabs(x[i] - x[i - 1]));
    }
    return step;
}

void test_reverse_click() {
    TapeRuntime rt;
    transport_init(rt);
    constexpr int n = 4096;
    prepare_tracks(rt, n);
    solo(rt, 0);
    transport_bind_character(rt, 0);
    for (int i = 0; i < n; ++i) {
        const float s = -1.f + 2.f * static_cast<float>(i) / static_cast<float>(n - 1);
        rt.ch[0][0][i] = s;
        rt.ch[0][1][i] = s;
    }
    rt.loopStart = 0;
    rt.loopEnd = n;
    rt.varispeed = 1.f;
    rt.metronome = false;
    transport_reset_filters(rt);
    std::vector<float> in(static_cast<size_t>(n * 2), 0.f);
    std::vector<float> out(static_cast<size_t>(n * 2), 0.f);
    std::vector<float> outR(static_cast<size_t>(n * 2), 0.f);
    rt.pos = 0.0;
    rt.playing = true;
    rt.recording = false;
    rt.reverse = false;
    process_block(rt, in.data(), in.data(), out.data(), outR.data(), n * 2);
    const float forward = max_step(out);
    std::cerr << "forward boundary step " << forward << "\n";
    CHECK(forward < 0.2f);
    for (int i = 0; i < n; ++i) {
        const float s = -1.f + 2.f * static_cast<float>(i) / static_cast<float>(n - 1);
        rt.ch[0][0][i] = s;
        rt.ch[0][1][i] = s;
    }
    transport_reset_filters(rt);
    rt.pos = static_cast<double>(n) / 2.0;
    rt.playing = true;
    rt.recording = false;
    rt.reverse = true;
    rt.rng = 1u;
    process_block(rt, in.data(), in.data(), out.data(), outR.data(), n * 2);
    const float backward = max_step(out);
    std::cerr << "reverse boundary step " << backward << "\n";
    CHECK(backward < 0.2f);
    release_tracks(rt);
}

struct PassMeasure {
    double ratio;
    double crest;
    double centroidProxy;
};

PassMeasure measure_pass(const std::vector<float>& x) {
    const int n = static_cast<int>(x.size());
    double low = 0.0;
    double high = 0.0;
    bands(x, n, low, high);
    PassMeasure m;
    m.ratio = high / std::max(low, 1e-12);
    m.crest = crest(x, n);
    m.centroidProxy = m.ratio;
    return m;
}

std::vector<float> generation(TapeRuntime& rt, const std::vector<float>& in, int character) {
    transport_bind_character(rt, character);
    transport_reset_filters(rt);
    rt.rng = 1u;
    rt.wowPhase = 0.f;
    rt.varispeed = 1.f;
    rt.loopStart = 0;
    rt.loopEnd = 0;
    rt.metronome = false;
    rt.resampleInput = false;
    rt.reverse = false;
    solo(rt, 0);
    clear_track(rt, 0);
    std::vector<float> recorded;
    render(rt, in, recorded, true);
    transport_reset_filters(rt);
    rt.rng = 1u;
    std::vector<float> silent(in.size(), 0.f);
    std::vector<float> played;
    render(rt, silent, played, false);
    return played;
}

void test_overdub() {
    constexpr int n = 4096;
    TapeRuntime rt;
    transport_init(rt);
    prepare_tracks(rt, n);
    const std::vector<float> hit = make_hit(n);
    const PassMeasure dry = measure_pass(hit);
    for (int character = 0; character < 2; ++character) {
        std::vector<float> sig = hit;
        PassMeasure last = dry;
        for (int pass = 0; pass < 4; ++pass) {
            sig = generation(rt, sig, character);
            last = measure_pass(sig);
        }
        std::cerr << character_row(character).name << " dry crest " << dry.crest << " pass4 crest " << last.crest
                  << " dry ratio " << dry.ratio << " pass4 ratio " << last.ratio << "\n";
        CHECK(last.crest < dry.crest);
        CHECK(last.ratio < dry.ratio * 0.85);
    }
    std::vector<float> deck = hit;
    std::vector<float> pocket = hit;
    for (int pass = 0; pass < 4; ++pass) {
        deck = generation(rt, deck, 0);
        pocket = generation(rt, pocket, 1);
    }
    const PassMeasure deckM = measure_pass(deck);
    const PassMeasure pocketM = measure_pass(pocket);
    std::cerr << "pass4 ratio Deck " << deckM.ratio << " Pocket " << pocketM.ratio << "\n";
    CHECK(pocketM.ratio < deckM.ratio);

    auto silence_rms = [&](int character) {
        transport_bind_character(rt, character);
        transport_reset_filters(rt);
        rt.rng = 3u;
        solo(rt, 0);
        clear_track(rt, 0);
        std::vector<float> silent(static_cast<size_t>(n), 0.f);
        std::vector<float> played;
        render(rt, silent, played, false);
        double sum = 0.0;
        const int start = n / 2;
        for (int i = start; i < n; ++i) {
            const double a = played[static_cast<size_t>(i)];
            sum += a * a;
        }
        return std::sqrt(sum / static_cast<double>(n - start));
    };
    const double deckHiss = silence_rms(0);
    const double pocketHiss = silence_rms(1);
    std::cerr << "hiss rms Deck " << deckHiss << " Pocket " << pocketHiss << "\n";
    CHECK(pocketHiss > deckHiss * 3.0);
    release_tracks(rt);
}

void test_copyright() {
    namespace fs = std::filesystem;
    const fs::path root{LIFT_SOURCE_DIR};
    const std::string line = "// Copyright (c) 2026 Martial Systems LLC. All rights reserved.";
    int seen = 0;
    for (const char* folder : {"src", "tests"}) {
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root / folder)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto ext = entry.path().extension();
            if (ext != ".h" && ext != ".cpp") {
                continue;
            }
            std::ifstream in(entry.path());
            std::string first;
            std::getline(in, first);
            if (!first.empty() && first.back() == '\r') {
                first.pop_back();
            }
            CHECK(first == line);
            ++seen;
        }
    }
    CHECK(seen > 10);
}

struct NamedTest {
    const char* name;
    void (*fn)();
};

const NamedTest kTests[] = {
    {"names", test_names},
    {"version", test_version},
    {"roundtrip", test_roundtrip},
    {"record", test_record_rate},
    {"scrub", test_reverse_and_scrub},
    {"edit", test_lift_drop},
    {"metronome", test_metronome},
    {"reverse", test_reverse_click},
    {"overdub", test_overdub},
    {"copyright", test_copyright},
};

}  // namespace

int run_pool_suite(const char* only);

int main(int argc, char** argv) {
    const char* only = nullptr;
    if (argc >= 3 && std::strcmp(argv[1], "--only") == 0) {
        only = argv[2];
    }
    for (const NamedTest& test : kTests) {
        if (only != nullptr && std::strcmp(only, test.name) != 0) {
            continue;
        }
        test.fn();
    }
    g_fails += run_pool_suite(only);
    if (g_fails != 0) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
