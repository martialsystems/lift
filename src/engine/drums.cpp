// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "engine/drums.h"

#include "shogun.h"

#include <cmath>

namespace lift::eng {

const char* const kDrumVoiceNames[kDrumVoices] = {"KICK", "KICK 2", "SNARE", "RIM", "CLAP", "CLAVE", "SHAKER",
                                                  "COWBELL", "CL HAT", "OP HAT", "CYMBAL", "LO TOM", "MID TOM",
                                                  "HI TOM"};

namespace {

using namespace shogun;

struct P {
    int id;
    float u;
};

// Voice -> the SHOGUN parameter the PITCH knob moves, and the DECAY knob's
// (up to two, moved together).
struct VoiceMap {
    int pitch;
    int decay[2];
};
const VoiceMap kMap[kDrumVoices] = {
    {P_BD1_TUNE, {P_BD1_DECAY, -1}},  {P_BD2_TUNE, {P_BD2_DECAY, -1}}, {P_SD_TUNE, {P_SD_SNDEC, P_SD_TDECAY}},
    {P_RS_TUNE, {-1, -1}},            {P_CP_FILTER, {P_CP_DECAY, -1}}, {P_CL_TUNE, {P_CL_DECAY, -1}},
    {-1, {P_MA_DECAY, -1}},           {P_CB_TUNE, {P_CB_DECAY, -1}},   {P_CH_TUNE, {P_CH_DECAY, -1}},
    {P_CH_TUNE, {P_OH_DECAY, -1}},    {P_CY_TUNE, {P_CY_DECAY, -1}},   {P_LTC_TUNE, {P_LTC_DECAY, -1}},
    {P_MTC_TUNE, {P_MTC_DECAY, -1}},  {P_HTC_TUNE, {P_HTC_DECAY, -1}},
};
const int kChokeParam[kDrumVoices] = {P_BD1_CHOKE, P_BD2_CHOKE, P_SD_CHOKE, P_RS_CHOKE, P_CP_CHOKE,
                                      P_CL_CHOKE,  P_MA_CHOKE,  P_CB_CHOKE, P_CH_CHOKE, P_OH_CHOKE,
                                      P_CY_CHOKE,  P_LTC_CHOKE, P_MTC_CHOKE, P_HTC_CHOKE};
const int kLevelParam[kDrumVoices] = {P_BD1_LEVEL, P_BD2_LEVEL, P_SD_LEVEL, P_RS_LEVEL, P_CP_LEVEL,
                                      P_CL_LEVEL,  P_MA_LEVEL,  P_CB_LEVEL, P_CH_LEVEL, P_OH_LEVEL,
                                      P_CY_LEVEL,  P_LTC_LEVEL, P_MTC_LEVEL, P_HTC_LEVEL};
const int kPanParam[kDrumVoices] = {P_BD1_PAN, P_BD2_PAN, P_SD_PAN, P_RS_PAN, P_CP_PAN, P_CL_PAN, P_MA_PAN,
                                    P_CB_PAN,  P_CH_PAN,  P_OH_PAN, P_CY_PAN, P_LTC_PAN, P_MTC_PAN, P_HTC_PAN};

// ---------------------------------------------------------------- kits
// Tuned by ear and by measurement (tests/drum_check.cpp prints the -40 dB
// decay of every voice). SHOGUN's noon decays (~340 ms to -40 dB on every
// voice) are far too long for the classic boxes, so every kit sets its own.
// DECAY law: tau = 8 ms * e^(4.5 u); -40 dB arrives after about 4.6 tau.

// KIT 1: the classic bridged-T box (808 style). Long-ish, tunable kick;
// tight snare, clap, hats.
const P k808[] = {
    {P_BD1_TUNE, 0.260f}, {P_BD1_PITCH, 0.100f}, {P_BD1_DECAY, 0.618f}, {P_BD1_ATTACK, 0.35f}, {P_BD1_NOISE, 0.0f},
    {P_BD1_DRIVE, 0.08f}, {P_BD1_LEVEL, 0.80f},
    {P_BD2_LEVEL, 0.0f},
    {P_SD_TUNE, 0.280f}, {P_SD_DETUNE, 0.62f}, {P_SD_PITCH, 0.10f}, {P_SD_TONE, 0.600f}, {P_SD_SNAPPY, 0.530f},
    {P_SD_TDECAY, 0.244f}, {P_SD_SNDEC, 0.204f}, {P_SD_LEVEL, 0.70f},
    {P_RS_TUNE, 0.45f}, {P_RS_LEVEL, 0.62f},
    {P_CP_ATTACK, 0.5f}, {P_CP_COUNT, 0.40f}, {P_CP_FILTER, 0.750f}, {P_CP_DECAY, 0.266f}, {P_CP_LEVEL, 0.72f},
    {P_CL_TUNE, 0.45f}, {P_CL_DECAY, 0.039f}, {P_CL_LEVEL, 0.60f},
    {P_MA_DECAY, 0.052f}, {P_MA_LEVEL, 0.62f},
    {P_CB_TUNE, 0.42f}, {P_CB_DECAY, 0.455f}, {P_CB_LEVEL, 0.55f},
    {P_CH_TUNE, 0.430f}, {P_CH_DECAY, 0.174f}, {P_CH_LEVEL, 0.70f}, {P_CH_CHOKE, 0.3f},
    {P_OH_DECAY, 0.600f}, {P_OH_LEVEL, 0.62f}, {P_OH_CHOKE, 0.3f},
    {P_CY_TUNE, 0.5f}, {P_CY_TONE, 0.55f}, {P_CY_DECAY, 0.68f}, {P_CY_LEVEL, 0.55f},
    {P_LTC_TUNE, 0.35f}, {P_LTC_DECAY, 0.48f}, {P_LTC_LEVEL, 0.62f},
    {P_MTC_TUNE, 0.45f}, {P_MTC_DECAY, 0.45f}, {P_MTC_LEVEL, 0.62f},
    {P_HTC_TUNE, 0.55f}, {P_HTC_DECAY, 0.42f}, {P_HTC_LEVEL, 0.62f},
    {P_CH_PAN, 0.42f}, {P_OH_PAN, 0.42f}, {P_CY_PAN, 0.6f}, {P_CB_PAN, 0.58f}, {P_CL_PAN, 0.62f},
    {P_LTC_PAN, 0.38f}, {P_MTC_PAN, 0.5f}, {P_HTC_PAN, 0.62f},
    {P_CP_SOUND, 0.000f},
};
// KIT 2: the VCO-kick box (909 style). Punchy kick with its click, brighter
// snare with more noise, tighter and brighter hats, a long crash.
const P k909[] = {
    {P_BD1_LEVEL, 0.0f},
    {P_BD2_TUNE, 0.310f}, {P_BD2_DECAY, 0.591f}, {P_BD2_TONE, 0.700f}, {P_BD2_LEVEL, 0.82f},
    {P_SD_TUNE, 0.600f}, {P_SD_DETUNE, 0.66f}, {P_SD_PITCH, 0.28f}, {P_SD_TONE, 0.780f}, {P_SD_SNAPPY, 0.700f},
    {P_SD_TDECAY, 0.416f}, {P_SD_SNDEC, 0.456f}, {P_SD_LEVEL, 0.70f},
    {P_RS_TUNE, 0.55f}, {P_RS_LEVEL, 0.60f},
    {P_CP_ATTACK, 0.6f}, {P_CP_COUNT, 0.40f}, {P_CP_FILTER, 0.770f}, {P_CP_DECAY, 0.220f}, {P_CP_LEVEL, 0.74f},
    {P_CL_TUNE, 0.55f}, {P_CL_DECAY, 0.014f}, {P_CL_LEVEL, 0.55f},
    {P_MA_DECAY, 0.040f}, {P_MA_LEVEL, 0.60f},
    {P_CB_TUNE, 0.50f}, {P_CB_DECAY, 0.417f}, {P_CB_LEVEL, 0.50f},
    {P_CH_TUNE, 0.880f}, {P_CH_DECAY, 0.228f}, {P_CH_LEVEL, 0.66f}, {P_CH_CHOKE, 0.3f},
    {P_OH_DECAY, 0.570f}, {P_OH_LEVEL, 0.60f}, {P_OH_CHOKE, 0.3f},
    {P_CY_TUNE, 0.62f}, {P_CY_TONE, 0.75f}, {P_CY_DECAY, 0.794f}, {P_CY_LEVEL, 0.52f},
    {P_LTC_TUNE, 0.300f}, {P_LTC_DECAY, 0.589f}, {P_LTC_LEVEL, 0.62f},
    {P_MTC_TUNE, 0.100f}, {P_MTC_DECAY, 0.479f}, {P_MTC_LEVEL, 0.62f},
    {P_HTC_TUNE, 0.000f}, {P_HTC_DECAY, 0.516f}, {P_HTC_LEVEL, 0.62f},
    {P_CH_PAN, 0.40f}, {P_OH_PAN, 0.40f}, {P_CY_PAN, 0.62f}, {P_CB_PAN, 0.58f}, {P_CL_PAN, 0.62f},
    {P_LTC_PAN, 0.36f}, {P_MTC_PAN, 0.5f}, {P_HTC_PAN, 0.64f},
    {P_CP_SOUND, 0.320f},
};
// Pitch drop at each hit, semitones (decays with the voice's bend time):
// kick and toms, fitted to the reference one-shots.
const float kBend[2][kDrumVoices] = {
    {5.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 2.f, 2.f, 2.f},
    {0.f, 26.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 4.f, 4.5f, 2.5f},
};
constexpr double kTomShift909 = -0.6;

struct KitDef {
    const P* p;
    int n;
};
const KitDef kKitDefs[kKits] = {
    {k808, static_cast<int>(sizeof k808 / sizeof k808[0])}, {k909, static_cast<int>(sizeof k909 / sizeof k909[0])},
    {k808, static_cast<int>(sizeof k808 / sizeof k808[0])}, {k909, static_cast<int>(sizeof k909 / sizeof k909[0])},
    {k808, static_cast<int>(sizeof k808 / sizeof k808[0])}, {k909, static_cast<int>(sizeof k909 / sizeof k909[0])},
    {k808, static_cast<int>(sizeof k808 / sizeof k808[0])}, {k909, static_cast<int>(sizeof k909 / sizeof k909[0])},
};

constexpr std::uint32_t steps(const char* s) {
    std::uint32_t m = 0;
    for (int i = 0; s[i] != 0 && i < 32; ++i) {
        if (s[i] == 'x') {
            m |= 1u << i;
        }
    }
    return m;
}

const KitInfo kInfo[kKits] = {
    {"808", {steps("x.....x...x.....") /*BD*/, 0, steps("....x.......x...") /*SD*/, 0, 0, 0, 0, 0,
             steps("x.x.x.x.x.x.x.x.") /*CH*/, 0, 0, 0, 0, 0}},
    {"909", {0, steps("x...x...x...x...") /*BD2*/, steps("....x.......x..x"), 0, steps("....x.......x..."), 0, 0,
             0, steps("x.x.x.x.x.x.x.x."), steps("..x...x...x...x."), 0, 0, 0, 0}},
    {"808 B", {}}, {"909 B", {}}, {"808 C", {}}, {"909 C", {}}, {"808 D", {}}, {"909 D", {}},
};

// Keyboard: C .. B of the first octave and C .. B of the second, voice order
// laid out so the white keys carry the core kit.
const int kKeyVoice[24] = {
    0,  1,  2, 3,  4,  5, 6,  8,  7, 9,  10, 11,  // C C# D D# E F F# G G# A A# B
    12, 13, 0, 2, 4, 8, 9, 10, -1, -1, -1, -1};

}  // namespace

int drumVoiceForKey(int key) noexcept { return key >= 0 && key < 24 ? kKeyVoice[key] : -1; }

const KitInfo& kitInfo(int kit) noexcept { return kInfo[kit < 0 || kit >= kKits ? 0 : kit]; }

DrumKnobs kitDefaultKnobs(int kit, int voice) noexcept {
    DrumKnobs k;
    const KitDef& d = kKitDefs[kit < 0 || kit >= kKits ? 0 : kit];
    for (int i = 0; i < d.n; ++i) {
        if (voice >= 0 && voice < kDrumVoices && d.p[i].id == kChokeParam[voice]) {
            k.choke = d.p[i].u;
        }
    }
    return k;
}

Drums::Drums() : e_(std::make_unique<shogun::Engine>()) {}
Drums::~Drums() = default;

void Drums::prepare(double fs) {
    e_->prepare(fs, 2);
    e_->setIdeal();
    e_->setRunning(false);
    e_->setSleepEnabled(true);
    for (int v = 0; v < kDrumVoices; ++v) {
        knobs_[v] = kitDefaultKnobs(kit_, v);
    }
    setKit(kit_);
}

void Drums::reset() noexcept { e_->reset(); }

namespace {
float kitValue(int kit, int id) noexcept {
    const KitDef& d = kKitDefs[kit];
    for (int i = 0; i < d.n; ++i) {
        if (d.p[i].id == id) {
            return d.p[i].u;
        }
    }
    return shogun::kParams[id].def;
}
float clamp01(float x) noexcept { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
}  // namespace

void Drums::setKit(int kit) noexcept {
    kit_ = kit < 0 || kit >= kKits ? 0 : kit;
    // SHOGUN's own INIT values first (noon), then the kit, then the knobs.
    for (int v = 0; v < kDrumVoices; ++v) {
        e_->setParamNow(kLevelParam[v], kitValue(kit_, kLevelParam[v]));
        e_->setParamNow(kPanParam[v], kitValue(kit_, kPanParam[v]));
    }
    const KitDef& d = kKitDefs[kit_];
    for (int i = 0; i < d.n; ++i) {
        e_->setParamNow(d.p[i].id, d.p[i].u);
    }
    // Per-kit voice shapes beyond SHOGUN's parameters (tools/drum_ref_analysis.py):
    // the 909 clap's quicker bursts, the 909 kick's fast pitch drop, the lower 909 toms.
    const bool is909 = kit_ % 2 == 1;
    e_->cp().setShape(is909 ? 0.008 : 0.0105, is909 ? 0.0022 : 0.003);
    e_->bd2().setBendTau(is909 ? 0.040 : 0.080);
    for (int t : {LTC, MTC, HTC}) {
        e_->tom(t).setShift(is909 ? kTomShift909 : 0.0);
    }
    for (int v = 0; v < kDrumVoices; ++v) {
        bend_[v] = kBend[is909 ? 1 : 0][v];
    }
    for (int v = 0; v < kDrumVoices; ++v) {
        applyVoice(v, true);
    }
}

void Drums::setKnobs(int voice, const DrumKnobs& k) noexcept {
    if (voice < 0 || voice >= kDrumVoices) {
        return;
    }
    knobs_[voice] = k;
    applyVoice(voice, false);
}

void Drums::applyVoice(int v, bool now) noexcept {
    const VoiceMap& m = kMap[v];
    const DrumKnobs& k = knobs_[v];
    auto set = [&](int id, float u) {
        if (now) {
            e_->setParamNow(id, u);
        } else {
            e_->setParam(id, u);
        }
    };
    if (m.pitch >= 0 && !(v == OH)) {  // OH shares CH's tune (one metal bank)
        set(m.pitch, clamp01(kitValue(kit_, m.pitch) + (k.pitch - 0.5f) * 0.8f + 0.2f * cvPitch_[v]));
    }
    for (int id : m.decay) {
        if (id >= 0) {
            set(id, clamp01(kitValue(kit_, id) + (k.decay - 0.5f) * 0.9f));
        }
    }
    set(kChokeParam[v], k.choke);
}

void Drums::setTranspose(int voice, float volts) noexcept {
    if (voice < 0 || voice >= kDrumVoices) {
        return;
    }
    volts = volts < -5.f ? -5.f : (volts > 5.f ? 5.f : volts);
    if (std::fabs(volts - cvPitch_[voice]) < 0.01f) {
        return;
    }
    cvPitch_[voice] = volts;
    applyVoice(voice, false);
}

void Drums::hit(int voice, float vel) noexcept {
    if (voice < 0 || voice >= kDrumVoices) {
        return;
    }
    const double volts = 5.0 * static_cast<double>(clamp01(vel));
    e_->trigger(voice, volts, bend_[voice], vel > 0.9f ? 3 : 2);
}

void Drums::render(float* L, float* R, int n, const std::uint32_t* hitMask, const float* vel) noexcept {
    for (int i = 0; i < n; ++i) {
        if (hitMask != nullptr && hitMask[i] != 0) {
            for (int v = 0; v < kDrumVoices; ++v) {
                if ((hitMask[i] >> v) & 1u) {
                    hit(v, vel != nullptr ? vel[v] : 1.f);
                }
            }
        }
        e_->processSample();
        L[i] = static_cast<float>(e_->mainL() * gain_);
        R[i] = static_cast<float>(e_->mainR() * gain_);
    }
}

float Drums::envelope(int voice) const noexcept {
    if (voice < 0 || voice >= kDrumVoices || !e_->voiceActive(voice)) {
        return 0.f;
    }
    return static_cast<float>(e_->voice(voice).env());
}

}  // namespace lift::eng
