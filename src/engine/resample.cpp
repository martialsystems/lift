// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/resample.h"

#include <algorithm>
#include <cmath>

namespace lift::eng {

namespace {
constexpr double kPi = 3.14159265358979323846;
inline float dbToLin(float db) noexcept { return std::pow(10.f, db / 20.f); }
// Gated loudness from mean-square sub-blocks of 100 ms (400 ms windows, 75 % overlap).
float gatedLufs(const std::vector<double>& sub) {
    if (sub.empty()) return -200.f;
    std::vector<double> win;
    if (sub.size() < 4) {
        double m = 0.0;
        for (double v : sub) m += v;
        win.push_back(m / static_cast<double>(sub.size()));
    } else {
        for (size_t k = 0; k + 4 <= sub.size(); ++k) win.push_back(0.25 * (sub[k] + sub[k + 1] + sub[k + 2] + sub[k + 3]));
    }
    auto lufs = [](double ms) { return ms > 1e-20 ? -0.691 + 10.0 * std::log10(ms) : -200.0; };
    double sum = 0.0;
    int cnt = 0;
    for (double w : win) {
        if (lufs(w) > -70.0) {
            sum += w;
            ++cnt;
        }
    }
    if (cnt == 0) return -200.f;
    const double rel = lufs(sum / cnt) - 10.0;
    double s2 = 0.0;
    int c2 = 0;
    for (double w : win) {
        if (lufs(w) > -70.0 && lufs(w) > rel) {
            s2 += w;
            ++c2;
        }
    }
    return static_cast<float>(c2 > 0 ? lufs(s2 / c2) : -200.0);
}
}  // namespace

// ------------------------------------------------------------------ K-weighting

void KWeight::prepare(double fs) noexcept {
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        b[0][0] = (Vh + Vb * K / Q + K * K) / a0;
        b[0][1] = 2.0 * (K * K - Vh) / a0;
        b[0][2] = (Vh - Vb * K / Q + K * K) / a0;
        a[0][1] = 2.0 * (K * K - 1.0) / a0;
        a[0][2] = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        b[1][0] = 1.0;
        b[1][1] = -2.0;
        b[1][2] = 1.0;
        a[1][1] = 2.0 * (K * K - 1.0) / a0;
        a[1][2] = (1.0 - K / Q + K * K) / a0;
    }
    reset();
}

void KWeight::reset() noexcept {
    for (auto& s : z)
        for (auto& c : s)
            for (auto& v : c) v = 0.0;
}

float KWeight::tick(float x, int ch) noexcept {
    double y = x;
    for (int s = 0; s < 2; ++s) {
        double* q = z[s][ch];
        // transposed direct form II
        const double out = b[s][0] * y + q[0];
        q[0] = b[s][1] * y - a[s][1] * out + q[1];
        q[1] = b[s][2] * y - a[s][2] * out;
        y = out;
    }
    return static_cast<float>(y);
}

float integratedLufs(const float* l, const float* r, int n, double fs) {
    KWeight kw;
    kw.prepare(fs);
    const int blk = std::max(1, static_cast<int>(fs / 10.0));
    std::vector<double> sub;
    sub.reserve(static_cast<size_t>(n / blk + 1));
    double acc = 0.0;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const double a = kw.tick(l[i], 0), b = kw.tick(r[i], 1);
        acc += a * a + b * b;
        if (++k == blk) {
            sub.push_back(acc / blk);
            acc = 0.0;
            k = 0;
        }
    }
    if (k > blk / 2 || sub.empty()) sub.push_back(k > 0 ? acc / k : 0.0);
    return gatedLufs(sub);
}

namespace {
// 4x oversampling for true peak (BS.1770-4 annex 2 style): 3 interpolating
// phases of a 16-tap Hann-windowed sinc.
struct TpFir {
    float h[3][16];
    TpFir() {
        for (int p = 0; p < 3; ++p) {
            const double frac = (p + 1) / 4.0;
            double sum = 0.0;
            for (int k = 0; k < 16; ++k) {
                const double t = (k - 7) - frac;  // taps at x[i-7 .. i+8], centred between i and i+1
                const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double w = 0.5 + 0.5 * std::cos(kPi * t / 8.5);
                h[p][k] = static_cast<float>(sinc * w);
                sum += sinc * w;
            }
            for (int k = 0; k < 16; ++k) h[p][k] = static_cast<float>(h[p][k] / sum);
        }
    }
};
const TpFir& tpFir() {
    static const TpFir f;
    return f;
}
inline float tpAt(const float* x, int n, int i) noexcept {
    // the largest of the sample and its three interpolated neighbours towards i + 1
    float pk = std::fabs(x[i]);
    if (i < 7 || i + 8 >= n) return pk;
    const TpFir& f = tpFir();
    for (int p = 0; p < 3; ++p) {
        float y = 0.f;
        for (int k = 0; k < 16; ++k) y += f.h[p][k] * x[i - 7 + k];
        pk = std::max(pk, std::fabs(y));
    }
    return pk;
}
}  // namespace

float truePeak(const float* l, const float* r, int n) noexcept {
    float pk = 0.f;
    for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(tpAt(l, n, i), tpAt(r, n, i)));
    return pk;
}

namespace {
// Offline look-ahead limiter: 1 ms attack ahead of each peak, 50 ms release.
void limitTo(float* l, float* r, int n, double fs, float ceil) {
    const int la = std::max(1, static_cast<int>(0.001 * fs));
    std::vector<float> need(static_cast<size_t>(n), 1.f);
    for (int i = 0; i < n; ++i) {
        const float p = std::max(tpAt(l, n, i), tpAt(r, n, i));
        if (p > ceil) need[static_cast<size_t>(i)] = ceil / p;
    }
    // look-ahead: each sample's gain is the smallest need within the next la samples, ramped
    std::vector<float> g(static_cast<size_t>(n), 1.f);
    for (int i = n - 1; i >= 0; --i) {
        float m = need[static_cast<size_t>(i)];
        if (i + 1 < n) m = std::min(m, g[static_cast<size_t>(i + 1)] + (1.f - g[static_cast<size_t>(i + 1)]) / static_cast<float>(la));
        g[static_cast<size_t>(i)] = m;
    }
    const float rel = 1.f - std::exp(-1.f / static_cast<float>(0.05 * fs));
    float e = 1.f;
    for (int i = 0; i < n; ++i) {
        const float t = g[static_cast<size_t>(i)];
        e = t < e ? t : e + rel * (t - e);
        l[i] *= e;
        r[i] *= e;
        l[i] = std::clamp(l[i], -ceil, ceil);
        r[i] = std::clamp(r[i], -ceil, ceil);
    }
}
}  // namespace

float trimToTarget(float* l, float* r, int n, double fs, float targetLufs, float ceilingDbtp) {
    if (n <= 0) return 0.f;
    float total = 0.f;
    // two passes: the limiter takes a little loudness off loud material
    for (int pass = 0; pass < 3; ++pass) {
        const float lu = integratedLufs(l, r, n, fs);
        if (lu < -69.f) return total;  // silence: leave it
        const float gDb = std::clamp(targetLufs - lu, -40.f, 40.f);
        if (std::fabs(gDb) < 0.05f && pass > 0) break;
        const float g = dbToLin(gDb);
        for (int i = 0; i < n; ++i) {
            l[i] *= g;
            r[i] *= g;
        }
        total += gDb;
        const float ceil = dbToLin(ceilingDbtp);
        if (truePeak(l, r, n) > ceil) limitTo(l, r, n, fs, ceil * 0.995f);
    }
    return total;
}

// ------------------------------------------------------------------ capture

void CaptureRing::prepare(double fs, double seconds) {
    fs_ = fs;
    cap_ = static_cast<int>(fs * seconds);
    l_.assign(static_cast<size_t>(cap_), 0.f);
    r_.assign(static_cast<size_t>(cap_), 0.f);
    blockLen_ = static_cast<int>(fs / 10.0);
    blocks_.assign(static_cast<size_t>(seconds * 10.0) + 4, 0.f);
    kw_.prepare(fs);
    reset();
}

void CaptureRing::reset() noexcept {
    written_.store(0);
    nBlocks_.store(0);
    acc_ = 0.0;
    accN_ = 0;
    kw_.reset();
}

void CaptureRing::write(const float* l, const float* r, int n) noexcept {
    if (cap_ <= 0) return;
    std::int64_t w = written_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(w % cap_);
        l_[k] = l[i];
        r_[k] = r[i];
        ++w;
        const double a = kw_.tick(l[i], 0), b = kw_.tick(r[i], 1);
        acc_ += a * a + b * b;
        if (++accN_ == blockLen_) {
            const std::int64_t nb = nBlocks_.load(std::memory_order_relaxed);
            blocks_[static_cast<size_t>(nb % static_cast<std::int64_t>(blocks_.size()))] = static_cast<float>(acc_ / blockLen_);
            nBlocks_.store(nb + 1, std::memory_order_release);
            acc_ = 0.0;
            accN_ = 0;
        }
    }
    written_.store(w, std::memory_order_release);
}

int CaptureRing::read(float* l, float* r, int n, std::int64_t endFrame) const noexcept {
    const std::int64_t w = endFrame < 0 ? written() : std::min(endFrame, written());
    n = static_cast<int>(std::min<std::int64_t>({static_cast<std::int64_t>(n), w, static_cast<std::int64_t>(cap_)}));
    const std::int64_t from = w - n;
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>((from + i) % cap_);
        l[i] = l_[k];
        r[i] = r_[k];
    }
    return n;
}

float CaptureRing::loudness(double seconds) const noexcept {
    const std::int64_t nb = nBlocks_.load(std::memory_order_acquire);
    const int want = std::max(1, std::min(static_cast<int>(seconds * 10.0), static_cast<int>(blocks_.size()) - 2));
    const int have = static_cast<int>(std::min<std::int64_t>(nb, want));
    if (have <= 0) return -200.f;
    std::vector<double> sub;
    sub.reserve(static_cast<size_t>(have));
    for (int k = 0; k < have; ++k) {
        sub.push_back(blocks_[static_cast<size_t>((nb - have + k) % static_cast<std::int64_t>(blocks_.size()))]);
    }
    return gatedLufs(sub);
}

// ------------------------------------------------------------------ selection

int nearestZero(const Clip& c, int frame, int radius) noexcept {
    const int n = c.frames();
    if (n < 2) return std::clamp(frame, 0, std::max(0, n));
    frame = std::clamp(frame, 0, n - 1);
    auto m = [&](int i) { return c.l[static_cast<size_t>(i)] + c.r[static_cast<size_t>(i)]; };
    for (int d = 0; d <= radius; ++d) {
        for (int s : {frame - d, frame + d}) {
            if (s >= 1 && s < n && ((m(s - 1) <= 0.f && m(s) > 0.f) || (m(s - 1) >= 0.f && m(s) < 0.f))) return s;
        }
    }
    return frame;
}

int firstOnset(const Clip& c) noexcept {
    const int n = c.frames();
    float pk = 0.f;
    for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(c.l[static_cast<size_t>(i)]) + std::fabs(c.r[static_cast<size_t>(i)]));
    const float thr = 0.0316f * pk;  // -30 dB
    for (int i = 0; i < n; ++i) {
        if (std::fabs(c.l[static_cast<size_t>(i)]) + std::fabs(c.r[static_cast<size_t>(i)]) > thr) return std::max(0, i - 48);
    }
    return 0;
}

int snapFrame(const Clip& c, int frame, int snap, double beatFrames) noexcept {
    const int n = c.frames();
    frame = std::clamp(frame, 0, n);
    switch (snap) {
    case SNAP_ZERO: return nearestZero(c, frame);
    case SNAP_HIT: {
        // the nearest onset at or after the frame (a jump in the 5 ms envelope)
        const int hop = 240;
        float prev = 0.f;
        for (int i = std::max(0, frame - hop); i + hop < n; i += hop) {
            float e = 0.f;
            for (int k = 0; k < hop; ++k) e = std::max(e, std::fabs(c.l[static_cast<size_t>(i + k)]));
            if (i >= frame && e > 2.f * prev + 1e-4f) return nearestZero(c, i, 120);
            prev = e;
        }
        return frame;
    }
    case SNAP_BEAT:
    case SNAP_BAR: {
        const double q = snap == SNAP_BEAT ? beatFrames : 4.0 * beatFrames;
        if (q < 1.0) return frame;
        return std::clamp(static_cast<int>(std::lround(frame / q) * q), 0, n);
    }
    default: return frame;
    }
}

Selection defaultSelection(const Clip& c, double fs) {
    Selection s;
    s.snap = SNAP_ZERO;
    const int n = c.frames();
    if (n <= 0) return s;
    if (c.loop) {
        s.start = nearestZero(c, 0, 240);
        const int end = nearestZero(c, n - 1, 240);
        s.length = std::max(1, end - s.start);
        return s;
    }
    // HIT: the first onset to where the sound falls 40 dB under its peak (+ 20 ms)
    const int on = firstOnset(c);
    float pk = 0.f;
    for (int i = on; i < n; ++i) pk = std::max(pk, std::fabs(c.l[static_cast<size_t>(i)]) + std::fabs(c.r[static_cast<size_t>(i)]));
    int last = n - 1;
    const float thr = 0.01f * pk;
    for (int i = n - 1; i > on; --i) {
        if (std::fabs(c.l[static_cast<size_t>(i)]) + std::fabs(c.r[static_cast<size_t>(i)]) > thr) {
            last = i;
            break;
        }
    }
    last = std::min(n - 1, last + static_cast<int>(0.02 * fs));
    s.start = nearestZero(c, on, 120);
    const int end = nearestZero(c, last, 240);
    s.length = std::max(1, end - s.start);
    return s;
}

std::shared_ptr<const Clip> cutSelection(const Clip& c, const Selection& s) {
    auto o = std::make_shared<Clip>();
    const int n = c.frames();
    const int a = std::clamp(s.start, 0, n), len = std::clamp(s.length, 0, n - a);
    o->l.assign(c.l.begin() + a, c.l.begin() + a + len);
    o->r.assign(c.r.begin() + a, c.r.begin() + a + len);
    const int fade = std::min(96, len / 2);
    for (int i = 0; i < fade; ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(fade);
        o->l[static_cast<size_t>(i)] *= g;
        o->r[static_cast<size_t>(i)] *= g;
        o->l[static_cast<size_t>(len - 1 - i)] *= g;
        o->r[static_cast<size_t>(len - 1 - i)] *= g;
    }
    o->loop = c.loop;
    o->lufsIn = c.lufsIn;
    o->gainDb = c.gainDb;
    o->id = c.id;
    return o;
}

// ------------------------------------------------------------------ master limiter

void MasterLimiter::prepare(double fs) noexcept {
    rel_ = std::exp(-1.f / static_cast<float>(0.1 * fs));
    reset();
}

void MasterLimiter::reset() noexcept {
    g_ = 1.f;
    w_ = 0;
    for (int i = 0; i < kLook; ++i) dl_[i] = dr_[i] = 0.f, pk_[i] = 1.f;
}

void MasterLimiter::process(float* l, float* r, int n) noexcept {
    float minG = 1.f;
    for (int i = 0; i < n; ++i) {
        const float p = std::max(std::fabs(l[i]), std::fabs(r[i]));
        const float need = p > ceil_ ? ceil_ / p : 1.f;
        const float xl = dl_[w_], xr = dr_[w_];
        dl_[w_] = std::isfinite(l[i]) ? l[i] : 0.f;
        dr_[w_] = std::isfinite(r[i]) ? r[i] : 0.f;
        pk_[w_] = need;
        w_ = (w_ + 1) % kLook;
        float m = 1.f;
        for (int k = 0; k < kLook; ++k) m = std::min(m, pk_[k]);
        // attack: reach the window's minimum within the look-ahead; release 100 ms
        if (m < g_) {
            g_ += (m - g_) * 0.25f;
        } else {
            g_ = m + (g_ - m) * rel_;
        }
        float yl = xl * g_, yr = xr * g_;
        yl = std::clamp(yl, -ceil_, ceil_);
        yr = std::clamp(yr, -ceil_, ceil_);
        l[i] = yl;
        r[i] = yr;
        minG = std::min(minG, g_);
    }
    gr_ = minG < 1.f ? -20.f * std::log10(minG) : 0.f;
}

}  // namespace lift::eng
