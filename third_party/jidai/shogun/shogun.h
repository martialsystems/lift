#pragma once
// SHOGUN engine v2 (spec v2.2). Host rate, one oversampled domain (M = 1/2/4) with one decimator per audio output,
// latency 0/23/26, control outputs at zero latency. 16 voices, sequencer, mod matrix, mixer, 151-port jack table.
//
// Allocation happens only in prepare(). processSample() is the per-sample contract shared by jidai-rack
// (ShogunDevice, §13.4), the plugin and the web build.

#include <algorithm>
#include <cstdint>
#include <memory>

#include <jidai/jcs/Detect.h>

#include "dsp.h"
#include "mix.h"
#include "mod.h"
#include "params.h"
#include "ports.h"
#include "seq.h"
#include "voices/bd1.h"
#include "voices/bd2.h"
#include "voices/cb.h"
#include "voices/cp.h"
#include "voices/metal.h"
#include "voices/perc.h"
#include "voices/sd.h"
#include "voices/synth.h"
#include "voices/toms.h"

namespace shogun {

// KEPT target peaks (§4.4/§9.5) used as calibration: each voice at its noon defaults, g_vel = g_level = 1, peaks here.
constexpr double kTargetPeak[kVoices] = {0.60, 0.60, 0.50, 0.28, 0.50, 0.28, 0.18, 0.28,
                                         0.24, 0.24, 0.24, 0.42, 0.42, 0.42, 0.30, 0.42};

enum ClockSource : int { SRC_HOST, SRC_INT, SRC_EXT };

struct HostTransport {
  bool valid = false;   // the host supplied a position this block
  bool playing = false;
  double ppq = 0.0;     // quarter notes at the first sample of the block
  double bpm = 120.0;
};

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // ---------------------------------------------------------------- setup
  void prepare(double fs, int os);  // os: 1, 2 or 4. Allocates; resets voices and transport.
  double sampleRate() const { return fs_; }
  int osFactor() const { return M_; }
  int latencySamples() const { return osLatency(M_); }  // 0 / 23 / 26 (§3.4)
  void reset();                                         // voices, clock, mod, mixer state (keeps params, pattern)
  void loadInit();                                      // INIT kit (noon defaults) + empty pattern (§14.1)

  // ---------------------------------------------------------------- parameters (u ∈ [0,1])
  void setParam(int id, double u);     // smoothed (5 ms) for continuous parameters
  void setParamNow(int id, double u);  // no smoothing (patch load, tests)
  double param(int id) const { return target_[id]; }
  double effective(int id) const { return ue_[id]; }  // u_eff of the last processed sample (mod + CV + lock)
  void setCvAmt(int port, double amt) { cvAmt_[port] = amt; }
  // Input law of a pitch jack loaded through an alias (§12.3): 0 = 1 V/oct, 1 = lin55 (old HZ/V cable), applied with
  // the shared jidai::jcs::AliasConversion (AliasLaw::Lin55ToVoct = jcs::pitch::lin55ToVoct).
  void setInputLaw(int port, int law) {
    inLaw_[port] = static_cast<std::uint8_t>(law);
    inConv_[port] = aliasConversion(law);  // the shared AliasTable law (1 = AliasLaw::Lin55ToVoct)
  }
  int inputLaw(int port) const { return inLaw_[port]; }
  double cvAmt(int port) const { return cvAmt_[port]; }
  void setSerial(std::uint32_t serial);
  std::uint32_t serial() const { return serial_; }
  // Convenience: TOLERANCE and DRIFT at 0 (§15.5 IDEAL).
  void setIdeal() {
    setParamNow(P_GLOBAL_TOLERANCE, 0.0);
    setParamNow(P_GLOBAL_DRIFT, 0.0);
  }

  // ---------------------------------------------------------------- pattern and transport
  Pattern& pattern() { return pattern_; }
  const Pattern& pattern() const { return pattern_; }
  void setPattern(const Pattern& p) { pattern_ = p; }
  void setHostTransport(const HostTransport& t);  // call at each block start (SOURCE = HOST)
  void setRunning(bool run);
  bool running() const { return running_; }
  bool previewing() const { return previewing_; }
  bool sequencing() const { return wasRunning_; }
  // MIDI out tap (plugin): every pattern hit that actually plays (after probability, with ratchets / flams / micro-
  // timing as scheduled), at the sample it fires. Reading it never changes the audio.
  struct Played {
    std::int64_t at;  // absolute sample index (sampleIndex() at the hit)
    int voice, kind, acc, note;  // kind 0 drum hit, 1 synth note on, 2 synth rest
    bool tie;
  };
  static constexpr int kPlayedMax = 512;
  void setPlayedTap(bool on) { tapPlayed_ = on; nPlayed_ = 0; }
  int takePlayed(Played* out, int max) {
    const int n = std::min(nPlayed_, max);
    for (int i = 0; i < n; ++i) out[i] = played_[i];
    nPlayed_ = 0;
    return n;
  }
  std::int64_t sampleIndex() const { return sample_; }  // the pattern is running (or armed, EXT) after the last sample
  void restart();                       // position to step 1, keeps running
  std::int64_t counter() const { return counter_; }  // clock steps fired since start
  int displayStep() const { return displayStep_; }   // 1-based step of the bar
  double periodSamples() const;                      // one clock step at the current tempo
  double tempo() const;
  long globalStep() const { return gStep_; }

  // ---------------------------------------------------------------- immediate events (UI pads, MIDI)
  void trigger(int voice, double velVolts = 5.0, double bend = 0.0, int accLevel = 3);
  void noteOn(int voice, double note, double velVolts = 5.0, bool tie = false);
  void noteOff(int voice);
  void setModWheel(double v) { mod_.modW = v; }
  void setAftertouch(double v) { mod_.at = v; }

  // ---------------------------------------------------------------- per-sample processing
  // values[kPorts]: inputs are read, outputs written (volts). connected[kPorts]: the graph's connected flags
  // (normals, §12.2). With nullptr for both, every jack is unpatched and only the internal bay applies.
  void processSample(float* values, const bool* connected);
  void processSample() { processSample(nullptr, nullptr); }
  double mainL() const { return outMainL_; }   // host output (±1.0 = ±5 V)
  double mainR() const { return outMainR_; }
  const double* aux() const { return outAux_; }  // 8 stereo pairs, interleaved L/R
  bool auxUsed(int pair) const { return auxUsed_[pair]; }
  const float* portValues() const { return values_; }  // the internal port buffer (web bay, probes)

  // Internal bay: cables between Shogun's own jacks (web/plugin ROUTE tab). One sample delay, like a rack cable.
  static constexpr int kMaxCables = 64;
  bool addCable(int fromPort, int toPort);
  void removeCable(int fromPort, int toPort);
  void clearCables();
  int cableCount() const { return nCables_; }
  void cable(int i, int& from, int& to) const {
    from = cableFrom_[i];
    to = cableTo_[i];
  }
  void setExternalInput(int port, float volts, bool connected) {
    extValue_[port] = volts;
    extConnected_[port] = connected;
  }

  // ---------------------------------------------------------------- modulation
  mod::ModSystem& modulation() { return mod_; }
  const mod::ModSystem& modulation() const { return mod_; }

  // ---------------------------------------------------------------- exact-silence sleep
  // With no voice, RET or delay active and every mixer, bus and decimator state exactly +0, a base sample's voice and
  // mix pass would only write +0 again: it is skipped (the clock, sequencer, modulation and outputs still run). The
  // output is bit-identical to running it (tests/engine.cpp testSleepBitExact compares the two).
  void setSleepEnabled(bool on) {
    sleepEnabled_ = on;
    sleepVerified_ = false;
  }
  bool asleep() const { return asleep_; }
  long sleptSamples() const { return sleptSamples_; }

  // ---------------------------------------------------------------- probes (tests, UI meters)
  bool voiceActive(int v) const { return active_[v]; }
  Voice& voice(int v) { return *voices_[v]; }
  const Voice& voice(int v) const { return *voices_[v]; }
  Bd1Voice& bd1() { return *bd1_; }
  Bd2Voice& bd2() { return *bd2_; }
  SdVoice& sd() { return *sd_; }
  CpVoice& cp() { return *cp_; }
  HatVoice& ch() { return *ch_; }
  HatVoice& oh() { return *oh_; }
  TomVoice& tom(int v) { return v == LTC ? *ltc_ : (v == MTC ? *mtc_ : *htc_); }
  SynthVoice& synth(int v) { return v == LEAD ? *lead_ : *bass_; }
  double voiceOut(int v) const { return voiceOut_[v]; }       // post-VCA mono at the last sub-sample (pre-pan)
  double voiceGain(int v) const { return hitGain_[v]; }       // g_vel of the current hit
  double calib(int v) const { return calib_[v]; }
  void setCalib(int v, double c) { calib_[v] = c; }
  bool overRange(int synthVoice) const { return synthVoice == LEAD ? lead_->over : bass_->over; }
  bool clipOver() const { return clip_.over; }
  double busGain(int b) const { return bus_[b].lastGain; }  // compressor gain of bus A–D (1 = no reduction)

 private:
  struct Event {
    std::int64_t when = 0;
    int voice = -1;
    int kind = 0;  // 0 drum hit, 1 note on, 2 rest (note off)
    int acc = 2;
    double bend = 0.0;
    double note = 48.0;
    bool tie = false;
    int track = -1, pos = -1;  // step whose locks apply
    bool pattern = true;
    bool live = false;
  };
  struct Pending {
    bool on = false;
    int kind = 0;  // 0 drum hit, 1 note on
    double velVolts = 5.0;
    int acc = 2;
    double bend = 0.0;
    double note = 48.0;
    bool tie = false;
    int track = -1, pos = -1;
    bool fromJack = false, velPatched = false;
    double trigVolts = 5.0, velNorm = 1.0, accNorm = 0.5;
  };
  static constexpr int kMaxEvents = 512;

  void buildTables();
  void drawTolerances();
  void clockSample(const float* in, const bool* con);
  void onTrackStep(int t, long s, double period);
  void scheduleHits(int t, long s, int pos, double when, double period);
  void schedule(const Event& e);
  void fireDue(const float* in, const bool* con);
  void fireEvent(const Event& e, const float* in, const bool* con);
  void hit(int v, double velVolts, int accLevel, double bend, int track, int pos, bool fromJack, double trigVolts);
  void startNote(int v, double note, double velVolts, int accLevel, bool tie, int track, int pos);
  void applyLocks(int v, int track, int pos);
  void computeEffective(const float* in, const bool* con);
  VoiceCtx makeCtx(int v, const float* in, const bool* con) const;
  void renderSubSamples(const float* in, const bool* con);
  void writeControlOutputs(float* out);
  void stopAll();
  double gVelFor(int v, double velVolts) const;

  double fs_ = 48000.0, fsE_ = 96000.0;
  int M_ = 2;
  std::uint32_t serial_ = 0x5A31C0DEu;

  // parameters
  double target_[kParamCount] = {};
  double smooth_[kParamCount] = {};
  double ue_[kParamCount] = {};
  double lock_[kParamCount] = {};
  bool locked_[kParamCount] = {};
  double velDecayOff_[kVoices] = {};
  double cvAmt_[kPorts] = {};
  double aSmooth_ = 0.0;
  int decayParams_[kVoices][2] = {};
  int toneParam_[kVoices] = {};
  VoiceParams vparams_[kVoices] = {};
  int voiceParamList_[kVoices][96] = {};
  int voiceParamCount_[kVoices] = {};
  WaveParams waveIds_[5] = {};
  Pending pending_[kVoices];
  XorShift32 rndHit_[kVoices];
  double driftCents_[kVoices] = {};

  // tolerance and drift (§3.5)
  double zPitch_[kVoices] = {}, zTau_[kVoices] = {}, zCut_[kVoices] = {}, zMetal_[3][6] = {};
  OuDrift drift_[kVoices];

  // voices
  std::unique_ptr<Bd1Voice> bd1_;
  std::unique_ptr<Bd2Voice> bd2_;
  std::unique_ptr<SdVoice> sd_;
  std::unique_ptr<RsVoice> rs_;
  std::unique_ptr<CpVoice> cp_;
  std::unique_ptr<ClVoice> cl_;
  std::unique_ptr<MaVoice> ma_;
  std::unique_ptr<CbVoice> cb_;
  std::unique_ptr<HatVoice> ch_, oh_;
  std::unique_ptr<CyVoice> cy_;
  std::unique_ptr<TomVoice> ltc_, mtc_, htc_;
  std::unique_ptr<SynthVoice> lead_, bass_;
  Voice* voices_[kVoices] = {};
  bool active_[kVoices] = {};
  double hitGain_[kVoices] = {};
  double calib_[kVoices] = {};
  double chokeGain_[kVoices] = {}, chokeA_ = 0.0;
  bool choking_[kVoices] = {};
  TptOnePole dcL_[kVoices], dcR_[kVoices];
  double voiceOut_[kVoices] = {};
  double vcPrev_[5] = {}, vcCur_[5] = {};
  double coreOut_[kVoices] = {};
  VoiceCtx ctx_[kVoices];
  jidai::jcs::Schmitt trigDet_[kVoices];  // JCS R3 (shared detector), fed float volts
  jidai::jcs::Schmitt clkDet_, rstDet_, runDet_, gateDet_[2];
  bool synthGateJack_[2] = {};
  double synthSeqNote_[2] = {48.0, 48.0};

  // decimators / upsamplers
  Decimator decMain_[2], decAux_[16], decOut_[kVoices], decSend_[2];
  Upsampler upRet_[kVoices], upFx_[2];
  double retBuf_[kVoices][4] = {};
  double outTap_[kVoices] = {};
  double fxBuf_[2][4] = {};

  // mixer
  mix::Bus bus_[4];
  mix::Drive masterDrive_;
  mix::Compressor glue_;
  mix::Width width_;
  mix::Clip clip_;
  mix::Delay delay_;
  bool glueOn_ = false, delayActive_ = false;
  bool sleepEnabled_ = true, sleepVerified_ = false, asleep_ = false;
  std::uint64_t sleepSig_ = 0;
  long sleptSamples_ = 0;
  dsp::Memo1 sleepGain_[4];
  bool canSleep(const bool* retOn, const bool* busUsed, const bool* con);
  int busSc_[4] = {-1, -1, -1, -1};
  double delayEnergy_ = 0.0;
  double outMainL_ = 0.0, outMainR_ = 0.0, outAux_[16] = {};
  bool auxUsed_[8] = {};
  double volume_ = 1.0;
  // PERF PROTOTYPE: caches of values derived from u_eff (recomputed only when an input changes; bit-exact).
  double panKey_[kVoices], panLc_[kVoices], panRc_[kVoices];
  double busKey_[4][8];
  double mDriveKey_, glueKey_, ceilKey_;
  double dlyKey_[5];
  int inPorts_[kPorts]; int nInPorts_ = 0;
  void invalidateCaches() {
    const double nan = __builtin_nan("");
    for (int v = 0; v < kVoices; ++v) panKey_[v] = nan;
    for (auto& b : busKey_) for (double& k : b) k = nan;
    mDriveKey_ = glueKey_ = ceilKey_ = nan;
    for (double& k : dlyKey_) k = nan;
    sleepVerified_ = false;
  }

  // modulation
  mod::ModSystem mod_;

  // sequencer and clock
  Pattern pattern_;
  Event events_[kMaxEvents];
  int nEvents_ = 0;
  std::int64_t sample_ = 0;  // absolute base-sample index
  bool running_ = false, wasRunning_ = false;
  bool previewing_ = false;
  bool tapPlayed_ = false;
  int nPlayed_ = 0;
  Played played_[kPlayedMax];  // ▶ preview while the host is stopped (SRC HOST)
  double ppq_ = 0.0;         // song position of the current sample
  double intPpq_ = 0.0, intAnchorPpq_ = 0.0, intAnchorBpm_ = 120.0;
  std::int64_t intAnchorSample_ = 0;
  HostTransport host_;
  int hostOffset_ = 0;
  long lastStep_[16] = {};
  long gStep_ = -1;
  std::int64_t counter_ = 0;
  int displayStep_ = 1;
  bool started_[16] = {};
  // EXT clock
  std::int64_t lastClkEdge_ = -1;
  double extPeriod_ = 6000.0;
  long extCount_ = 0;
  std::int64_t startSample_ = -100;
  // clock outputs
  int rstPulse_ = 0, runPulse_ = 0;
  double accOut_ = 0.0;
  long accStep_ = -1;
  double rnd_ = 0.0;

  // ports
  float values_[kPorts] = {};
  bool connected_[kPorts] = {};
  float inBuf_[kPorts] = {};
  std::uint8_t inLaw_[kPorts] = {};
  jidai::jcs::AliasConversion inConv_[kPorts] = {};
  int moving_[kParamCount] = {}, touchedList_[kParamCount] = {};
  int nMoving_ = 0, nTouched_ = 0;
  bool isMoving_[kParamCount] = {}, touched_[kParamCount] = {};
  void refreshBase(int p);
  void addEffective(int p, double d);
  float extValue_[kPorts] = {};
  bool extConnected_[kPorts] = {};
  int cableFrom_[kMaxCables] = {}, cableTo_[kMaxCables] = {};
  int nCables_ = 0;
};

}  // namespace shogun
