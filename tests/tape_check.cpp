// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Offline renders through the tape engine, checked in code. Writes WAVs to the
// folder in LIFT_RENDER_DIR (default ./lift-renders). Exit code 0 means pass.

#include "audio/prepare.h"
#include "audio/process_block.h"
#include "tape/engine.h"
#include "tape/transport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
int g_fails = 0;
std::string g_dir;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++g_fails;
    }
}

using Buf = std::vector<float>;

bool clean(const Buf& x) {
    for (float v : x) {
        if (!std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) {
            return false;
        }
    }
    return true;
}

void write_wav(const std::string& name, const Buf& l, const Buf& r) {
    const std::string path = g_dir + "/" + name;
    std::ofstream f(path, std::ios::binary);
    const uint32_t n = static_cast<uint32_t>(l.size());
    const uint32_t bytes = n * 2u * 4u;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36u + bytes);
    f.write("WAVEfmt ", 8);
    u32(16u);
    u16(3u);
    u16(2u);
    u32(48000u);
    u32(48000u * 8u);
    u16(8u);
    u16(32u);
    f.write("data", 4);
    u32(bytes);
    for (uint32_t i = 0; i < n; ++i) {
        f.write(reinterpret_cast<const char*>(&l[i]), 4);
        f.write(reinterpret_cast<const char*>(&r[i]), 4);
    }
    std::printf("  wrote %s\n", path.c_str());
}

Buf sine(double hz, double amp, int n) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        x[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / kFs));
    }
    return x;
}

Buf log_sweep(double f0, double f1, double amp, int n) {
    Buf x(static_cast<size_t>(n));
    const double T = n / kFs;
    const double k = std::log(f1 / f0);
    for (int i = 0; i < n; ++i) {
        const double t = i / kFs;
        x[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * f0 * T / k * (std::exp(t * k / T) - 1.0)));
    }
    return x;
}

// Amplitude of one frequency over [from, to), Goertzel style.
double tone_amp(const Buf& x, double hz, int from, int to) {
    double re = 0.0;
    double im = 0.0;
    for (int i = from; i < to; ++i) {
        const double ph = 2.0 * kPi * hz * i / kFs;
        re += x[static_cast<size_t>(i)] * std::cos(ph);
        im += x[static_cast<size_t>(i)] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (to - from);
}

double thd(const Buf& x, double hz, int from, int to) {
    const double f = tone_amp(x, hz, from, to);
    double h = 0.0;
    for (int k = 2; k <= 12 && k * hz < kFs / 2; ++k) {
        const double a = tone_amp(x, k * hz, from, to);
        h += a * a;
    }
    return std::sqrt(h) / std::max(f, 1e-12);
}

TapeParams quiet_params(const Character& row) {
    TapeEngine tmp;
    tape_engine_init(tmp);
    tape_engine_set_character(tmp, row);
    TapeParams p = tmp.p;
    p.wowDepth = 0.f;
    p.flutterDepth = 0.f;
    p.hissDb = -120.f;
    return p;
}

void record(TapeEngine& e, const Buf& in, Buf& out) {
    out.assign(in.size(), 0.f);
    Buf r(in.size(), 0.f);
    tape_engine_record(e, in.data(), in.data(), out.data(), r.data(), static_cast<int>(in.size()));
}

void play(TapeEngine& e, const Buf& in, Buf& outL, Buf& outR, float varispeed = 1.f) {
    outL.assign(in.size(), 0.f);
    outR.assign(in.size(), 0.f);
    TapeNoDenormals guard;
    for (size_t i = 0; i < in.size(); ++i) {
        tape_engine_next_speed(e, varispeed);
        float l = in[i];
        float r = in[i];
        tape_engine_play(e, l, r);
        outL[i] = l;
        outR[i] = r;
    }
}

// Per-cycle frequency from interpolated rising zero crossings.
std::vector<double> cycle_freqs(const Buf& x, int from) {
    std::vector<double> cross;
    for (size_t i = static_cast<size_t>(from) + 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.f && x[i] >= 0.f) {
            const double t = static_cast<double>(i - 1) + static_cast<double>(x[i - 1]) / (static_cast<double>(x[i - 1]) - x[i]);
            cross.push_back(t);
        }
    }
    std::vector<double> f;
    for (size_t i = 1; i < cross.size(); ++i) {
        f.push_back(kFs / (cross[i] - cross[i - 1]));
    }
    return f;
}

double max_dev(const std::vector<double>& f, double hz) {
    double m = 0.0;
    for (double v : f) {
        m = std::max(m, std::fabs(v / hz - 1.0));
    }
    return m;
}

void test_drive_thd(std::unique_ptr<TapeEngine>& e) {
    std::printf("\n-- saturation and hysteresis (4x oversampled record chain)\n");
    const int n = 48000;
    const Buf in = sine(1000.0, 0.5, n);
    const float drives[] = {0.5f, 1.f, 2.f, 4.f, 8.f};
    double last = -1.0;
    bool rising = true;
    bool ok = true;
    Buf out;
    for (float d : drives) {
        TapeParams p = quiet_params(character_row(0));
        p.drive = d;
        tape_engine_set_params(*e, p);
        tape_engine_reset(*e);
        record(*e, in, out);
        ok = ok && clean(out);
        const double t = thd(out, 1000.0, n / 2, n);
        std::printf("  drive %.1f  THD %.3f %%\n", d, t * 100.0);
        rising = rising && t > last;
        last = t;
        if (d == 1.f) {
            write_wav("sine1k_drive1.wav", out, out);
        }
        if (d == 8.f) {
            write_wav("sine1k_drive8.wav", out, out);
        }
    }
    check(ok, "record chain output finite, no denormals");
    check(rising, "THD rises with every drive step");
    check(last > 0.03, "drive 8 THD above 3 %");
    double under = 0.0;
    double over = 0.0;
    for (int b = 0; b < 2; ++b) {
        TapeParams p = quiet_params(character_row(0));
        p.drive = 1.f;
        p.bias = b == 0 ? 0.05f : 0.95f;
        tape_engine_set_params(*e, p);
        tape_engine_reset(*e);
        record(*e, sine(1000.0, 0.1, n), out);
        (b == 0 ? under : over) = thd(out, 1000.0, n / 2, n);
    }
    std::printf("  low level THD: under-bias %.3f %%, over-bias %.3f %%\n", under * 100.0, over * 100.0);
    check(under > over * 2.0, "under-bias distorts more than over-bias");
    const double gain = tone_amp(out, 1000.0, n / 2, n) / 0.1;
    check(gain > 0.8 && gain < 1.25, "small-signal record gain near unity at drive 1 (" + std::to_string(gain) + ")");
    std::printf("  record latency %d samples\n", tape_engine_latency(*e));
}

void test_speed_eq(std::unique_ptr<TapeEngine>& e) {
    std::printf("\n-- head bump and gap loss vs tape speed\n");
    const int n = 24000;
    const float speeds[] = {7.5f, 15.f, 30.f};
    double hf[3];
    double bump55[3];
    Buf l;
    Buf r;
    bool ok = true;
    for (int s = 0; s < 3; ++s) {
        auto gain = [&](double hz) {
            TapeParams p = quiet_params(character_row(1));
            p.speedIps = speeds[s];
            tape_engine_set_params(*e, p);
            tape_engine_reset(*e);
            play(*e, sine(hz, 0.25, n), l, r);
            ok = ok && clean(l);
            return 20.0 * std::log10(tone_amp(l, hz, n / 2, n) / 0.25);
        };
        const double ref = gain(1000.0);
        hf[s] = gain(10000.0) - ref;
        bump55[s] = gain(55.0) - ref;
        const double bumpHere = gain(110.0 * speeds[s] / 15.0) - ref;
        std::printf("  %4.1f ips  10 kHz %+6.2f dB   55 Hz %+5.2f dB   bump @%5.1f Hz %+5.2f dB\n", speeds[s], hf[s],
                    bump55[s], 110.0 * speeds[s] / 15.0, bumpHere);
        if (s == 1) {
            check(bumpHere > 3.0, "head bump present (Pocket, 15 ips)");
        }
    }
    check(ok, "playback chain output finite, no denormals");
    check(hf[0] < hf[1] - 1.0 && hf[1] < hf[2] - 1.0, "10 kHz rolls off more at lower speed");
    check(hf[0] < -6.0, "7.5 ips Pocket loses at least 6 dB at 10 kHz");
    check(bump55[0] > bump55[2] + 2.0, "head bump moves down with speed");

    const int sn = 48000 * 5;
    const Buf sw = log_sweep(20.0, 20000.0, 0.3, sn);
    write_wav("sweep_dry.wav", sw, sw);
    for (int s = 0; s < 3; s += 2) {
        TapeEngine& t = *e;
        tape_engine_set_character(t, character_row(1));
        TapeParams p = t.p;
        p.speedIps = speeds[s];
        tape_engine_set_params(t, p);
        tape_engine_reset(t);
        Buf rec;
        record(t, sw, rec);
        play(t, rec, l, r);
        write_wav(s == 0 ? "sweep_pocket_7_5ips.wav" : "sweep_pocket_30ips.wav", l, r);
    }
}

void test_wow_flutter(std::unique_ptr<TapeEngine>& e) {
    std::printf("\n-- wow and flutter (modulated fractional delay)\n");
    const int n = 48000 * 4;
    const double hz = 1000.0;
    const Buf in = sine(hz, 0.5, n);
    Buf l;
    Buf r;
    struct Case {
        const char* name;
        float wowHz, wow, flutHz, flut;
        double lo;
    };
    const Case cases[] = {
        {"wow 0.2 % @ 1 Hz", 1.f, 0.002f, 8.f, 0.f, 0.85},
        {"flutter 0.05 % @ 8 Hz", 1.f, 0.f, 8.f, 0.0005f, 0.5},
        {"wow 0.15 % + flutter 0.05 %", 0.6f, 0.0015f, 12.f, 0.0005f, 0.6},
    };
    for (const Case& c : cases) {
        TapeParams p = quiet_params(character_row(0));
        p.wowHz = c.wowHz;
        p.wowDepth = c.wow;
        p.flutterHz = c.flutHz;
        p.flutterDepth = c.flut;
        tape_engine_set_params(*e, p);
        tape_engine_reset(*e);
        play(*e, in, l, r);
        const double dev = max_dev(cycle_freqs(l, 24000), hz);
        const double cfg = c.wow + c.flut;
        std::printf("  %-30s configured %.4f %%  measured peak %.4f %%\n", c.name, cfg * 100.0, dev * 100.0);
        check(clean(l) && dev <= cfg * 1.1 && dev >= cfg * c.lo,
              std::string("pitch deviation within configured range: ") + c.name);
    }
    TapeParams p = quiet_params(character_row(0));
    tape_engine_set_params(*e, p);
    tape_engine_reset(*e);
    play(*e, in, l, r);
    const double still = max_dev(cycle_freqs(l, 24000), hz);
    check(still < 2e-5, "no wow, no flutter: pitch steady (" + std::to_string(still * 100.0) + " %)");

    tape_engine_set_character(*e, character_row(2));
    p = e->p;
    p.wowDepth = 0.004f;
    p.flutterDepth = 0.0008f;
    tape_engine_set_params(*e, p);
    tape_engine_reset(*e);
    Buf rec;
    record(*e, sine(440.0, 0.4, n), rec);
    play(*e, rec, l, r);
    write_wav("sine440_shed_heavy_wow.wav", l, r);
}

Buf chord(int n) {
    Buf x(static_cast<size_t>(n));
    const double f[] = {220.0, 277.18, 329.63, 440.0};
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (double v : f) {
            s += std::sin(2.0 * kPi * v * i / kFs);
        }
        x[static_cast<size_t>(i)] = static_cast<float>(0.15 * s);
    }
    return x;
}

double max_step(const Buf& x, int from, int to) {
    double m = 0.0;
    for (int i = std::max(from, 2); i < to; ++i) {
        m = std::max(m, static_cast<double>(std::fabs(x[static_cast<size_t>(i)] - x[static_cast<size_t>(i - 1)])));
    }
    return m;
}

double max_curve(const Buf& x, int from, int to) {
    double m = 0.0;
    for (int i = std::max(from, 2); i < to; ++i) {
        const size_t k = static_cast<size_t>(i);
        m = std::max(m, static_cast<double>(std::fabs(x[k] - 2.f * x[k - 1] + x[k - 2])));
    }
    return m;
}

void test_transport() {
    std::printf("\n-- varispeed, tape stop and start, sound on sound (TapeRuntime + process_block)\n");
    auto rt = std::make_unique<TapeRuntime>();
    transport_init(*rt);
    const int frames = 48000 * 8;
    prepare_tracks(*rt, frames);
    for (int t = 0; t < kTrackCount; ++t) {
        rt->mute[t] = t != 0;
    }
    rt->arm = 0;
    const int block = 256;
    auto run = [&](Buf& outL, Buf& outR, const Buf* in, int n) {
        const size_t at = outL.size();
        outL.resize(at + static_cast<size_t>(n));
        outR.resize(at + static_cast<size_t>(n));
        for (int i = 0; i < n; i += block) {
            const int m = std::min(block, n - i);
            const float* src = in != nullptr ? in->data() + i : nullptr;
            process_block(*rt, src, src, outL.data() + at + i, outR.data() + at + i, m);
        }
    };

    // Print a chord through the record chain.
    const Buf music = chord(48000 * 4);
    Buf l;
    Buf r;
    rt->pos = 0.0;
    rt->recording = true;
    rt->playing = true;
    run(l, r, &music, static_cast<int>(music.size()));
    rt->recording = false;
    const Buf take1(rt->ch[0][0], rt->ch[0][0] + music.size());
    check(clean(take1) && tone_amp(take1, 440.0, 48000, 96000) > 0.08, "print lands on the armed track");
    // Alignment: print should match the input at lag 0 (latency compensated).
    auto lag_of = [&](const Buf& a, const Buf& b) {
        int best = 0;
        double bestv = -1e30;
        for (int lag = -64; lag <= 64; ++lag) {
            double s = 0.0;
            for (int i = 48000; i < 96000; ++i) {
                s += static_cast<double>(a[static_cast<size_t>(i)]) * b[static_cast<size_t>(i + lag)];
            }
            if (s > bestv) {
                bestv = s;
                best = lag;
            }
        }
        return best;
    };
    const int lag1 = lag_of(music, take1);
    check(std::abs(lag1) <= 1, "print is time aligned with the input (lag " + std::to_string(lag1) + ")");

    // Sound on sound: overdub a 1 kHz tone; the old take goes through the chain again.
    const Buf tone = sine(1000.0, 0.2, static_cast<int>(music.size()));
    l.clear();
    r.clear();
    rt->pos = 0.0;
    rt->overdub = true;
    rt->recording = true;
    run(l, r, &tone, static_cast<int>(tone.size()));
    rt->recording = false;
    rt->overdub = false;
    const Buf take2(rt->ch[0][0], rt->ch[0][0] + music.size());
    // Each print has a small-signal gain of sqrt(drive), as a hotter print does on a deck.
    const double keep = tone_amp(take2, 440.0, 48000, 96000) / tone_amp(take1, 440.0, 48000, 96000) /
                        std::sqrt(static_cast<double>(rt->engine.p.drive));
    check(clean(take2) && keep > 0.7 && keep < 1.3 && tone_amp(take2, 1000.0, 48000, 96000) > 0.12,
          "overdub keeps the old take and adds the new one through the record chain");
    const int lag2 = lag_of(take1, take2);
    check(std::abs(lag2) <= 1, "overdub stays aligned (lag " + std::to_string(lag2) + ")");

    // Play, then a 1.2 s tape stop. Hiss off so the click checks see only the signal.
    TapeParams quiet = rt->engine.p;
    quiet.hissDb = -120.f;
    tape_engine_set_params(rt->engine, quiet);
    Buf out;
    Buf outR;
    rt->pos = 0.0;
    rt->playing = true;
    run(out, outR, nullptr, 48000);
    const int stopAt = static_cast<int>(out.size());
    transport_tape_stop(*rt, 1.2f);
    run(out, outR, nullptr, 48000 * 2);
    int halt = static_cast<int>(out.size());
    for (int i = static_cast<int>(out.size()) - 1; i >= stopAt; --i) {
        if (out[static_cast<size_t>(i)] != 0.f) {
            halt = i + 1;
            break;
        }
    }
    std::printf("  tape stop: halted after %.3f s\n", (halt - stopAt) / kFs);
    check(!rt->playing, "transport stopped after the ramp");
    check(clean(out), "stop render finite, no denormals");
    check(std::fabs(halt - stopAt - 1.2 * kFs) < 0.01 * kFs, "ramp length matches the setting");
    double tail = 0.0;
    for (int i = halt - 480; i < halt; ++i) {
        tail = std::max(tail, static_cast<double>(std::fabs(out[static_cast<size_t>(i)])));
    }
    check(tail < 2e-3, "level reaches silence at the end of the ramp (" + std::to_string(tail) + ")");
    const double steadyStep = max_step(out, 4800, stopAt);
    const double steadyCurve = max_curve(out, 4800, stopAt);
    check(max_step(out, stopAt, static_cast<int>(out.size())) <= steadyStep * 1.02 &&
              max_curve(out, stopAt, static_cast<int>(out.size())) <= steadyCurve * 1.02,
          "no clicks during or after the stop (slope and curvature never exceed steady play)");
    bool falling = true;
    double prev = 1e9;
    for (int w = stopAt; w + 4800 <= halt; w += 4800) {
        double s = 0.0;
        for (int i = w; i < w + 4800; ++i) {
            s += out[static_cast<size_t>(i)] * out[static_cast<size_t>(i)];
        }
        s = std::sqrt(s / 4800.0);
        if (!(s <= prev * 1.15 + 1e-4)) {
            std::printf("  window at %.3f s: %.5f after %.5f\n", (w - stopAt) / kFs, s, prev);
        }
        falling = falling && s <= prev * 1.15 + 1e-4;
        prev = s;
    }
    check(falling, "level falls smoothly through the stop");
    const std::vector<double> fq = cycle_freqs(Buf(out.begin() + stopAt + 28800, out.begin() + stopAt + 33600), 0);
    std::printf("  pitch mid-stop: %.1f Hz cycles (220 Hz at speed)\n", fq.empty() ? 0.0 : fq[0]);

    // Start from the stop with a 0.4 s ramp.
    transport_tape_start(*rt, 0.4f);
    const int startAt = static_cast<int>(out.size());
    run(out, outR, nullptr, 48000);
    check(std::fabs(out[static_cast<size_t>(startAt)]) < 1e-3, "start ramp begins silent");
    std::printf("  steady step %.5f curve %.5f | start step %.5f curve %.5f\n", steadyStep, steadyCurve,
                max_step(out, startAt, static_cast<int>(out.size())), max_curve(out, startAt, static_cast<int>(out.size())));
    check(max_step(out, startAt, static_cast<int>(out.size())) <= steadyStep * 1.02 &&
              max_curve(out, startAt, static_cast<int>(out.size())) <= steadyCurve * 1.02,
          "no clicks during the start ramp");

    // Varispeed: glide to 0.5x, pitch follows.
    const int vsAt = static_cast<int>(out.size());
    transport_set_varispeed(*rt, 0.5f);
    run(out, outR, nullptr, 48000);
    std::printf("  varispeed step %.5f curve %.5f\n", max_step(out, vsAt, static_cast<int>(out.size())),
                max_curve(out, vsAt, static_cast<int>(out.size())));
    check(clean(out) && max_curve(out, vsAt, static_cast<int>(out.size())) <= steadyCurve * 1.02,
          "varispeed glide is smooth");
    const Buf seg(out.begin() + vsAt + 24000, out.begin() + vsAt + 48000);
    const double a500 = tone_amp(seg, 500.0, 0, 24000);
    const double a1k = tone_amp(seg, 1000.0, 0, 24000);
    std::printf("  at 0.5x: 500 Hz %.4f, 1 kHz %.4f\n", a500, a1k);
    check(a500 > 4.0 * a1k, "0.5x varispeed drops the overdubbed 1 kHz tone an octave");
    write_wav("chord_play_stop_start_varispeed.wav", out, outR);
    release_tracks(*rt);
}

void test_impulse(std::unique_ptr<TapeEngine>& e) {
    std::printf("\n-- impulse and silence tail\n");
    const int n = 48000 * 3;
    Buf in(static_cast<size_t>(n), 0.f);
    in[1000] = 0.9f;
    tape_engine_set_character(*e, character_row(1));
    TapeParams p = e->p;
    p.hissDb = -120.f;
    tape_engine_set_params(*e, p);
    tape_engine_reset(*e);
    Buf rec;
    Buf l;
    Buf r;
    record(*e, in, rec);
    play(*e, rec, l, r);
    check(clean(rec) && clean(l) && clean(r), "impulse then 3 s silence: finite, no denormals");
    write_wav("impulse_pocket.wav", l, r);
}

}  // namespace

int main() {
    const char* dir = std::getenv("LIFT_RENDER_DIR");
    g_dir = dir != nullptr ? dir : "lift-renders";
    std::filesystem::create_directories(g_dir);
    auto e = std::make_unique<TapeEngine>();
    tape_engine_init(*e);
    tape_engine_prepare(*e, kFs);
    test_drive_thd(e);
    test_speed_eq(e);
    test_wow_flutter(e);
    test_impulse(e);
    tape_engine_release(*e);
    test_transport();
    std::printf("\n%s (%d failed)\n", g_fails == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
