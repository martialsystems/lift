#pragma once
// LEAD (OTA SVF) and BASS (ZDF ladder): PolyBLEP saw/square, glide in log2, filter env, RC VCA, accent (§7).
#include <jidai/jcs/Pitch.h>

#include "common.h"

namespace shogun {

struct SynthVoice : Voice {
  explicit SynthVoice(int id) : id_(id) {
    const bool lead = id == LEAD;
    pCut = lead ? P_LEAD_CUTOFF : P_BASS_CUTOFF;
    pReso = lead ? P_LEAD_RESO : P_BASS_RESO;
    pEnv = lead ? P_LEAD_ENV : P_BASS_ENV;
    pDecay = lead ? P_LEAD_DECAY : P_BASS_DECAY;
    pSqr = lead ? P_LEAD_SQR : P_BASS_SQR;
    pOct = lead ? P_LEAD_OCT : P_BASS_OCT;
    pTune = lead ? P_LEAD_TUNE : P_BASS_TUNE;
    pGlide = lead ? P_LEAD_GLIDE : P_BASS_GLIDE;
    pAcc = lead ? P_LEAD_ACCENT : P_BASS_ACCENT;
  }
  int id_, pCut, pReso, pEnv, pDecay, pSqr, pOct, pTune, pGlide, pAcc;
  PolyBlepOsc osc;
  OtaSvf ota;
  ZdfLadder ladder;
  RcEnv ef, vca;
  // Pitch state. noteTarget/noteGlided are in semitones including OCT (they drive NOTE OUT, §7.5);
  // TUNE, A4, V/OCT and tolerance are applied only to the audible frequency.
  double noteTarget = 48.0, noteGlided = 48.0, aGlide = 0.0;
  double vOct = 0.0, cutoffOct = 0.0, acc = 0.0, accAmt = 0.5, a4 = 440.0;
  double f = jidai::jcs::pitch::kC3Hz, fc = 1000.0, k = 0.0, R = 0.5, envAmt = 0.5, last = 0.0;
  bool gate = false, sqr = false, glideNext = false, over = false;
  int oct = 0;
  Memo2 mGlide_;
  Memo1 mPitch_, mTau_, mCut_, mEnv_, mAcc_;  // per-voice coefficient memos (bit-exact, see dsp.h Memo1)

  void prepare(double fs, double fsE) override {
    Voice::prepare(fs, fsE);
    ef.setAttack(0.0015, fsE);
    vca.setAttack(0.001, fsE);
    reset();
  }
  void reset() override {
    osc.reset(0.0);
    ota.reset();
    ladder.reset();
    ef.reset();
    vca.reset();
    gate = false;
    last = 0.0;
  }
  // Base-rate control: glide (log2 domain = semitone domain / 12, τ_g = 0.002·e^{5.5u}), filter laws.
  void control(const VoiceCtx& c) override {
    const double* ue = c.ue;
    aGlide = mGlide_(ue[pGlide], fs_, [](double u, double fs) { return rcCoef(glideTau(u), fs); });
    noteGlided = noteGlided + (1.0 - aGlide) * (noteTarget - noteGlided);
    if (std::fabs(noteTarget - noteGlided) < 1e-9) noteGlided = noteTarget;
    const double tuneCents = 100.0 * bip(ue[pTune]);
    // JCS R4: note 48 = 0 V = C3 = the shared kC3Hz (exact 55·2^(15/12)) at A4 440; A4 scales it (R4.6 receiver side).
    f = jidai::jcs::pitch::kC3Hz * (a4 / 440.0) * mPitch_((noteGlided - 48.0) / 12.0 + tuneCents / 1200.0 + vOct, [](double x) { return std::exp2(x); }) * c.tolPitch;
    envAmt = ue[pEnv];
    accAmt = ue[pAcc];
    ef.setDecay(mTau_(ue[pDecay], decayTau) * c.tolTau, fsE_);
    if (id_ == LEAD) {
      R = 1.0 - 0.985 * ue[pReso];
    } else {
      k = 4.2 * ue[pReso];
    }
  }
  double cutoffNow(const double* ue, double tolCut) {
    const double accOct = acc * accAmt * ef.e;  // accent: +1 oct of filter env · ACCENT
    const double eo = mEnv_(envAmt * 6.0 * ef.e + accOct + cutoffOct, [](double x) { return std::exp2(x); });
    if (id_ == LEAD) return 200.0 * mCut_(ue[pCut], [](double u) { return std::pow(40.0, u); }) * eo * tolCut;
    return 80.0 * mCut_(ue[pCut], [](double u) { return std::pow(50.0, u); }) * eo * tolCut;
  }
  // Note on from the sequencer, the GATE/NOTE jacks or MIDI. tie: glide, no retrigger (§10.2 TIE).
  void noteOn(const VoiceCtx& c, double note, double accIn, bool tie) {
    oct = stepIndex(c.ue[pOct], 3) - 1;
    const double target = note + 12.0 * oct;
    const bool legato = tie && gate;
    noteTarget = target;
    if (!legato) noteGlided = target;  // glide only on tied steps (or legato MIDI)
    over = false;
    if (!legato) {
      sqr = stepIndex(c.ue[pSqr], 2) == 1;
      acc = accIn;
      ef.trigger();
      vca.setSustain(1.0);
      vca.setDecay(0.030, fsE_);
      vca.trigger();
    }
    gate = true;
    control(c);
  }
  void noteOff() {
    if (!gate) return;
    gate = false;
    vca.discharge(0.030, fsE_);  // release τ = 30 ms (KEPT)
  }
  void trigger(const VoiceCtx& c, const HitInfo& h) override { noteOn(c, h.note, h.acc, h.tie); }
  bool tick(const VoiceCtx& c, double, double& L, double& R2) override {
    osc.setFreq(f, fsE_);
    const double s = sqr ? osc.pulse(0.5) : osc.saw();
    osc.advance();
    fc = cutoffNow(c.ue, c.tolCut);
    double y;
    if (id_ == LEAD) {
      ota.svf.set(fc, R, fsE_);
      y = ota.tick(s);
    } else {
      ladder.set(fc, fsE_);
      y = ladder.tick(s * (1.0 + 0.5 * k), k, 1.0);
    }
    const double accGain = mAcc_(4.0 * acc * accAmt / 20.0, [](double x) { return std::pow(10.0, x); });  // +4 dB · ACCENT
    last = y;
    y *= vca.e * accGain;
    ef.tick();
    vca.tick();
    L = R2 = y;
    return false;
  }
  // NOTE OUT = (note_glided − 48)/12 V, hard ±5 V rail; the over-range flag latches until the next note (§13.9).
  double noteOut() {
    bool o = false;
    namespace pitch = jidai::jcs::pitch;
    const double v = pitch::clampPitch(pitch::voltsForNote(pitch::Law::VOct, noteGlided), o);
    if (o) over = true;
    return v;
  }
  bool quiet() const override { return !gate && vca.quiet(); }
  double env() const override { return vca.e; }
  double filterEnv() const { return ef.e; }
  double pitchEnv() const override { return ef.e; }
  double core() const override { return last; }
};

}  // namespace shogun
