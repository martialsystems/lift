// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

// Offline checks of the instrument DSP core (src/engine, no JUCE): every patch
// connection changes the audio, feedback loops stay bounded, the 808 / 909
// kits' decay times, the drum sequencer's sample accuracy, a rough aliasing
// check per synth engine, and CPU per voice / engine / effect. The tape here
// is a stand-in (a varispeed delay line with two heads); the real tape is
// checked through the plugin in tests/demo_render.cpp.

#include "engine/instrument.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace lift::eng;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

constexpr double kFs = 48000.0;

// Stand-in tape: always recording, head 1 reads 0.25 s behind the record head
// at the varispeed rate, head 2 another 0.25 s behind; BIAS darkens playback.
struct FakeTape final : TapeHost {
    std::vector<float> buf = std::vector<float>(1 << 18, 0.f);
    int w = 0;
    double r = -12000.0;
    float lp = 0.f;
    void tapeBlock(const float* srcL, const float* srcR, const float* speedCv, bool, bool reverse, const float* biasCv,
                   float* outL, float* outR, float* head1, float* head2, int n) noexcept override {
        const int mask = static_cast<int>(buf.size()) - 1;
        for (int i = 0; i < n; ++i) {
            const float m = 0.5f * (srcL[i] + srcR[i]);
            buf[static_cast<size_t>(w & mask)] = m;
            ++w;
            const double sp = (speedCv != nullptr ? std::exp2(std::clamp(speedCv[i], -36.f, 36.f) / 12.0) : 1.0);
            r += reverse ? -sp : sp;
            if (r > w - 64) r = w - 64;
            if (r < w - 100000) r = w - 100000;
            auto rd = [&](double p) {
                const double f = std::floor(p);
                const float a = buf[static_cast<size_t>(static_cast<int>(f) & mask)];
                const float b = buf[static_cast<size_t>((static_cast<int>(f) + 1) & mask)];
                return a + static_cast<float>(p - f) * (b - a);
            };
            const float b = biasCv != nullptr ? std::clamp(0.3f + biasCv[i] / 5.f, 0.01f, 1.f) : 0.3f;
            lp += b * (rd(r) - lp);
            head1[i] = lp;
            head2[i] = rd(r - 12000.0);
            outL[i] = 0.5f * srcL[i] + 0.5f * head1[i];
            outR[i] = 0.5f * srcR[i] + 0.5f * head1[i];
        }
    }
};

struct Rig {
    std::unique_ptr<Instrument> inst = std::make_unique<Instrument>();
    std::unique_ptr<std::atomic<std::uint32_t>[]> pats{new std::atomic<std::uint32_t>[kKits * kDrumVoices]};
    FakeTape tape;
    InstrumentCtl ctl;
    explicit Rig(int kit = 0) {
        inst->prepare(kFs);
        for (int k = 0; k < kKits; ++k)
            for (int v = 0; v < kDrumVoices; ++v) pats[static_cast<size_t>(k * kDrumVoices + v)].store(kitInfo(k).pattern[v]);
        inst->setPatterns(pats.get());
        ctl.running = true;
        ctl.kit = kit;
        inst->drums.setKit(kit);
    }
    void patch(const std::vector<std::pair<int, int>>& cables) {
        std::vector<int> o, i;
        for (auto c : cables) {
            o.push_back(c.first);
            i.push_back(c.second);
        }
        inst->setPatch(planPatch(o.data(), i.data(), static_cast<int>(o.size())));
    }
    // seconds of audio, stereo interleaved
    std::vector<float> run(double secs) {
        const int blocks = static_cast<int>(secs * kFs / kBlock);
        std::vector<float> out;
        out.reserve(static_cast<size_t>(blocks * kBlock * 2));
        float l[kBlock], r[kBlock];
        for (int b = 0; b < blocks; ++b) {
            inst->block(tape, ctl, l, r);
            for (int s = 0; s < kBlock; ++s) {
                out.push_back(l[s]);
                out.push_back(r[s]);
            }
        }
        return out;
    }
};

double rms(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return std::sqrt(s / std::max<size_t>(1, x.size()));
}
double diffRms(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) s += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
    return std::sqrt(s / std::max<size_t>(1, n));
}

// The scene the patch tests share: the 808 pattern running, a two-note chord
// held on LOOM (so B PITCH / GATE arpeggiate), the spring on.
std::vector<float> scene(const std::vector<std::pair<int, int>>& cables, double secs = 3.0) {
    Rig r(0);
    r.inst->synth.setEngine(0);
    const float m[4] = {0.35f, 0.5f, 0.48f, 0.55f};
    r.inst->synth.setMacros(m);
    r.patch(cables);
    r.inst->noteOn(60, 0.8f);
    r.inst->noteOn(67, 0.8f);
    return r.run(secs);
}

const char* const kOutN[kJacks] = {"A PITCH", "A GATE", "B PITCH", "B GATE", "DRUM", "CLOCK", "RESET", "RADIO",
                                   "LFO", "ENV", "S&H", "VCA", "SLEW", "QUANT", "HEAD 1", "HEAD 2"};
const char* const kInN[kJacks] = {"PITCH", "GATE", "FM", "CUTOFF", "SLICE", "FX MAC", "CLK", "RST",
                                  "VCA IN", "VCA CV", "SLEW", "QUANT", "S&H", "SPEED", "REVERSE", "BIAS"};

void patchChecks() {
    const auto base = scene({});
    const double ref = rms(base);
    check(ref > 0.01, "scene is audible (rms " + std::to_string(ref) + ")");
    struct T {
        std::vector<std::pair<int, int>> c;
    };
    const std::vector<T> tests = {
        {{{O_BPITCH, I_PITCH}}}, {{{O_BGATE, I_GATE}}}, {{{O_DRUM, I_SLICE}}},
        {{{O_LFO, I_CUTOFF}}}, {{{O_LFO, I_FM}}}, {{{O_LFO, I_PITCH}}}, {{{O_ENV, I_CUTOFF}}},
        {{{O_SH, I_PITCH}}}, {{{O_CLOCK, I_SH}, {O_SH, I_CUTOFF}}}, {{{O_LFO, I_FXMAC}}}, {{{O_BGATE, I_CLK}}},
        {{{O_BGATE, I_RST}}}, {{{O_RESET, I_RST}, {O_CLOCK, I_CLK}}}, {{{O_LFO, I_SPEED}}}, {{{O_LFO, I_BIAS}}},
        {{{O_LFO, I_REVERSE}}}, {{{O_LFO, I_VCAIN}, {O_VCA, I_CUTOFF}}}, {{{O_LFO, I_VCAIN}, {O_LFO, I_VCACV}, {O_VCA, I_FM}}},
        {{{O_SH, I_SLEW}, {O_SLEW, I_PITCH}}}, {{{O_LFO, I_QUANT}, {O_QUANT, I_PITCH}}}, {{{O_SH, I_QUANT}, {O_QUANT, I_PITCH}}},
        {{{O_HEAD1, I_FM}}}, {{{O_HEAD2, I_SPEED}}}, {{{O_HEAD1, I_CUTOFF}}}, {{{O_ENV, I_SPEED}}},
    };
    for (const auto& t : tests) {
        const auto a = scene(t.c);
        const double d = diffRms(a, base);
        std::string name;
        for (auto c : t.c) name += std::string(name.empty() ? "" : " + ") + kOutN[c.first] + " -> " + kInN[c.second];
        bool finite = true;
        for (float v : a) finite = finite && std::isfinite(v);
        check(finite && d > 0.02 * ref,
              name + " changes the audio (" + std::to_string(20.0 * std::log10(std::max(1e-9, d / ref))) + " dB vs the scene)");
    }
    // A PITCH -> PITCH is a straight wire by design (the keyboard's own pitch)
    check(diffRms(scene({{O_APITCH, I_PITCH}}), base) < 1e-3 * ref, "A PITCH -> PITCH is a unity wire (by design)");
    check(diffRms(scene({{O_AGATE, I_GATE}}), base) < 0.05 * ref,
          "A GATE -> GATE follows the keyboard's own gate (near unity by design)");
    {  // RADIO is the one placeholder: its jack is silent
        check(diffRms(scene({{O_RADIO, I_FM}}), base) < 1e-6, "RADIO -> FM: placeholder, silent (by design)");
    }
}

void feedbackChecks() {
    const std::vector<std::vector<std::pair<int, int>>> loops = {
        {{O_HEAD2, I_SPEED}},
        {{O_HEAD1, I_SPEED}, {O_HEAD2, I_BIAS}, {O_HEAD1, I_FXMAC}},
        {{O_HEAD1, I_VCAIN}, {O_VCA, I_SPEED}, {O_HEAD2, I_FM}, {O_HEAD1, I_CUTOFF}},
    };
    const char* names[] = {"HEAD 2 -> SPEED", "HEAD 1 -> SPEED, HEAD 2 -> BIAS, HEAD 1 -> FX MAC",
                           "HEAD 1 -> VCA -> SPEED, HEAD 2 -> FM, HEAD 1 -> CUTOFF"};
    for (size_t k = 0; k < loops.size(); ++k) {
        Rig r(0);
        r.patch(loops[k]);
        int fb = 0;
        for (int c = 0; c < r.inst->router.plan().count; ++c) fb += r.inst->router.plan().c[c].fb ? 1 : 0;
        r.inst->noteOn(48, 1.f);
        r.inst->noteOn(55, 1.f);
        const auto a = r.run(30.0);
        bool finite = true;
        float pk = 0.f;
        for (float v : a) {
            finite = finite && std::isfinite(v);
            pk = std::max(pk, std::fabs(v));
        }
        // last 5 s vs first 5 s: no runaway growth
        const size_t n5 = static_cast<size_t>(5.0 * kFs * 2);
        const std::vector<float> first(a.begin(), a.begin() + static_cast<long>(n5)), last(a.end() - static_cast<long>(n5), a.end());
        const double g = rms(last) / std::max(1e-9, rms(first));
        check(finite && pk < 4.f && g < 3.0 && fb >= 1,
              std::string("feedback ") + names[k] + ": 30 s, " + std::to_string(fb) + " delayed cable(s), peak " +
                  std::to_string(pk) + ", no NaN, end/start rms x" + std::to_string(g));
    }
}

// -40 dB decay time (ms) of one hit on the bare drum engine
double t40(Drums& d, int v) {
    d.reset();
    float l, r;
    for (int i = 0; i < 480; ++i) d.render(&l, &r, 1, nullptr, nullptr);
    d.hit(v, 1.f);
    const int n = static_cast<int>(kFs * 4);
    std::vector<float> b(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        d.render(&l, &r, 1, nullptr, nullptr);
        b[static_cast<size_t>(i)] = std::fabs(l) + std::fabs(r);
    }
    const float pk = *std::max_element(b.begin(), b.end());
    int last = 0;
    for (int i = 0; i < n; ++i)
        if (b[static_cast<size_t>(i)] > pk * 0.01f) last = i;
    return last / (kFs / 1000.0);
}

void drumChecks() {
    // ballparks for the -40 dB time (ms), bracketing the reference one-shots
    // measured by tools/drum_ref_analysis.py (808 / 909 hardware samples)
    struct Ref { int voice; double lo808, hi808, lo909, hi909; };
    const Ref refs[] = {
        {0, 350, 900, 0, 0},     {1, 0, 0, 300, 650},   {2, 90, 250, 200, 600},  {4, 100, 250, 80, 200},
        {5, 25, 80, 25, 80},     {6, 30, 90, 30, 90},   {7, 150, 350, 150, 300}, {8, 40, 110, 60, 140},
        {9, 300, 650, 350, 650}, {10, 700, 1600, 800, 1800}, {11, 250, 500, 350, 650}, {12, 220, 450, 230, 420},
        {13, 180, 400, 250, 480},
    };
    auto d = std::make_unique<Drums>();
    d->prepare(kFs);
    for (int kit = 0; kit < 2; ++kit) {
        d->setKit(kit);
        for (int v = 0; v < kDrumVoices; ++v) d->setKnobs(v, kitDefaultKnobs(kit, v));
        {  // let the 5 ms knob smoothing land
            float l, r;
            for (int i = 0; i < 4800; ++i) d->render(&l, &r, 1, nullptr, nullptr);
        }
        std::string line = std::string("KIT ") + std::to_string(kit + 1) + " (" + kitInfo(kit).name + ") -40 dB:";
        bool ok = true;
        for (const Ref& r : refs) {
            const double lo = kit == 0 ? r.lo808 : r.lo909, hi = kit == 0 ? r.hi808 : r.hi909;
            if (hi <= 0) continue;
            const double t = t40(*d, r.voice);
            line += std::string(" ") + kDrumVoiceNames[r.voice] + " " + std::to_string(static_cast<int>(t)) + "ms";
            if (t < lo || t > hi) {
                ok = false;
                line += "(!" + std::to_string(static_cast<int>(lo)) + "-" + std::to_string(static_cast<int>(hi)) + ")";
            }
        }
        check(ok, line);
    }
    // the DECAY knob controls the 808 kick: short to long
    d->setKit(0);
    DrumKnobs k = kitDefaultKnobs(0, 0);
    k.decay = 0.f;
    d->setKnobs(0, k);
    float l, r;
    for (int i = 0; i < 4800; ++i) d->render(&l, &r, 1, nullptr, nullptr);
    const double shortT = t40(*d, 0);
    k.decay = 1.f;
    d->setKnobs(0, k);
    for (int i = 0; i < 4800; ++i) d->render(&l, &r, 1, nullptr, nullptr);
    const double longT = t40(*d, 0);
    check(longT > 2.5 * shortT, "808 kick DECAY knob: " + std::to_string(static_cast<int>(shortT)) + " ms to " +
                                    std::to_string(static_cast<int>(longT)) + " ms");
}

void sequencerChecks() {
    // ticks land on exact sample positions: step = fs * 60 / bpm / 4
    for (double bpm : {120.0, 97.0, 174.0}) {
        Rig r(0);
        r.ctl.bpm = bpm;
        std::vector<std::int64_t> at;
        float l[kBlock], rr[kBlock];
        int prevTicks = 0;
        for (int b = 0; b < static_cast<int>(20.0 * kFs / kBlock); ++b) {
            r.inst->block(r.tape, r.ctl, l, rr);
            if (r.inst->ticks() != prevTicks) {
                at.push_back(r.inst->lastTickSample());
                prevTicks = r.inst->ticks();
            }
        }
        const double step = kFs * 60.0 / bpm / 4.0;
        double worst = 0.0;
        for (size_t i = 0; i < at.size(); ++i) {
            // the tick at index i (counting from the first) belongs at first + i * step, rounded
            const double want = static_cast<double>(at[0]) + std::floor(static_cast<double>(i) * step + 0.5);
            worst = std::max(worst, std::fabs(static_cast<double>(at[i]) - want));
        }
        check(at.size() > 50 && worst <= 1.0, "sequencer at " + std::to_string(static_cast<int>(bpm)) + " BPM: " +
                                                  std::to_string(at.size()) + " steps over 20 s, worst error " +
                                                  std::to_string(worst) + " samples (no drift)");
    }
}

// Rough aliasing check: one sustained note, Hann-windowed FFT, the largest
// spectral peak that is not near a harmonic of f0, relative to the strongest.
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void aliasChecks() {
    const char* names[8] = {"LOOM", "BEND", "FOLD", "RATIO", "WIRE", "SWARM", "SPOOL", "SPARE"};
    // harmonic engines with a clean spectrum at zero detune
    const float macros[4][4] = {{0.f, 0.9f, 0.f, 1.f}, {0.7f, 0.9f, 0.f, 1.f}, {0.6f, 0.5f, 0.f, 1.f}, {0.f, 0.f, 0.6f, 0.3f}};
    for (int e = 0; e < 4; ++e) {
        for (int note : {72, 96}) {
            auto s = std::make_unique<PolySynth>();
            s->prepare(kFs);
            s->setEngine(e);
            s->setMacros(macros[e]);
            s->noteOn(note, 1.f);
            const size_t N = 1 << 15;
            std::vector<float> x;
            float buf[kBlock];
            SynthCv cv;
            for (int b = 0; b < static_cast<int>((N + 9600) / kBlock) + 1; ++b) {
                s->render(buf, kBlock, cv);
                x.insert(x.end(), buf, buf + kBlock);
            }
            std::vector<std::complex<double>> a(N);
            for (size_t i = 0; i < N; ++i)
                a[i] = x[i + 9600] * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / (N - 1)));
            fft(a);
            const double f0 = 440.0 * std::exp2((note - 69) / 12.0);
            const double binHz = kFs / N;
            double top = 0.0, alias = 0.0;
            for (size_t k = 2; k < N / 2; ++k) {
                const double m = std::abs(a[k]);
                top = std::max(top, m);
                const double f = k * binHz;
                const double h = f / f0;
                const double off = std::fabs(h - std::floor(h + 0.5)) * f0;  // Hz from the nearest harmonic
                if (off > 6.0 * binHz && f > 30.0) alias = std::max(alias, m);
            }
            const double db = 20.0 * std::log10(std::max(1e-12, alias / top));
            check(db < -40.0, std::string(names[e]) + " note " + std::to_string(note) + " (" +
                                   std::to_string(static_cast<int>(f0)) + " Hz): worst non-harmonic spike " +
                                   std::to_string(static_cast<int>(std::round(db))) + " dB");
        }
    }
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void cpuReport() {
    const double secs = 10.0;
    const char* names[8] = {"LOOM", "BEND", "FOLD", "RATIO", "WIRE", "SWARM", "SPOOL", "SPARE"};
    std::vector<float> spool(96000);
    for (size_t i = 0; i < spool.size(); ++i) spool[i] = 0.3f * static_cast<float>(std::sin(i * 0.05));
    std::string line = "CPU (% of one core at 48 kHz) per synth voice:";
    for (int e = 0; e < 7; ++e) {
        auto s = std::make_unique<PolySynth>();
        s->prepare(kFs);
        s->setSpoolSource(spool.data(), spool.data(), static_cast<int>(spool.size()), 0, 0);
        s->setEngine(e);
        const float m[4] = {0.5f, 0.6f, 0.6f, 0.5f};
        s->setMacros(m);
        for (int k = 0; k < kSynthVoices; ++k) s->noteOn(48 + 4 * k, 0.9f);
        float buf[kBlock];
        SynthCv cv;
        const int blocks = static_cast<int>(secs * kFs / kBlock);
        const double t0 = nowMs();
        for (int b = 0; b < blocks; ++b) {
            if (b % 3000 == 0)
                for (int k = 0; k < kSynthVoices; ++k) s->noteOn(48 + 4 * k, 0.9f);
            s->render(buf, kBlock, cv);
        }
        const double pct = (nowMs() - t0) / (secs * 1000.0) * 100.0;
        char t[64];
        std::snprintf(t, sizeof t, " %s %.2f", names[e], pct / kSynthVoices);
        line += t;
    }
    std::printf("INFO  %s\n", line.c_str());
    {  // drums: the 808 pattern on the sequencer
        Rig r(0);
        const double t0 = nowMs();
        r.run(secs);
        // minus the scene's other parts is hard to split; report the whole instrument too
        const double whole = (nowMs() - t0) / (secs * 1000.0) * 100.0;
        auto d = std::make_unique<Drums>();
        d->prepare(kFs);
        float l[kBlock], rr[kBlock];
        std::uint32_t mask[kBlock] = {};
        float vel[kDrumVoices];
        std::fill(vel, vel + kDrumVoices, 1.f);
        const double t1 = nowMs();
        for (int b = 0; b < static_cast<int>(secs * kFs / kBlock); ++b) {
            mask[0] = (b % 94 == 0) ? 0x3fffu : 0u;  // every voice every ~60 ms
            d->render(l, rr, kBlock, mask, vel);
        }
        const double drums = (nowMs() - t1) / (secs * 1000.0) * 100.0;
        std::printf("INFO  CPU drums (all 14 voices retriggered): %.2f %%; whole instrument (808 pattern, spring, no notes): %.2f %%\n",
                    drums, whole);
    }
    std::string fxl = "CPU per effect:";
    for (int f = 0; f < kFxTypes; ++f) {
        auto fx = std::make_unique<FxRack>();
        fx->prepare(kFs);
        fx->setType(f);
        float p[4] = {kFxDefaults[f][0], kFxDefaults[f][1], kFxDefaults[f][2], 1.f};
        fx->setParams(p);
        float l[kBlock], r[kBlock];
        std::uint32_t rng = 1;
        const double t0 = nowMs();
        for (int b = 0; b < static_cast<int>(secs * kFs / kBlock); ++b) {
            for (int s = 0; s < kBlock; ++s) {
                rng = rng * 1664525u + 1013904223u;
                l[s] = r[s] = (b % 200 < 3) ? (static_cast<float>(rng >> 9) / 4194304.f - 1.f) * 0.5f : 0.f;
            }
            fx->process(l, r, kBlock, nullptr);
        }
        char t[64];
        std::snprintf(t, sizeof t, " %s %.2f%%", kFxNames[f], (nowMs() - t0) / (secs * 1000.0) * 100.0);
        fxl += t;
    }
    std::printf("INFO  %s\n", fxl.c_str());
}

void springChecks() {
    // the spring: a click gives a long dispersive tail with the chirp (energy
    // arriving low after high), TENSION moves it, a hard transient shakes it (boing)
    auto fx = std::make_unique<FxRack>();
    fx->prepare(kFs);
    fx->setType(FX_SPRING);
    float p[4] = {0.55f, 0.5f, 0.5f, 1.f};
    fx->setParams(p);
    float l[kBlock], r[kBlock];
    std::vector<float> out;
    for (int b = 0; b < static_cast<int>(3.0 * kFs / kBlock); ++b) {
        std::fill(l, l + kBlock, 0.f);
        std::fill(r, r + kBlock, 0.f);
        if (b == 10) l[0] = r[0] = 0.9f;
        fx->process(l, r, kBlock, nullptr);
        out.insert(out.end(), l, l + kBlock);
    }
    // 20 ms RMS windows: the tail is smeared, so judge its energy, not sample peaks
    const size_t W = 960;
    std::vector<double> env;
    for (size_t i = 0; i + W <= out.size(); i += W) {
        double e = 0.0;
        for (size_t j = 0; j < W; ++j) e += static_cast<double>(out[i + j]) * out[i + j];
        env.push_back(std::sqrt(e / W));
    }
    const double pk = *std::max_element(env.begin(), env.end());
    size_t last = 0;
    for (size_t i = 0; i < env.size(); ++i)
        if (env[i] > pk * 0.01) last = i;
    bool finite = true;
    for (float v : out) finite = finite && std::isfinite(v);
    const double ms = static_cast<double>(last * W) / 48.0;
    check(finite && pk > 1e-3 && ms > 900.0 && ms < 4000.0,
          "SPRING (DECAY 50 %): an impulse rings " + std::to_string(static_cast<int>(ms)) + " ms to -40 dB (energy), no NaN");
}

// Every effect in the rack: on a drum loop plus a held saw, at its default
// knobs, the output differs clearly from the dry, stays finite and bounded.
void fxChecks() {
    auto source = [](int i) {
        const double t = i / kFs;
        const double saw = 2.0 * std::fmod(t * 110.0, 1.0) - 1.0;
        const double hit = std::fmod(t, 0.5) < 0.02 ? std::sin(2.0 * 3.14159265 * 60.0 * t) * std::exp(-std::fmod(t, 0.5) * 80.0) : 0.0;
        return static_cast<float>(0.25 * saw + 0.6 * hit);
    };
    for (int f = 0; f < kFxTypes; ++f) {
        auto fx = std::make_unique<FxRack>();
        fx->prepare(kFs);
        fx->setType(f);
        fx->setParams(kFxDefaults[f]);
        float l[kBlock], r[kBlock];
        double d2 = 0.0, x2 = 0.0;
        float pk = 0.f;
        bool fin = true;
        int i0 = 0;
        for (int b = 0; b < static_cast<int>(3.0 * kFs / kBlock); ++b) {
            float dry[kBlock];
            for (int s = 0; s < kBlock; ++s) dry[s] = l[s] = r[s] = source(i0 + s);
            i0 += kBlock;
            fx->process(l, r, kBlock, nullptr);
            if (b * kBlock < kFs) continue;  // skip the first second
            for (int s = 0; s < kBlock; ++s) {
                d2 += (l[s] - dry[s]) * (l[s] - dry[s]);
                x2 += dry[s] * dry[s];
                pk = std::max(pk, std::max(std::fabs(l[s]), std::fabs(r[s])));
                fin = fin && std::isfinite(l[s]) && std::isfinite(r[s]);
            }
        }
        const double db = 10.0 * std::log10(d2 / std::max(1e-12, x2));
        check(fin && db > -26.0 && pk < 2.f, std::string("FX ") + kFxNames[f] + ": changes the sound (" +
                                                 std::to_string(static_cast<int>(std::round(db))) + " dB vs dry), peak " +
                                                 std::to_string(pk).substr(0, 4) + ", finite");
    }
    {  // COMP evens out a loud / quiet alternation
        auto fx = std::make_unique<FxRack>();
        fx->prepare(kFs);
        fx->setType(FX_COMP);
        const float p[4] = {0.8f, 0.2f, 0.3f, 1.f};
        fx->setParams(p);
        float l[kBlock], r[kBlock];
        double loud = 0, quiet = 0;
        for (int b = 0; b < static_cast<int>(4.0 * kFs / kBlock); ++b) {
            const bool hi = (b / 375) % 2 == 0;  // 0.25 s each
            for (int s = 0; s < kBlock; ++s) l[s] = r[s] = (hi ? 0.8f : 0.1f) * static_cast<float>(std::sin(0.05 * (b * kBlock + s)));
            fx->process(l, r, kBlock, nullptr);
            if (b % 375 > 200) {
                for (int s = 0; s < kBlock; ++s) (hi ? loud : quiet) += l[s] * l[s];
            }
        }
        const double range = 10.0 * std::log10(loud / quiet);
        check(range < 12.0, "COMP: an 18 dB loud/quiet swing comes out " + std::to_string(range).substr(0, 4) + " dB apart");
    }
    {  // DRIVE: antialiased saturation, a hot 3 kHz sine leaves few alias spikes
        auto fx = std::make_unique<FxRack>();
        fx->prepare(kFs);
        fx->setType(FX_DRIVE);
        const float p[4] = {0.7f, 1.f, 0.f, 1.f};
        fx->setParams(p);
        const size_t N = 1 << 15;
        std::vector<float> x;
        float l[kBlock], r[kBlock];
        int i0 = 0;
        while (x.size() < N + 4800) {
            for (int s = 0; s < kBlock; ++s) l[s] = r[s] = 0.8f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 3100.0 * (i0 + s) / kFs));
            i0 += kBlock;
            fx->process(l, r, kBlock, nullptr);
            x.insert(x.end(), l, l + kBlock);
        }
        std::vector<std::complex<double>> a(N);
        for (size_t i = 0; i < N; ++i) a[i] = x[i + 4800] * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (N - 1)));
        fft(a);
        double top = 0, alias = 0, worstHz = 0;
        for (size_t k = 2; k < N / 2; ++k) {
            const double m = std::abs(a[k]), f = k * kFs / N, h = f / 3100.0;
            top = std::max(top, m);
            if (std::fabs(h - std::floor(h + 0.5)) * 3100.0 > 30.0 && m > alias) {
                alias = m;
                worstHz = f;
            }
        }
        const double db = 20.0 * std::log10(alias / top);
        check(db < -40.0, "DRIVE (ADAA): 3.1 kHz driven hard, worst alias spike " + std::to_string(static_cast<int>(db)) + " dB (at " + std::to_string(static_cast<int>(worstHz)) + " Hz)");
    }
}

}  // namespace

int main() {
    patchChecks();
    feedbackChecks();
    drumChecks();
    sequencerChecks();
    aliasChecks();
    springChecks();
    fxChecks();
    cpuReport();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
