// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#pragma once
// TripleShaper: the shared west-coast style triple wave shaper (SHOGUN_Redesign.md 4.6, ORIGAMI_Proposal.md 1).
// Header-only C++17, no plugin or framework dependencies. SHOGUN's WAVE and the ORIGAMI effect both use THIS file,
// so bug fixes and tests land once.
//
// ---- Math (exactly SHOGUN spec v2.2 4.6) ---------------------------------------------------------------------
//   Units: x = volts / 5 (one shaper unit = 5 V Jidai = 2.5 V in the +-2.5 V modular convention). Call toUnits()/toVolts().
//   Stage i (i = 1..3), amount a in [0,1], symmetry b in [-1,1]:
//     g = 1 + K_i*a,  K = (4, 2, 2)
//     theta(x) = (pi/2)*g*(x + b)
//     s(x) = sin theta(x) - sin theta(0)              static bias removed: s(0) = 0
//     y = (1 - a)*x + a*s(x)                          a = 0 -> y = x exactly, for ANY b
//   First-order ADAA per stage, in double, with the CURRENT sample's (a, b) for both terms:
//     F(x) = (1 - a)*x^2/2 + a*( -(2/(pi*g))*cos theta(x) - x*sin theta(0) )
//     y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1]);   |dx| < 1e-6 -> f((x[n] + x[n-1])/2)
//   A stage is skipped (a true wire, no ADAA half-sample delay; its x[n-1] still updates) when a = 0 for the WHOLE
//   block, whatever its symmetry b (1.1.2: at a = 0 the stage law is y = x, so AMT 0 is transparent; before 1.1.2 a
//   stage also needed b = 0 and otherwise ran ADAA on a straight line, which is a two-sample average: half a sample
//   late and -6 dB at 10 kHz for three such stages at 48 kHz). The caller plans each block with
//   TripleShaper::planBlock (controls, steady, vcLive). A stage whose a merely passes through 0 on some samples
//   (modulation, a live VC with AMT depth, a smoothing ramp on an amount control) keeps its ADAA, so the half-sample
//   delay never toggles mid-stream. Without a plan no stage is skipped.
//   Group delay: every stage that runs (not a wire) adds the ADAA's half sample at the rate it runs at (0.5, 1.0 or
//   1.5 samples for 1, 2 or 3 stages; a quarter sample per stage at the base rate when it runs at 2x).
//   TripleShaper::groupDelay() gives it for the current plan. It depends on WAVE and is fractional, so it is NOT
//   latency to report to a host (reporting it would change the host latency while a knob turns); a wet/dry mix
//   that wants exact alignment must treat it as part of the wet path's tone.
//   Macro m in [0,1] (front WAVE knob), staggered:
//     c1 = clamp(2m), c2 = clamp(2m - 0.5), c3 = clamp(2m - 1)
//     a_i = clamp(c_i + trim_i + mod_i + vc*vcToAmt_i, 0, 1)
//     b_i = clamp(sym_i + modSym_i + vc*vcToSym_i, -1, 1)
//   vc is the audio-rate VC in shaper units (volts/5): read per sample, never smoothed (FOLD VC / ORIGAMI VC 1-3).
//   WAVE 0 with all amount trims at 0 (and no VC > AMT depth on a live source) is a true bypass, whatever the SYM
//   settings (1.1.2): isBypass() tells the caller to skip the stages and LEVEL COMP so the output is bit-identical.
//
// ---- API ---------------------------------------------------------------------------------------------------
//   namespace jidai::dsp
//   ShaperStage::f (x, a, b, K) / ShaperStage::F (x, a, b, K)       static transfer function and antiderivative
//   AdaaStage            one stage with ADAA state:  double process (x, a, b, K);  bool adaa = true;  reset()
//   macroAmounts (m, c[3])
//   ShaperControls       { macro, trim[3], sym[3], vcToAmt[3], vcToSym[3] } (+ modAmt[3], modSym[3] for SHOGUN's matrix)
//   ShaperControls::isBypass (vcLive)   true -> skip everything (bit-exact bypass)
//   TripleShaper         three AdaaStages in series:  planBlock (ctl, steady, vcLive) once per block (groupDelay(),
//                        runningStages() for the plan), then
//                        double process (x, const ShaperControls&, double vc)
//                        (one VC for every stage, SHOGUN) or process (x, ctl, const double vc[3]) (one VC per
//                        stage, ORIGAMI); processStages (x, a[3], b[3]); stageParams (...); setAdaa (bool); reset()
//   LevelComp            detector-ratio LEVEL COMP: C = sqrt(<x^2>/<y^2>), 20 ms detectors, clamp +-12 dB,
//                        5 ms gain smoothing:  prepare (fs);  double process (in, wet);  track (in) while bypassed.
//                        Both detectors are floored at 1e-30 (no 0/0 on silence). That floor is a deliberate
//                        deviation from a bare ratio: SHOGUN measured it moving its output by at most 9.9e-7.
//   DcBlocker            TPT one-pole high-pass, 8 Hz by default:  prepare (fs, hz);  double process (x)
//   toUnits (volts) = volts/5,  toVolts (units) = units*5
// Not here (SHOGUN-only): SHAPE from resonator quadrature, PRE-VCA routing, the voice envelope.

#include <cmath>
#include <functional>

namespace jidai::dsp {

inline constexpr double kPi = 3.14159265358979323846;
// Intentional exact comparison (caches and bit-exact paths); std::equal_to keeps -Wfloat-equal builds quiet.
inline bool same (double a, double b) noexcept { return std::equal_to<double>{} (a, b); }
inline constexpr double kShaperK[3] = { 4.0, 2.0, 2.0 };
inline constexpr double kVoltsPerUnit = 5.0;

constexpr double toUnits (double volts) noexcept { return volts / kVoltsPerUnit; }
constexpr double toVolts (double units) noexcept { return units * kVoltsPerUnit; }

constexpr double clamp01 (double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
constexpr double clampSym (double v) noexcept { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }

struct ShaperStage
{
    static double f (double x, double a, double b, double K) noexcept
    {
        const double g = 1.0 + K * a;
        const double th0 = 0.5 * kPi * g * b;
        return (1.0 - a) * x + a * (std::sin (0.5 * kPi * g * (x + b)) - std::sin (th0));
    }
    static double F (double x, double a, double b, double K) noexcept
    {
        const double g = 1.0 + K * a;
        const double th0 = 0.5 * kPi * g * b;
        return (1.0 - a) * x * x * 0.5 + a * (-(2.0 / (kPi * g)) * std::cos (0.5 * kPi * g * (x + b)) - x * std::sin (th0));
    }
};

class AdaaStage
{
public:
    bool adaa = true;
    // Set per block by TripleShaper::planBlock: a is 0 on every sample of this block, so the stage is a wire (y = x for
    // any b).
    bool wire = false;

    void reset() noexcept { xPrev_ = 0.0; cacheValid_ = false; }

    double process (double x, double a, double b, double K) noexcept
    {
        if (wire)
        {
            if (same (a, 0.0))
            {
                xPrev_ = x;          // a true wire; keep the history so ADAA restarts cleanly
                cacheValid_ = false;
                return x;
            }
            wire = false;            // the plan was wrong (a control moved off 0 mid-block): run ADAA from here on
        }
        if (! adaa)
        {
            xPrev_ = x;
            cacheValid_ = false;
            return ShaperStage::f (x, a, b, K);
        }
        const double d = x - xPrev_;
        double y;
        if (std::fabs (d) < 1e-6)
        {
            y = ShaperStage::f (0.5 * (x + xPrev_), a, b, K);
            cacheValid_ = false;
        }
        else
        {
            // F(x[n-1]) with the CURRENT parameters; reuse last sample's F(x) when the parameters did not move.
            const double Fp = (cacheValid_ && same (a, aPrev_) && same (b, bPrev_)) ? Fprev_ : ShaperStage::F (xPrev_, a, b, K);
            const double Fx = ShaperStage::F (x, a, b, K);
            y = (Fx - Fp) / d;
            Fprev_ = Fx;
            cacheValid_ = true;
        }
        aPrev_ = a;
        bPrev_ = b;
        xPrev_ = x;
        return y;
    }

private:
    double xPrev_ = 0.0, Fprev_ = 0.0, aPrev_ = 0.0, bPrev_ = 0.0;
    bool cacheValid_ = false;
};

inline void macroAmounts (double m, double c[3]) noexcept
{
    c[0] = clamp01 (2.0 * m);
    c[1] = clamp01 (2.0 * m - 0.5);
    c[2] = clamp01 (2.0 * m - 1.0);
}

struct ShaperControls
{
    double macro = 0.0;               // WAVE, 0..1
    double trim[3] { 0, 0, 0 };       // STAGE 1-3 (per-stage amount trims), -1..1
    double sym[3] { 0, 0, 0 };        // per-stage SYM (ORIGAMI adds its global SYM here), -1..1
    double vcToAmt[3] { 0, 0, 0 };    // VC -> AMT depth, -1..1
    double vcToSym[3] { 0, 0, 0 };    // VC -> SYM depth, -1..1
    double modAmt[3] { 0, 0, 0 };     // control-rate matrix sums (SHOGUN), 0 for ORIGAMI
    double modSym[3] { 0, 0, 0 };

    // vcLive: a VC source is connected (jack patched or an internal source selected). Symmetry (sym, modSym, vcToSym)
    // does not count: with every amount at 0 each stage is y = x (1.1.2).
    bool isBypass (bool vcLive) const noexcept
    {
        if (! same (macro, 0.0)) return false;
        for (int i = 0; i < 3; ++i)
        {
            if (! same (trim[i], 0.0) || ! same (modAmt[i], 0.0)) return false;
            if (vcLive && ! same (vcToAmt[i], 0.0)) return false;
        }
        return true;
    }
};

class TripleShaper
{
public:
    void reset() noexcept { for (auto& s : st_) { s.reset(); s.wire = false; } }
    void setAdaa (bool on) noexcept { for (auto& s : st_) s.adaa = on; }

    // Once per block, before its first sample (a stage is skipped when its amount is 0 for the whole block).
    // c: the block's controls without VC. steady: the amount inputs (macro, trim, modAmt) do not change during the
    // block (no smoothing ramp, no per-sample modulation); symmetry may move, it has no effect at a = 0.
    // vcLive[i]: a VC source drives stage i. Stage i is a wire for the block when steady, its a is exactly 0, and no
    // live VC has AMT depth on it (VC > SYM depth alone keeps the wire).
    void planBlock (const ShaperControls& c, bool steady, const bool vcLive[3]) noexcept
    {
        double m[3];
        macroAmounts (c.macro, m);
        for (int i = 0; i < 3; ++i)
        {
            const double a = clamp01 (m[i] + c.trim[i] + c.modAmt[i]);
            const bool vcDrives = vcLive[i] && ! same (c.vcToAmt[i], 0.0);
            st_[i].wire = steady && same (a, 0.0) && ! vcDrives;
        }
    }
    void planBlock (const ShaperControls& c, bool steady, bool vcLive) noexcept
    {
        const bool live[3] = { vcLive, vcLive, vcLive };
        planBlock (c, steady, live);
    }
    bool stageIsWire (int i) const noexcept { return i >= 0 && i < 3 && st_[i].wire; }
    // Stages that run ADAA under the current plan, and their group delay in samples at the rate the shaper runs at
    // (0.5 per stage). Not host latency: see the header comment.
    int runningStages() const noexcept { return (st_[0].wire ? 0 : 1) + (st_[1].wire ? 0 : 1) + (st_[2].wire ? 0 : 1); }
    double groupDelay() const noexcept { return 0.5 * (double) runningStages(); }

    static void stageParams (const ShaperControls& c, double vc, double a[3], double b[3]) noexcept
    {
        double m[3];
        macroAmounts (c.macro, m);
        for (int i = 0; i < 3; ++i)
        {
            a[i] = clamp01 (m[i] + c.trim[i] + c.modAmt[i] + vc * c.vcToAmt[i]);
            b[i] = clampSym (c.sym[i] + c.modSym[i] + vc * c.vcToSym[i]);
        }
    }

    // Per-stage VC (ORIGAMI's VC 1 / VC 2 / VC 3 each drive their own stage).
    static void stageParams (const ShaperControls& c, const double vc[3], double a[3], double b[3]) noexcept
    {
        double m[3];
        macroAmounts (c.macro, m);
        for (int i = 0; i < 3; ++i)
        {
            a[i] = clamp01 (m[i] + c.trim[i] + c.modAmt[i] + vc[i] * c.vcToAmt[i]);
            b[i] = clampSym (c.sym[i] + c.modSym[i] + vc[i] * c.vcToSym[i]);
        }
    }

    // x and vc in shaper units. Stages run in series 1 -> 2 -> 3. One VC for all stages (SHOGUN FOLD VC).
    double process (double x, const ShaperControls& c, double vc) noexcept
    {
        double a[3], b[3];
        stageParams (c, vc, a, b);
        return processStages (x, a, b);
    }
    // One VC per stage (ORIGAMI).
    double process (double x, const ShaperControls& c, const double vc[3]) noexcept
    {
        double a[3], b[3];
        stageParams (c, vc, a, b);
        return processStages (x, a, b);
    }
    // Explicit stage parameters (already clamped).
    double processStages (double x, const double a[3], const double b[3]) noexcept
    {
        for (int i = 0; i < 3; ++i)
            x = st_[i].process (x, a[i], b[i], kShaperK[i]);
        return x;
    }

private:
    AdaaStage st_[3];
};

// LEVEL COMP: holds the wet RMS at the input RMS (SHOGUN 4.6, ORIGAMI 2).
class LevelComp
{
public:
    void prepare (double fs) noexcept
    {
        fs = fs > 0.0 ? fs : 48000.0;
        aDet_ = 1.0 - std::exp (-1.0 / (0.020 * fs));
        aGain_ = 1.0 - std::exp (-1.0 / (0.005 * fs));
        reset();
    }
    void reset() noexcept { px_ = py_ = 1e-12; gain_ = 1.0; }

    double gainFor (double inSq, double wetSq) noexcept
    {
        px_ += (inSq - px_) * aDet_;
        py_ += (wetSq - py_) * aDet_;
        if (px_ < 1e-30) px_ = 1e-30;
        if (py_ < 1e-30) py_ = 1e-30;
        double g = std::sqrt (px_ / py_);
        g = g < kMin ? kMin : (g > kMax ? kMax : g);
        gain_ += (g - gain_) * aGain_;
        return gain_;
    }
    double process (double in, double wet) noexcept { return wet * gainFor (in * in, wet * wet); }
    // While the shaper is bypassed the wet equals the input: keep the detectors running so C eases back to 1.
    void track (double in) noexcept { (void) gainFor (in * in, in * in); }
    double gain() const noexcept { return gain_; }

    static constexpr double kMax = 3.9810717055349722;   // +12 dB
    static constexpr double kMin = 0.25118864315095801;  // -12 dB

private:
    double aDet_ = 0.001, aGain_ = 0.004, px_ = 1e-12, py_ = 1e-12, gain_ = 1.0;
};

// TPT one-pole high-pass (SHOGUN 4.2): G = g/(1+g); v = (x - s)G; lp = v + s; s = lp + v; hp = x - lp.
class DcBlocker
{
public:
    void prepare (double fs, double hz = 8.0) noexcept
    {
        const double g = std::tan (kPi * hz / (fs > 0.0 ? fs : 48000.0));
        G_ = g / (1.0 + g);
        s_ = 0.0;
    }
    void reset() noexcept { s_ = 0.0; }
    double process (double x) noexcept
    {
        const double v = (x - s_) * G_;
        const double lp = v + s_;
        s_ = lp + v;
        if (std::fabs (s_) < 1e-30) s_ = 0.0;
        return x - lp;
    }

private:
    double G_ = 0.0005, s_ = 0.0;
};

} // namespace jidai::dsp
