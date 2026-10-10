// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// The resampling core (src/engine/resample.h, clipvoice.h, no JUCE): loudness
// measurement, the automatic trim before every keep, no level drift over keep
// -> process -> keep cycles, the capture ring, default selections (the SELECT
// fast path), the master safety limiter, clip voices, and the dry sources
// (synth engines, drum kits) level-matched.

#include "engine/instrument.h"
#include "engine/resample.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace lift::eng;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
constexpr double kFs = 48000.0;
std::string db(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

void loudnessChecks() {
    // BS.1770: a full-scale 1 kHz sine in one channel reads -3.01 LUFS
    const int n = static_cast<int>(kFs * 3);
    std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n), 0.f);
    for (int i = 0; i < n; ++i) l[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 * i / kFs));
    const float lu = integratedLufs(l.data(), r.data(), n, kFs);
    check(std::fabs(lu + 3.01f) < 0.1f, "1 kHz 0 dBFS one channel = -3.01 LUFS (" + db(lu) + ")");
    // silence is gated away
    std::vector<float> z(static_cast<size_t>(n), 0.f);
    check(integratedLufs(z.data(), z.data(), n, kFs) < -150.f, "silence reads -inf");
    // true peak catches an inter-sample peak (a sine at fs/4, phase 45 degrees)
    std::vector<float> t(64);
    for (int i = 0; i < 64; ++i) t[static_cast<size_t>(i)] = static_cast<float>(std::sin(3.14159265358979 * (0.5 * i + 0.25)));
    const float tp = truePeak(t.data(), t.data(), 64);
    check(tp > 0.97f, "true peak sees the inter-sample peak (" + db(tp) + " vs sample peak 0.707)");
}

// a "process": gain, a tone change and a little drive, like a pass through the FX
void process(std::vector<float>& l, std::vector<float>& r, float gainDb, float lp, float drive) {
    const float g = std::pow(10.f, gainDb / 20.f);
    float zl = 0.f, zr = 0.f;
    for (size_t i = 0; i < l.size(); ++i) {
        zl += lp * (l[i] - zl);
        zr += lp * (r[i] - zr);
        l[i] = std::tanh(drive * g * zl) / drive;
        r[i] = std::tanh(drive * g * zr) / drive;
    }
}

void trimChecks() {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    const int n = static_cast<int>(kFs * 4);
    // a drum-ish source: noise bursts with decays, every 250 ms
    std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float e = std::exp(-static_cast<float>(i % 12000) / 1500.f);
        l[static_cast<size_t>(i)] = 0.3f * e * nd(rng);
        r[static_cast<size_t>(i)] = 0.3f * e * nd(rng);
    }
    for (float lvl : {-30.f, -12.f, 0.f, 12.f}) {
        auto a = l, b = r;
        const float g = std::pow(10.f, lvl / 20.f);
        for (size_t i = 0; i < a.size(); ++i) a[i] *= g, b[i] *= g;
        trimToTarget(a.data(), b.data(), n, kFs);
        const float lu = integratedLufs(a.data(), b.data(), n, kFs);
        const float tp = 20.f * std::log10(truePeak(a.data(), b.data(), n));
        check(std::fabs(lu - kTargetLufs) < 0.5f && tp <= kCeilingDbtp + 0.2f,
              "trim from " + db(lvl) + " dB: " + db(lu) + " LUFS, true peak " + db(tp) + " dBTP");
    }
    // no drift: keep -> process -> keep, five times
    auto a = l, b = r;
    trimToTarget(a.data(), b.data(), n, kFs);
    const float first = integratedLufs(a.data(), b.data(), n, kFs);
    float worst = 0.f;
    const float gains[5] = {-9.f, 6.f, -3.f, 10.f, -14.f};
    for (int k = 0; k < 5; ++k) {
        process(a, b, gains[k], 0.35f + 0.1f * k, 1.f + 0.5f * k);
        trimToTarget(a.data(), b.data(), n, kFs);
        const float lu = integratedLufs(a.data(), b.data(), n, kFs);
        worst = std::max(worst, std::fabs(lu - first));
    }
    check(worst < 1.f, "5 x (process, keep): the level stays within 1 dB (worst " + db(worst) + " dB)");
}

void captureChecks() {
    CaptureRing c;
    c.prepare(kFs, 60.0);
    std::vector<float> l(32), r(32);
    std::int64_t w = 0;
    const int blocks = static_cast<int>(70.0 * kFs / 32);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 32; ++i, ++w) {
            l[static_cast<size_t>(i)] = static_cast<float>(w % 1000) / 1000.f * 0.2f;
            r[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 997.0 * w / kFs));
        }
        c.write(l.data(), r.data(), 32);
    }
    std::vector<float> ol(48000), orr(48000);
    const int got = c.read(ol.data(), orr.data(), 48000);
    bool same = got == 48000;
    for (int i = 0; i < got && same; ++i) {
        const std::int64_t f = w - 48000 + i;
        same = std::fabs(ol[static_cast<size_t>(i)] - static_cast<float>(f % 1000) / 1000.f * 0.2f) < 1e-6f;
    }
    check(same, "capture ring: 70 s written, the last second reads back exactly");
    check(c.capacity() == static_cast<int>(60 * kFs), "capture ring holds 60 s");
    std::vector<float> bl(static_cast<size_t>(3 * kFs)), br(bl.size());
    c.read(bl.data(), br.data(), static_cast<int>(bl.size()));
    const float direct = integratedLufs(bl.data(), br.data(), static_cast<int>(bl.size()), kFs);
    const float meter = c.loudness(3.0);
    check(std::fabs(direct - meter) < 0.5f, "pre-keep meter (100 ms blocks) " + db(meter) + " = measured " + db(direct) + " LUFS");
}

void selectionChecks() {
    Clip hit;
    const int n = static_cast<int>(kFs * 2);
    hit.l.assign(static_cast<size_t>(n), 0.f);
    hit.r.assign(static_cast<size_t>(n), 0.f);
    const int on = 24000;
    for (int i = on; i < n; ++i) {
        const float e = std::exp(-static_cast<float>(i - on) / 2400.f);
        const float x = e * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 110.0 * (i - on) / kFs + 0.3));
        hit.l[static_cast<size_t>(i)] = hit.r[static_cast<size_t>(i)] = x;
    }
    const Selection s = defaultSelection(hit, kFs);
    const bool zeroAt = s.start > 0 && ((hit.l[static_cast<size_t>(s.start - 1)] <= 0.f) != (hit.l[static_cast<size_t>(s.start)] <= 0.f));
    check(std::abs(s.start - on) < 480 && zeroAt, "HIT default: starts at the onset (" + std::to_string(s.start - on) +
                                                    " frames) on a zero crossing");
    check(s.length > 9000 && s.length < 40000, "HIT default: covers the decay, not the silence (" + std::to_string(s.length) + " frames)");
    Clip loop = hit;
    loop.loop = true;
    const Selection ls = defaultSelection(loop, kFs);
    check(ls.start < 480 && ls.start + ls.length > n - 480, "LOOP default: the whole loop");
    auto cut = cutSelection(hit, s);
    check(cut->frames() == s.length && std::fabs(cut->l[0]) < 1e-6f, "the cut has the selection's length and starts from silence");
    check(snapFrame(hit, 30000, SNAP_BEAT, 24000.0) == 24000, "snap BEAT rounds to the beat");
    check(snapFrame(hit, 20000, SNAP_HIT, 24000.0) >= on - 240 && snapFrame(hit, 20000, SNAP_HIT, 24000.0) < on + 480,
          "snap HIT finds the next hit");
}

void limiterChecks() {
    MasterLimiter m;
    m.prepare(kFs);
    float pk = 0.f;
    bool finite = true;
    std::vector<float> l(32), r(32);
    for (int b = 0; b < 3000; ++b) {
        for (int i = 0; i < 32; ++i) {
            const double t = (b * 32 + i) / kFs;
            l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(4.0 * std::sin(2.0 * 3.14159265358979 * 220.0 * t));
        }
        if (b == 1500) l[3] = r[3] = 1e30f;
        m.process(l.data(), r.data(), 32);
        for (float v : l) {
            finite = finite && std::isfinite(v);
            pk = std::max(pk, std::fabs(v));
        }
    }
    check(finite && pk <= 0.8913f + 1e-4f, "master safety limiter: +12 dB in, peak " + db(20.0 * std::log10(pk)) + " dBFS out, no NaN");
    // quiet material passes untouched (after the look-ahead)
    MasterLimiter q;
    q.prepare(kFs);
    std::vector<float> a(32), b2(32);
    float err = 0.f;
    std::vector<float> hist;
    for (int b = 0; b < 100; ++b) {
        for (int i = 0; i < 32; ++i) a[static_cast<size_t>(i)] = b2[static_cast<size_t>(i)] = 0.25f * static_cast<float>(std::sin(0.01 * (b * 32 + i)));
        std::vector<float> in = a;
        q.process(a.data(), b2.data(), 32);
        if (b > 0) {
            for (int i = 0; i < 32; ++i) err = std::max(err, std::fabs(a[static_cast<size_t>(i)] - hist[static_cast<size_t>(i)]));
        }
        hist = in;
    }
    check(err < 1e-6f, "the limiter is transparent below the ceiling (one block late)");
}

void clipVoiceChecks() {
    ClipPlayer p;
    p.prepare(kFs);
    std::vector<float> c(4800, 0.5f);
    p.trigger(c.data(), c.data(), 4800, 2.0, 1.f, 60, true);
    std::vector<float> L(32), R(32);
    int len = 0;
    for (int b = 0; b < 400 && p.active(); ++b) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        p.render(L.data(), R.data(), 32);
        len += 32;
    }
    check(std::abs(len - 2400) <= 64, "a clip voice at rate 2 plays in half the time (" + std::to_string(len) + " frames)");
}

// The dry sources sit at one loudness: every synth engine for the same chord,
// every drum kit for its pattern (trims in synth.cpp / drums.cpp).
float synthLufs(int engine) {
    auto inst = std::make_unique<Instrument>();
    inst->prepare(kFs);
    inst->synth.setEngine(engine);
    std::vector<float> spool(static_cast<size_t>(kFs * 2));
    for (size_t i = 0; i < spool.size(); ++i) spool[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 261.63 * i / kFs));
    inst->synth.setSpoolSource(spool.data(), spool.data(), static_cast<int>(spool.size()), 0, 0);
    const float m[4] = {0.35f, 0.62f, 0.48f, 0.55f};
    inst->synth.setMacros(m);
    std::vector<float> l, r;
    float buf[kBlock];
    SynthCv cv;
    for (int b = 0; b < static_cast<int>(3.0 * kFs / kBlock); ++b) {
        const double t = b * kBlock / kFs;
        if (b % static_cast<int>(0.5 * kFs / kBlock) == 0 && t < 2.5) {
            inst->synth.noteOn(48 + (b / 750) % 3 * 7, 0.8f);
            inst->synth.noteOn(60, 0.8f);
        }
        if (b % static_cast<int>(0.5 * kFs / kBlock) == 600) inst->synth.noteOff(-1);
        inst->synth.render(buf, kBlock, cv);
        for (float v : buf) l.push_back(v), r.push_back(v);
    }
    return integratedLufs(l.data(), r.data(), static_cast<int>(l.size()), kFs);
}

float kitLufs(int kit) {
    struct NoTape final : TapeHost {
        void transportBlock(float*, int) noexcept override {}
        void headBlock(int, const float*, bool, const float*, float* h, int n) noexcept override { std::fill(h, h + n, 0.f); }
        void inputBlock(const float*, const float*, float* l, float* r, int n) noexcept override {
            std::fill(l, l + n, 0.f);
            std::fill(r, r + n, 0.f);
        }
        void mixBlock(const TapeMixIo& io, int n) noexcept override {
            for (int i = 0; i < n; ++i) io.outL[i] = io.srcL[i], io.outR[i] = io.srcR[i], io.send[i] = 0.f;
        }
    } tape;
    auto inst = std::make_unique<Instrument>();
    inst->prepare(kFs);
    std::unique_ptr<std::atomic<std::uint32_t>[]> pats{new std::atomic<std::uint32_t>[kKits * kDrumVoices]};
    for (int k = 0; k < kKits; ++k)
        for (int v = 0; v < kDrumVoices; ++v) pats[static_cast<size_t>(k * kDrumVoices + v)].store(kitInfo(k % 2).pattern[v]);
    inst->setPatterns(pats.get());
    inst->drums.setKit(kit);
    InstrumentCtl ctl;
    ctl.running = true;
    ctl.kit = kit;
    ctl.fxOn = false;
    std::vector<float> l, r;
    float bl[kBlock], br[kBlock];
    for (int b = 0; b < static_cast<int>(8.0 * kFs / kBlock); ++b) {
        inst->block(tape, ctl, bl, br);
        for (int i = 0; i < kBlock; ++i) l.push_back(bl[i]), r.push_back(br[i]);
    }
    return integratedLufs(l.data(), r.data(), static_cast<int>(l.size()), kFs);
}

void drySourceChecks(bool calibrate) {
    // dry target: 1 LU under the keep target (LOOM's level), so a keep of a dry source is a small lift
    const float target = kTargetLufs - 1.f;
    std::string line = "synth engines dry:";
    float worst = 0.f;
    for (int e = 0; e < SPARE; ++e) {
        if (calibrate) PolySynth::kEngineTrim[e] = 1.f;
        const float lu = synthLufs(e);
        line += " " + std::to_string(e) + ":" + db(lu);
        if (calibrate) std::printf("  engine %d raw %.2f LUFS -> trim %.4f\n", e, lu, std::pow(10.f, (target - lu) / 20.f));
        worst = std::max(worst, std::fabs(lu - target));
    }
    check(calibrate || worst < 1.5f, line + " LUFS (target " + db(target) + ", worst " + db(worst) + " LU off)");
    line = "drum kits dry:";
    worst = 0.f;
    for (int k = 0; k < kKits; ++k) {
        if (calibrate) Drums::kKitTrim[k] = 1.0;
        const float lu = kitLufs(k);
        line += " " + std::to_string(k) + ":" + db(lu);
        if (calibrate) std::printf("  kit %d raw %.2f LUFS -> trim %.4f\n", k, lu, std::pow(10.f, (target - lu) / 20.f));
        worst = std::max(worst, std::fabs(lu - target));
    }
    check(calibrate || worst < 1.5f, line + " LUFS (target " + db(target) + ", worst " + db(worst) + " LU off)");
}

}  // namespace

int main(int argc, char** argv) {
    const bool calibrate = argc > 1 && std::strcmp(argv[1], "--calibrate") == 0;
    loudnessChecks();
    trimChecks();
    captureChecks();
    selectionChecks();
    limiterChecks();
    clipVoiceChecks();
    drySourceChecks(calibrate);
    std::printf(failures == 0 ? "ALL PASS (0 failures)\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
