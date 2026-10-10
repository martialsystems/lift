// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// LIFT's resampling core, framework free.
//
//  CaptureRing   always on: the last 60 s of what you hear (the master after
//                the safety limiter), written by the audio thread one block
//                at a time; it also keeps the K-weighted energy of every
//                100 ms, so the pre-keep meter and the keep's loudness cost
//                nothing on the audio thread.
//  keep          (message thread) LIFT: the last N seconds; REC: one pass.
//                Every keep runs the automatic trim stage: integrated loudness
//                (ITU-R BS.1770 K-weighting, -70 LUFS absolute and -10 LU
//                relative gates) re-levelled to kTargetLufs, then a true-peak
//                safety limiter (4x interpolated peaks, ceiling kCeilingDbtp).
//                So keep -> play -> keep never drifts (tests/resample_check).
//  Selection     start / length (frames) and snap (off / zero crossing / hit /
//                beat / bar). The fast path: a keep comes with its default
//                selection (a short sound = the HIT: onset to tail; a loop =
//                the whole LOOP), snapped to zero crossings, so DROP without
//                opening SELECT places it at once.
//  MasterLimiter one block (32 samples) of look-ahead, ceiling -1 dBFS,
//                100 ms release; real time safe.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace lift::eng {

constexpr float kTargetLufs = -16.f;
constexpr float kCeilingDbtp = -1.f;

// BS.1770 K-weighting (pre-filter shelf + RLB high-pass), any sample rate.
struct KWeight {
    void prepare(double fs) noexcept;
    float tick(float x, int ch) noexcept;
    void reset() noexcept;
    double b[2][3] = {}, a[2][3] = {};
    double z[2][2][2] = {};  // [stage][channel][state]
};

// Integrated loudness (LUFS) of a stereo buffer; -inf (-200) for silence.
float integratedLufs(const float* l, const float* r, int n, double fs);
// True-peak estimate (linear) with 4x interpolation between samples.
float truePeak(const float* l, const float* r, int n) noexcept;
// The trim stage: gain to kTargetLufs, then the true-peak limiter. Returns
// the gain applied (dB). In place.
float trimToTarget(float* l, float* r, int n, double fs, float targetLufs = kTargetLufs, float ceilingDbtp = kCeilingDbtp);

class CaptureRing {
public:
    void prepare(double fs, double seconds);  // allocates
    void reset() noexcept;
    // audio thread
    void write(const float* l, const float* r, int n) noexcept;
    // message thread: the newest n frames (n <= capacity) into dst
    int read(float* l, float* r, int n, std::int64_t endFrame = -1) const noexcept;
    std::int64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
    int capacity() const noexcept { return cap_; }
    double rate() const noexcept { return fs_; }
    // short-term (last `seconds`) K-weighted loudness from the 100 ms blocks, LUFS
    float loudness(double seconds) const noexcept;

private:
    double fs_ = 48000.0;
    int cap_ = 0;
    std::vector<float> l_, r_;
    std::atomic<std::int64_t> written_{0};
    KWeight kw_;
    double acc_ = 0.0;
    int accN_ = 0, blockLen_ = 4800;
    std::vector<float> blocks_;  // mean square per 100 ms
    std::atomic<std::int64_t> nBlocks_{0};
};

enum Snap : int { SNAP_OFF, SNAP_ZERO, SNAP_HIT, SNAP_BEAT, SNAP_BAR, kSnaps };
const char* const kSnapNames[kSnaps] = {"OFF", "ZERO", "HIT", "BEAT", "BAR"};

struct Clip {
    std::vector<float> l, r;
    int frames() const noexcept { return static_cast<int>(l.size()); }
    float lufsIn = -200.f;   // measured before the trim
    float gainDb = 0.f;      // what the trim applied
    bool loop = false;       // kept from a loop (whole-loop default) or a hit
    int id = 0;
};

struct Selection {
    int start = 0, length = 0;
    int snap = SNAP_ZERO;
    int chop = 1;  // keys 1-8: the selection cut into this many slices
    bool operator==(const Selection& o) const noexcept {
        return start == o.start && length == o.length && snap == o.snap && chop == o.chop;
    }
};

// The default selection (the SELECT fast path).
Selection defaultSelection(const Clip& c, double fs);
// Snap a frame to the clip under a snap mode (beatFrames for BEAT / BAR).
int snapFrame(const Clip& c, int frame, int snap, double beatFrames) noexcept;
// The nearest zero crossing (mono sum) within +-radius frames.
int nearestZero(const Clip& c, int frame, int radius = 480) noexcept;
// The first onset (a jump above -30 dB of the clip peak).
int firstOnset(const Clip& c) noexcept;

// Copy a selection (with 2 ms edge fades) into a new clip.
std::shared_ptr<const Clip> cutSelection(const Clip& c, const Selection& s);

class MasterLimiter {
public:
    void prepare(double fs) noexcept;
    void reset() noexcept;
    // kLook samples of look-ahead; in place, n <= 64
    void process(float* l, float* r, int n) noexcept;
    float gainReduction() const noexcept { return gr_; }  // dB, last block
    static constexpr int kLook = 32;

private:
    float ceil_ = 0.891f;  // -1 dBFS
    float rel_ = 0.9998f;
    float g_ = 1.f;
    float dl_[kLook] = {}, dr_[kLook] = {};
    float pk_[kLook] = {};
    int w_ = 0;
    float gr_ = 0.f;
};

}  // namespace lift::eng
