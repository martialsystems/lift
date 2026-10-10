#include "shogun.h"

#include <jidai/jcs/Pitch.h>

#include <algorithm>
#include <cstring>

namespace shogun {

namespace {

// JCS R4 (shared Pitch.h): V/OCT note for a jack's volts, 0 V = C3 = note 48.
inline double voctNote(double v) { return jidai::jcs::pitch::note(jidai::jcs::pitch::Law::VOct, v); }

// Calibration measured on this engine (tools/measure_calib, §15.5): KEPT target peak / noon peak at g_vel = g_level = 1.
// Re-measure whenever a voice body changes (TESTPLAN "calibration").
constexpr double kCalibDefault[kVoices] = {
#include "calib_table.inc"
};

// Delay divisions in beats (FX:DELAY TIME choices).
constexpr double kDelayBeats[12] = {0.25, 0.375, 1.0 / 6.0, 0.5, 0.75, 1.0 / 3.0, 1.0, 1.5, 2.0 / 3.0, 2.0, 3.0, 4.0 / 3.0};
// PPQN rates for CLK IN / CLK OUT (index 0 = STEP).
constexpr double kPpqn[6] = {0.0, 1.0, 2.0, 4.0, 24.0, 48.0};
// Old-pair layout for OUTPUT = PAIR (aux pair, side: 0 L only, 1 R only, 2 stereo).
constexpr int kPairAux[kVoices] = {0, 0, 1, 1, 3, 5, 6, 5, 2, 2, 2, 4, 4, 4, 7, 7};
constexpr int kPairSide[kVoices] = {0, 1, 0, 1, 2, 0, 2, 1, 0, 0, 1, 2, 2, 2, 0, 1};

inline std::uint32_t hash32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
// Reproducible uniform for (pattern seed, bar, track, step) (§10.2 PROB).
inline double seededUniform(std::uint32_t seed, long bar, int track, long step, std::uint32_t salt) {
  std::uint32_t h = hash32(seed ^ salt);
  h = hash32(h ^ static_cast<std::uint32_t>(bar) * 0x9E3779B9u);
  h = hash32(h ^ static_cast<std::uint32_t>(track + 1) * 0x85EBCA6Bu);
  h = hash32(h ^ static_cast<std::uint32_t>(step) * 0xC2B2AE35u);
  return static_cast<double>(h) * (1.0 / 4294967296.0);
}

}  // namespace

Engine::Engine() {
  bd1_ = std::make_unique<Bd1Voice>();
  bd2_ = std::make_unique<Bd2Voice>();
  sd_ = std::make_unique<SdVoice>();
  rs_ = std::make_unique<RsVoice>();
  cp_ = std::make_unique<CpVoice>();
  cl_ = std::make_unique<ClVoice>();
  ma_ = std::make_unique<MaVoice>();
  cb_ = std::make_unique<CbVoice>();
  ch_ = std::make_unique<HatVoice>(CH);
  oh_ = std::make_unique<HatVoice>(OH);
  cy_ = std::make_unique<CyVoice>();
  ltc_ = std::make_unique<TomVoice>(LTC);
  mtc_ = std::make_unique<TomVoice>(MTC);
  htc_ = std::make_unique<TomVoice>(HTC);
  lead_ = std::make_unique<SynthVoice>(LEAD);
  bass_ = std::make_unique<SynthVoice>(BASS);
  Voice* v[kVoices] = {bd1_.get(), bd2_.get(), sd_.get(),  rs_.get(),  cp_.get(),  cl_.get(),  ma_.get(),  cb_.get(),
                       ch_.get(),  oh_.get(),  cy_.get(),  ltc_.get(), mtc_.get(), htc_.get(), lead_.get(), bass_.get()};
  for (int i = 0; i < kVoices; ++i) voices_[i] = v[i];
  buildTables();
  loadInit();
  prepare(48000.0, 2);
}

Engine::~Engine() = default;

void Engine::buildTables() {
  for (int v = 0; v < kVoices; ++v) {
    decayParams_[v][0] = decayParams_[v][1] = -1;
    toneParam_[v] = -1;
    vparams_[v] = voiceParams(v);
    voiceParamCount_[v] = 0;
  }
  for (int p = 0; p < kParamCount; ++p) {
    const int v = kParams[p].voice;
    if (v >= 0 && voiceParamCount_[v] < 96) voiceParamList_[v][voiceParamCount_[v]++] = p;
  }
  static constexpr int kWave[5] = {BD1, BD2, LTC, MTC, HTC};
  for (int i = 0; i < 5; ++i) waveIds_[i] = waveParams(kWave[i]);
  auto D = [&](int v, int a, int b = -1) {
    decayParams_[v][0] = a;
    decayParams_[v][1] = b;
  };
  // §5 table: DECAY → / TONE → per voice.
  D(BD1, P_BD1_DECAY);
  toneParam_[BD1] = P_BD1_DRIVE;
  D(BD2, P_BD2_DECAY);
  toneParam_[BD2] = P_BD2_TONE;
  D(SD, P_SD_TDECAY, P_SD_SNDEC);
  toneParam_[SD] = P_SD_SNAPPY;
  toneParam_[RS] = P_RS_LEVEL;  // RS DECAY uses the τ·2^{V/2.5} law (VoiceCtx::decayV)
  D(CP, P_CP_DECAY);
  toneParam_[CP] = P_CP_FILTER;
  D(CL, P_CL_DECAY);
  toneParam_[CL] = P_CL_LEVEL;
  D(MA, P_MA_DECAY);
  toneParam_[MA] = P_MA_LEVEL;
  D(CB, P_CB_DECAY);  // CB TONE: BP centre ±2 oct (VoiceCtx::toneV)
  D(CH, P_CH_DECAY);  // hats TONE: HP/BP ±2 oct (toneV)
  D(OH, P_OH_DECAY);
  D(CY, P_CY_DECAY);
  toneParam_[CY] = P_CY_TONE;
  D(LTC, P_LTC_DECAY);
  toneParam_[LTC] = P_LTC_WAVE;
  D(MTC, P_MTC_DECAY);
  toneParam_[MTC] = P_MTC_WAVE;
  D(HTC, P_HTC_DECAY);
  toneParam_[HTC] = P_HTC_WAVE;
  D(LEAD, P_LEAD_DECAY);
  D(BASS, P_BASS_DECAY);
}

void Engine::loadInit() {
  for (int p = 0; p < kParamCount; ++p) {
    target_[p] = smooth_[p] = ue_[p] = static_cast<double>(kParams[p].def);
    locked_[p] = false;
    lock_[p] = 0.0;
    isMoving_[p] = touched_[p] = false;
  }
  nMoving_ = nTouched_ = 0;
  for (int i = 0; i < kPorts; ++i) {
    cvAmt_[i] = 1.0;
    inLaw_[i] = 0;
    inConv_[i] = {};
  }
  for (int v = 0; v < kVoices; ++v) {
    calib_[v] = kCalibDefault[v];
    velDecayOff_[v] = 0.0;
  }
  pattern_ = Pattern{};
  mod_.clearRows();
  clearCables();
}

void Engine::setSerial(std::uint32_t s) {
  serial_ = s != 0 ? s : 0x5A31C0DEu;
  drawTolerances();
}

void Engine::drawTolerances() {
  XorShift32 r;
  r.seed(serial_);
  auto gauss = [&]() {
    double u1 = r.uniform();
    if (u1 < 1e-12) u1 = 1e-12;
    const double u2 = r.uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
  };
  for (int v = 0; v < kVoices; ++v) {
    zPitch_[v] = gauss();
    zTau_[v] = gauss();
    zCut_[v] = gauss();
  }
  for (auto& bank : zMetal_)
    for (double& z : bank) z = gauss();
}

void Engine::prepare(double fs, int os) {
  invalidateCaches();
  nInPorts_ = 0;
  for (int i = 0; i < kPorts; ++i) if (kPortTable[i].dir == PortDir::In) inPorts_[nInPorts_++] = i;
  fs_ = fs;
  M_ = (os >= 4) ? 4 : (os == 2 ? 2 : 1);
  fsE_ = fs_ * M_;
  aSmooth_ = rcCoef(0.005, fs_);
  for (int v = 0; v < kVoices; ++v) {
    voices_[v]->prepare(fs_, fsE_);
    dcL_[v].set(8.0, fsE_);
    dcR_[v].set(8.0, fsE_);
    decOut_[v].prepare(M_);
    upRet_[v].prepare(M_);
    drift_[v].prepare(fs_);
    drift_[v].rng.seed(voiceSeed(v) ^ 0x51ED270Bu);
  }
  for (auto& d : decMain_) d.prepare(M_);
  for (auto& d : decAux_) d.prepare(M_);
  for (auto& d : decSend_) d.prepare(M_);
  for (auto& u : upFx_) u.prepare(M_);
  for (auto& b : bus_) b.prepare(fsE_);
  width_.prepare(fsE_);
  delay_.prepare(fs_, 4.6);
  mod_.prepare(fs_);
  chokeA_ = rcCoef(0.0015, fsE_);  // 1.5 ms choke discharge (§6.6)
  drawTolerances();
  reset();
}

void Engine::reset() {
  for (int v = 0; v < kVoices; ++v) {
    voices_[v]->reset();
    active_[v] = false;
    hitGain_[v] = 1.0;
    chokeGain_[v] = 1.0;
    choking_[v] = false;
    dcL_[v].reset();
    dcR_[v].reset();
    decOut_[v].reset();
    upRet_[v].reset();
    drift_[v].reset();
    voiceOut_[v] = coreOut_[v] = 0.0;
    trigDet_[v].reset();
    for (double& x : retBuf_[v]) x = 0.0;
  }
  for (auto& d : decMain_) d.reset();
  for (auto& d : decAux_) d.reset();
  for (auto& d : decSend_) d.reset();
  for (auto& u : upFx_) u.reset();
  for (auto& c : fxBuf_)
    for (double& x : c) x = 0.0;
  for (auto& b : bus_) b.reset();
  masterDrive_.reset();
  glue_.reset();
  width_.reset();
  clip_.reset();
  delay_.reset();
  delayActive_ = false;
  delayEnergy_ = 0.0;
  mod_.reset();
  for (int i = 0; i < 5; ++i) vcPrev_[i] = vcCur_[i] = 0.0;
  nEvents_ = 0;
  for (auto& e : events_) e.live = false;
  sample_ = 0;
  running_ = wasRunning_ = false;
  previewing_ = false;
  ppq_ = intPpq_ = 0.0;
  hostOffset_ = 0;
  for (int t = 0; t < 16; ++t) {
    lastStep_[t] = -1;
    started_[t] = false;
  }
  gStep_ = -1;
  counter_ = 0;
  displayStep_ = 1;
  clkDet_.reset();
  rstDet_.reset();
  runDet_.reset();
  synthGateJack_[0] = synthGateJack_[1] = false;
  lastClkEdge_ = -1;
  extCount_ = 0;
  rstPulse_ = runPulse_ = 0;
  accOut_ = 0.0;
  outMainL_ = outMainR_ = 0.0;
  for (double& a : outAux_) a = 0.0;
  for (float& x : values_) x = 0.0f;
  for (int i = 0; i < kVoices; ++i) pending_[i] = Pending{};
  sleepVerified_ = asleep_ = false;
}

void Engine::setParam(int id, double u) {
  if (id < 0 || id >= kParamCount) return;
  target_[id] = clampd(u, 0.0, 1.0);
  if (kParams[id].kind == ParamKind::Stepped || kParams[id].kind == ParamKind::Toggle) {
    smooth_[id] = target_[id];
    refreshBase(id);
  } else if (!isMoving_[id] && !dsp::exactEq(smooth_[id], target_[id])) {
    isMoving_[id] = true;
    moving_[nMoving_++] = id;
  }
}
void Engine::setParamNow(int id, double u) {
  if (id < 0 || id >= kParamCount) return;
  target_[id] = smooth_[id] = clampd(u, 0.0, 1.0);
  refreshBase(id);
}

double Engine::tempo() const {
  const int src = stepIndex(target_[P_CLOCK_SOURCE], 3);
  if (src == SRC_HOST && host_.valid && host_.bpm > 0.0) return host_.bpm;
  return tempoBpm(target_[P_CLOCK_TEMPO]);
}
double Engine::periodSamples() const {
  return 60.0 * fs_ / (tempo() * stepsPerQuarter(stepIndex(target_[P_CLOCK_SCALE], 4)));
}

void Engine::setHostTransport(const HostTransport& t) {
  host_ = t;
  hostOffset_ = 0;
}
void Engine::setRunning(bool run) {
  if (run && !running_) {
    startSample_ = sample_;
    intPpq_ = 0.0;
    intAnchorPpq_ = 0.0;
    intAnchorSample_ = sample_;
    intAnchorBpm_ = tempo();
  }
  running_ = run;
}
void Engine::restart() {
  intPpq_ = 0.0;
  intAnchorPpq_ = 0.0;
  intAnchorSample_ = sample_;
  intAnchorBpm_ = tempo();
  extCount_ = 0;
  for (int t = 0; t < 16; ++t) {
    lastStep_[t] = -1;
    started_[t] = false;
  }
  gStep_ = -1;
}

// ---------------------------------------------------------------- immediate events
void Engine::trigger(int voice, double velVolts, double bend, int accLevel) {
  if (voice < 0 || voice >= kVoices) return;
  Pending& p = pending_[voice];
  p.on = true;
  p.kind = 0;
  p.velVolts = velVolts;
  p.acc = accLevel;
  p.bend = bend;
  p.track = p.pos = -1;
  p.fromJack = false;
}
void Engine::noteOn(int voice, double note, double velVolts, bool tie) {
  if (voice != LEAD && voice != BASS) return;
  Pending& p = pending_[voice];
  p.on = true;
  p.kind = 1;
  p.note = note;
  p.velVolts = velVolts;
  p.acc = velVolts >= 4.5 ? 3 : 2;
  p.tie = tie;
  p.track = p.pos = -1;
}
void Engine::noteOff(int voice) {
  if (voice == LEAD) lead_->noteOff();
  if (voice == BASS) bass_->noteOff();
}

// ---------------------------------------------------------------- internal bay
bool Engine::addCable(int from, int to) {
  if (from < 0 || from >= kPorts || to < 0 || to >= kPorts) return false;
  if (kPortTable[from].dir != PortDir::Out || kPortTable[to].dir != PortDir::In) return false;
  // RackGraph kAllowed: Audio/CV feed Audio or CV; Gate feeds anything; Audio/CV into a Gate input is refused.
  if (kPortTable[to].type == PortType::Gate && kPortTable[from].type != PortType::Gate) return false;
  for (int i = 0; i < nCables_; ++i)
    if (cableFrom_[i] == from && cableTo_[i] == to) return true;
  if (nCables_ >= kMaxCables) return false;
  cableFrom_[nCables_] = from;
  cableTo_[nCables_] = to;
  ++nCables_;
  return true;
}
void Engine::removeCable(int from, int to) {
  for (int i = 0; i < nCables_; ++i)
    if (cableFrom_[i] == from && cableTo_[i] == to) {
      cableFrom_[i] = cableFrom_[nCables_ - 1];
      cableTo_[i] = cableTo_[nCables_ - 1];
      --nCables_;
      return;
    }
}
void Engine::clearCables() {
  nCables_ = 0;
  for (int i = 0; i < kPorts; ++i) {
    extValue_[i] = 0.0f;
    extConnected_[i] = false;
  }
}

// ---------------------------------------------------------------- velocity (§4.8)
double Engine::gVelFor(int v, double velVolts) const {
  const double g = gVelFromVolts(velVolts);
  const double A = target_[P_MASTER_ACCENT];
  const double g1 = 1.0 - A * (1.0 - g);
  const double k = target_[vparams_[v].velLevel];
  return 1.0 - k * (1.0 - g1);
}

// ---------------------------------------------------------------- clock and sequencer (§10)
void Engine::clockSample(const float* in, const bool* con) {
  const int src = stepIndex(target_[P_CLOCK_SOURCE], 3);
  const bool hostMode = src == SRC_HOST && host_.valid;
  const double bpm = tempo();
  const int gScale = stepIndex(target_[P_CLOCK_SCALE], 4);
  const double spqG = stepsPerQuarter(gScale);
  const int bar = 1 + stepIndex(target_[P_CLOCK_BAR], 32);

  // RUN IN toggles run (edge), RST IN restarts; START/RESET absorb a coincident clock edge (JCS R5).
  bool absorbClock = false;
  if (con && con[PORT_RUN_IN] && runDet_.rising(static_cast<float>(in[PORT_RUN_IN]))) {
    setRunning(!running_);
    absorbClock = true;
  }
  if (con && con[PORT_RST_IN] && rstDet_.rising(static_cast<float>(in[PORT_RST_IN]))) {
    restart();
    absorbClock = true;
  }
  bool clkEdge = false;
  if (con && con[PORT_CLK_IN]) clkEdge = clkDet_.rising(static_cast<float>(in[PORT_CLK_IN]));
  if (clkEdge && (absorbClock || sample_ - startSample_ <= 2) && src == SRC_EXT && extCount_ > 0) clkEdge = false;

  bool run = running_;
  double ppq = 0.0;
  if (hostMode && running_ && !host_.playing) {
    // ▶ with the host stopped: an internal preview at the host's tempo (the INT clock from the press), until the host
    // starts and takes over.
    previewing_ = true;
    if (!dsp::exactEq(bpm, intAnchorBpm_)) {
      intAnchorPpq_ = intPpq_;
      intAnchorSample_ = sample_;
      intAnchorBpm_ = bpm;
    }
    ppq = intAnchorPpq_ + static_cast<double>(sample_ - intAnchorSample_) * bpm / (60.0 * fs_);
    intPpq_ = ppq;
    ++hostOffset_;
  } else if (hostMode) {
    if (previewing_ && host_.playing) {  // the host started: the preview ends, the host's transport drives
      running_ = false;
      wasRunning_ = false;
    }
    previewing_ = false;  // (or ▶ stopped it: the stop below runs as usual)
    run = host_.playing;
    ppq = host_.ppq + hostOffset_ * bpm / (60.0 * fs_);
    ++hostOffset_;
  } else if (src == SRC_EXT) {
    previewing_ = false;
    if (run && clkEdge) {
      if (lastClkEdge_ >= 0) extPeriod_ = static_cast<double>(sample_ - lastClkEdge_);
      lastClkEdge_ = sample_;
      ++extCount_;
    }
    const int mode = stepIndex(target_[P_CLOCK_CLK_IN], 6);
    const double perQ = mode == 0 ? spqG : kPpqn[mode];
    ppq = extCount_ > 0 ? static_cast<double>(extCount_ - 1) / perQ : -1.0;
  } else {
    previewing_ = false;
    if (!dsp::exactEq(bpm, intAnchorBpm_)) {
      intAnchorPpq_ = intPpq_;
      intAnchorSample_ = sample_;
      intAnchorBpm_ = bpm;
    }
    ppq = intAnchorPpq_ + static_cast<double>(sample_ - intAnchorSample_) * bpm / (60.0 * fs_);
    intPpq_ = ppq;
  }

  if (run && !wasRunning_) {
    for (int t = 0; t < 16; ++t) {
      lastStep_[t] = -1;
      started_[t] = false;
    }
    gStep_ = -1;
    counter_ = 0;
    runPulse_ = static_cast<int>(std::lround(0.005 * fs_));
    for (auto& r : rndHit_) r.seed(pattern_.seed ^ 0x2545F491u);
  }
  if (!run) {
    if (wasRunning_) {
      stopAll();
      runPulse_ = static_cast<int>(std::lround(0.005 * fs_));
    }
    wasRunning_ = false;
    running_ = hostMode ? running_ : run;
    ppq_ = ppq;
    return;
  }
  wasRunning_ = true;
  ppq_ = ppq;
  if (ppq < 0.0) return;  // EXT: waiting for the first edge

  // Tracks (polymeter: each has its own scale and length).
  for (int t = 0; t < 16; ++t) {
    const Track& tr = pattern_.tracks[t];
    const double spq = stepsPerQuarter(tr.scale < 0 ? gScale : tr.scale);
    const double pos = ppq * spq + 1e-9;
    const long s = static_cast<long>(std::floor(pos));
    if (s == lastStep_[t]) continue;
    const double period = src == SRC_EXT ? extPeriod_ * spqG / spq : 60.0 * fs_ / (bpm * spq);
    bool fire = (s == lastStep_[t] + 1) || lastStep_[t] < 0 || src == SRC_EXT;
    if (!fire) fire = (pos - static_cast<double>(s)) * period < 1.0;  // host jump/loop: fire only on an exact boundary
    if (fire) onTrackStep(t, s, period);
    lastStep_[t] = s;
  }
  // Global clock grid: counter, display, RST OUT, BAR retrig, RND source.
  const long sg = static_cast<long>(std::floor(ppq * spqG + 1e-9));
  if (sg != gStep_) {
    gStep_ = sg;
    ++counter_;
    const long inBar = ((sg % bar) + bar) % bar;
    displayStep_ = static_cast<int>(inBar) + 1;
    if (inBar == 0) {
      rstPulse_ = static_cast<int>(std::lround(0.005 * fs_));
      for (auto& l : mod_.lfo) l.onBar();
    }
    rnd_ = seededUniform(pattern_.seed, sg / bar, 99, sg, 0x52A4D1u);
    mod_.rnd = rnd_;
    if (accStep_ != sg) accOut_ = 0.0;
  }
}

void Engine::onTrackStep(int t, long s, double period) {
  const Track& tr = pattern_.tracks[t];
  const int len = std::clamp(tr.len, 1, kMaxSteps);
  auto offset = [&](long k) {
    const Step& st = tr.steps[((k % len) + len) % len];
    const double sw = tr.swing < 0.0 ? 0.5 + 0.25 * target_[P_CLOCK_SWING] : tr.swing;
    double off = 0.0;
    if ((k & 1) == 1) off += (2.0 * sw - 1.0) * period;  // odd clock steps (KEPT)
    off += static_cast<double>(st.micro) * period;
    off += std::round(tr.shift * 0.030 * fs_);
    return off;
  };
  const double off = offset(s);
  if (off >= 0.0 || !started_[t]) scheduleHits(t, s, static_cast<int>(((s % len) + len) % len), static_cast<double>(sample_) + std::max(0.0, off), period);
  const double off2 = offset(s + 1);
  if (off2 < 0.0) scheduleHits(t, s + 1, static_cast<int>((((s + 1) % len) + len) % len), static_cast<double>(sample_) + period + off2, period);
  started_[t] = true;
}

void Engine::scheduleHits(int t, long s, int pos, double when, double period) {
  const Step& st = pattern_.tracks[t].steps[pos];
  Event e;
  e.voice = t;
  e.track = t;
  e.pos = pos;
  e.pattern = true;
  e.acc = std::clamp<int>(st.acc, 1, 3);
  e.bend = static_cast<double>(st.bend);
  e.note = st.note;
  e.tie = st.tie;
  if (t == LEAD || t == BASS) {
    e.kind = st.on ? 1 : 2;
    e.when = static_cast<std::int64_t>(std::llround(when));
    schedule(e);
    return;
  }
  if (!st.on) return;
  const int bar = 1 + stepIndex(target_[P_CLOCK_BAR], 32);
  if (st.prob < 1.0f) {
    const double u = seededUniform(pattern_.seed, s / bar, t, s, 0x9B0Bu);
    if (u >= static_cast<double>(st.prob)) return;
  }
  e.kind = 0;
  if (st.flam > 0 && t != CP) {  // flam is not on clap (KEPT)
    const int k = (st.flam - 1) & 15;
    const int hits = 2 + (k % 4);
    const double gap = std::round(0.00375 * (1 + k / 4) * fs_);
    for (int h = 0; h < hits; ++h) {
      e.when = static_cast<std::int64_t>(std::llround(when + h * gap));
      schedule(e);
    }
    return;
  }
  int r = st.ratchet;
  bool ok = false;
  for (int k : kRatchets) ok = ok || k == r;
  if (!ok) r = 1;
  for (int j = 0; j < r; ++j) {
    e.when = static_cast<std::int64_t>(std::llround(when + j * period / r));
    schedule(e);
  }
}

void Engine::schedule(const Event& e) {
  for (int i = 0; i < kMaxEvents; ++i) {
    if (!events_[i].live) {
      events_[i] = e;
      events_[i].live = true;
      if (i >= nEvents_) nEvents_ = i + 1;
      return;
    }
  }
}

void Engine::stopAll() {
  for (int i = 0; i < nEvents_; ++i)
    if (events_[i].pattern) events_[i].live = false;
  lead_->noteOff();
  bass_->noteOff();
  accOut_ = 0.0;
}

void Engine::fireDue(const float* in, const bool* con) {
  const bool ext = stepIndex(target_[P_CLOCK_MODE], 2) == 1;
  int top = 0;
  for (int i = 0; i < nEvents_; ++i) {
    Event& e = events_[i];
    if (!e.live) continue;
    if (e.when > sample_) {
      top = i + 1;
      continue;
    }
    e.live = false;  // <= so an event can never be stranded
    if (e.pattern && ext) {  // EXT ignores the pattern for the voices (KEPT switch law, forge-pinned) ...
      if (e.kind == 0 && isDrum(e.voice)) {  // ... but ACC OUT still carries the step's accent volts (§13.2)
        accOut_ = std::max(accOut_, kAccentVolts[e.acc - 1]);
        accStep_ = gStep_;
      }
      continue;
    }
    if (tapPlayed_ && e.pattern && nPlayed_ < kPlayedMax) played_[nPlayed_++] = {sample_, e.voice, e.kind, e.acc, static_cast<int>(std::lround(e.note)), e.tie};
    fireEvent(e, in, con);
  }
  nEvents_ = top;
}

void Engine::fireEvent(const Event& e, const float* in, const bool* con) {
  const int v = e.voice;
  if (e.kind == 2) {
    noteOff(v);
    return;
  }
  Pending& p = pending_[v];
  p.on = true;
  p.kind = e.kind;
  p.acc = e.acc;
  p.bend = e.bend;
  p.note = e.note;
  p.tie = e.tie;
  p.track = e.track;
  p.pos = e.pos;
  p.fromJack = false;
  // VEL normal: the step accent volts; a patched VEL jack replaces them (one law, §4.8).
  const int velPort = isDrum(v) ? drumPort(v, DJ_VEL) : synthPort(v - LEAD, SJ_VEL);
  p.velVolts = (con && con[velPort]) ? static_cast<double>(in[velPort]) : kAccentVolts[e.acc - 1];
  p.velPatched = con && con[velPort];
  if (e.pattern && isDrum(v)) {
    accOut_ = std::max(accOut_, kAccentVolts[e.acc - 1]);
    accStep_ = gStep_;
  }
}

void Engine::applyLocks(int v, int track, int pos) {
  for (int i = 0; i < voiceParamCount_[v]; ++i) {
    const int p = voiceParamList_[v][i];
    if (locked_[p]) {
      locked_[p] = false;
      refreshBase(p);
    }
  }
  if (track < 0 || pos < 0) return;
  const Step& st = pattern_.tracks[track].steps[pos];
  for (int i = 0; i < st.nLocks; ++i) {
    const int p = st.locks[i].param;
    if (p < 0 || p >= kParamCount) continue;
    locked_[p] = true;
    lock_[p] = static_cast<double>(st.locks[i].u);
    refreshBase(p);
  }
}

// ---------------------------------------------------------------- effective parameters (§8.4)
// u_eff = clamp(base + Σ mod + CV + VEL>DECAY), base = lock or smoothed knob (§8.4). Only parameters that move are
// touched per sample: smoothing runs on a list of moving parameters, and the additive terms are undone next sample.
void Engine::refreshBase(int p) {
  ue_[p] = clampd(locked_[p] ? lock_[p] : smooth_[p], 0.0, 1.0);
}
void Engine::addEffective(int p, double d) {
  if (!touched_[p]) {
    touched_[p] = true;
    touchedList_[nTouched_++] = p;
  }
  ue_[p] += d;
}
void Engine::computeEffective(const float* in, const bool* con) {
  int w = 0;
  for (int i = 0; i < nMoving_; ++i) {
    const int p = moving_[i];
    smooth_[p] = smooth_[p] + (1.0 - aSmooth_) * (target_[p] - smooth_[p]);
    if (std::fabs(target_[p] - smooth_[p]) < 1e-12) smooth_[p] = target_[p];
    if (!dsp::exactEq(smooth_[p], target_[p])) moving_[w++] = p;
    else isMoving_[p] = false;
    refreshBase(p);
  }
  nMoving_ = w;
  for (int i = 0; i < nTouched_; ++i) {
    touched_[touchedList_[i]] = false;
    refreshBase(touchedList_[i]);
  }
  nTouched_ = 0;
  for (int i = 0; i < mod_.nActive; ++i) addEffective(mod_.active[i], mod_.offset(mod_.active[i]));
  for (int v = 0; v < kDrumVoices; ++v) {
    const int dPort = drumPort(v, DJ_DECAY), tPort = drumPort(v, DJ_TONE);
    const bool dCon = con && con[dPort];
    if (!dsp::exactEq(velDecayOff_[v], 0.0) || dCon) {
      const double d = velDecayOff_[v] + (dCon ? cvAmt_[dPort] * static_cast<double>(in[dPort]) / 5.0 : 0.0);
      for (int k = 0; k < 2; ++k)
        if (decayParams_[v][k] >= 0) addEffective(decayParams_[v][k], d);
    }
    if (toneParam_[v] >= 0 && con && con[tPort]) addEffective(toneParam_[v], cvAmt_[tPort] * static_cast<double>(in[tPort]) / 5.0);
  }
  if (con && con[PORT_BD1_WAVE]) addEffective(P_BD1_WAVE, cvAmt_[PORT_BD1_WAVE] * static_cast<double>(in[PORT_BD1_WAVE]) / 5.0);
  if (con && con[PORT_BD2_WAVE]) addEffective(P_BD2_WAVE, cvAmt_[PORT_BD2_WAVE] * static_cast<double>(in[PORT_BD2_WAVE]) / 5.0);
  for (int i = 0; i < nTouched_; ++i) ue_[touchedList_[i]] = clampd(ue_[touchedList_[i]], 0.0, 1.0);
}

VoiceCtx Engine::makeCtx(int v, const float* in, const bool* con) const {
  VoiceCtx c;
  c.fs = fs_;
  c.fsE = fsE_;
  c.ue = ue_;
  c.moving = isMoving_;
  c.modulated = touched_;
  if (isDrum(v) && con) {
    const int pp = drumPort(v, DJ_PITCH), tp = drumPort(v, DJ_TONE), dp = drumPort(v, DJ_DECAY);
    if (con[pp]) c.pitchOct = cvAmt_[pp] * static_cast<double>(in[pp]);  // 1 V/oct × AMT (§12.3)
    if (con[tp]) c.toneV = cvAmt_[tp] * static_cast<double>(in[tp]);
    if (con[dp]) c.decayV = cvAmt_[dp] * static_cast<double>(in[dp]);
  }
  const double tol = ue_[P_GLOBAL_TOLERANCE];
  c.tolPitch = std::exp2((4.0 * tol * zPitch_[v] + driftCents_[v]) / 1200.0);
  c.tolTau = 1.0 + 0.03 * tol * zTau_[v];
  c.tolCut = 1.0 + 0.05 * tol * zCut_[v];
  return c;
}

// ---------------------------------------------------------------- per sample
void Engine::processSample(float* extValues, const bool* extCon) {
  // Inputs: the caller's graph values, or the internal bay (external inputs + Shogun→Shogun cables, 1-sample delay).
  const float* in;
  const bool* con;
  if (extValues) {
    in = extValues;
    con = extCon;
  } else {
    for (int k = 0; k < nInPorts_; ++k) {
      const int i = inPorts_[k];
      inBuf_[i] = extConnected_[i] ? extValue_[i] : kPortTable[i].rest;
      connected_[i] = extConnected_[i];
    }
    for (int c = 0; c < nCables_; ++c) {
      inBuf_[cableTo_[c]] += values_[cableFrom_[c]];
      connected_[cableTo_[c]] = true;
    }
    in = inBuf_;
    con = connected_;
  }

  clockSample(in, con);
  fireDue(in, con);

  // TRIG jacks: EXT plays only from jacks; INT ignores them unless TRIG MERGE (forge-pinned law, §12.2).
  const bool ext = stepIndex(target_[P_CLOCK_MODE], 2) == 1;
  for (int v = 0; v < kDrumVoices; ++v) {
    const int tp = drumPort(v, DJ_TRIG);
    if (!(con && con[tp])) continue;
    const bool edge = trigDet_[v].rising(static_cast<float>(in[tp]));
    if (!edge) continue;
    if (!ext && stepIndex(target_[vparams_[v].trigMerge], 2) == 0) continue;
    Pending& p = pending_[v];
    p.on = true;
    p.kind = 0;
    p.acc = 3;
    p.bend = 0.0;
    p.track = p.pos = -1;
    p.fromJack = true;
    p.trigVolts = static_cast<double>(in[tp]);
    const int vp = drumPort(v, DJ_VEL);
    p.velPatched = con[vp];
    p.velVolts = con[vp] ? static_cast<double>(in[vp]) : 5.0;
  }
  for (int s = 0; s < 2; ++s) {
    const int v = LEAD + s;
    SynthVoice& sv = synth(v);
    const int gp = synthPort(s, SJ_GATE), np = synthPort(s, SJ_NOTE), vo = synthPort(s, SJ_VOCT), cp = synthPort(s, SJ_CUTOFF);
    auto pitchIn = [&](int port) {
      const double x = static_cast<double>(in[port]);
      return inConv_[port].identity() ? x : inConv_[port].convert(x);  // old HZ/V cable: shared Lin55ToVoct (§12.3)
    };
    // The voice's CV AMT (the ROUTE knob for LEAD/BASS, stored on the NOTE port) scales the depth of the pitch CV into
    // that voice: NOTE, an old HZ/V cable on NOTE (after Lin55ToVoct) and V/OCT. 1.0 = exact 1 V/oct tracking (the
    // default), 0.5 = half, -1 = inverted around 0 V = C3. V/OCT keeps its own per-jack AMT as a second factor, so a
    // patch that set only V/OCT's AMT plays as before.
    const double pitchAmt = cvAmt_[np];
    sv.vOct = (con && con[vo]) ? pitchAmt * cvAmt_[vo] * pitchIn(vo) : 0.0;
    sv.cutoffOct = (con && con[cp]) ? cvAmt_[cp] * static_cast<double>(in[cp]) : 0.0;
    sv.a4 = a4Hz(target_[P_GLOBAL_A4]);
    const bool noteJack = con && con[np];
    // A patched NOTE jack is a continuous pitch: it moves the held note at once (slew is the patch's job; GLIDE is for
    // tied steps and legato notes).
    if (noteJack && sv.gate) sv.noteTarget = sv.noteGlided = voctNote(pitchAmt * pitchIn(np)) + 12.0 * sv.oct;
    if (con && con[gp]) {
      const bool wasHigh = gateDet_[s].high;
      const bool edge = gateDet_[s].rising(static_cast<float>(in[gp]));
      const bool use = ext || stepIndex(target_[vparams_[v].trigMerge], 2) == 1;
      if (use && edge) {
        Pending& p = pending_[v];
        p.on = true;
        p.kind = 1;
        p.note = noteJack ? voctNote(pitchAmt * pitchIn(np)) : synthSeqNote_[s];
        const int vp = synthPort(s, SJ_VEL);
        p.velVolts = con[vp] ? static_cast<double>(in[vp]) : 5.0;
        p.velPatched = con[vp];
        p.acc = p.velVolts >= 4.5 ? 3 : 2;
        p.tie = false;
        p.track = p.pos = -1;
      } else if (use && wasHigh && !gateDet_[s].high) {
        sv.noteOff();
      }
    }
  }

  // Hit bookkeeping that must precede u_eff: p-locks and VEL>DECAY.
  for (int v = 0; v < kVoices; ++v) {
    Pending& p = pending_[v];
    if (!p.on) continue;
    if (p.kind == 1 && (p.track >= 0)) synthSeqNote_[v - LEAD] = p.note;
    if (p.tie && p.kind == 1 && synth(v).gate) continue;  // tied: no new locks
    applyLocks(v, p.track, p.pos);
    // g_vel and the VEL source.
    double g;
    if (p.fromJack && !p.velPatched) {
      const int mode = stepIndex(target_[P_GLOBAL_TRIG_DYN], 4);
      const double V = p.trigVolts;
      if (mode == 0) g = 1.0;
      else if (mode == 1) g = V >= 4.5 ? 1.0 : 0.78;
      else if (mode == 2) g = 0.55 + 0.45 * clampd((V - 2.5) / 2.5, 0.0, 1.0);
      else g = 0.15 + 0.85 * clampd((V - 1.0) / 4.0, 0.0, 1.0);
      p.velVolts = 5.0 * (g - 0.15) / 0.85;
    }
    g = gVelFromVolts(p.velVolts);
    p.velNorm = (g - 0.15) / 0.85;
    p.accNorm = p.velPatched || p.fromJack ? clampd((p.velVolts - kAccentVolts[0]) / (5.0 - kAccentVolts[0]), 0.0, 1.0)
                                           : 0.5 * (p.acc - 1);
    velDecayOff_[v] = 0.3 * bip(target_[vparams_[v].velDecay]) * (p.velNorm - 1.0);
  }

  // Drift (§3.5) and modulation sources.
  const double sigma = 4.0 * target_[P_GLOBAL_DRIFT];
  for (int v = 0; v < kVoices; ++v) {
    drift_[v].setSigma(sigma);
    driftCents_[v] = sigma > 0.0 ? drift_[v].tick() : 0.0;
  }
  computeEffective(in, con);
  for (auto& l : mod_.lfo) l.read(ue_);  // before this sample's triggers (OWN VOICE restart needs MODE/RETRIG)

  // Triggers.
  for (int v = 0; v < kVoices; ++v) {
    Pending& p = pending_[v];
    if (!p.on) continue;
    p.on = false;
    VoiceCtx c = makeCtx(v, in, con);
    HitInfo h;
    h.gVel = gVelFor(v, p.velVolts);
    h.velNorm = p.velNorm;
    h.acc = p.accNorm;
    h.bend = p.bend;
    h.note = p.note;
    h.tie = p.tie;
    if (v == LEAD || v == BASS) {
      SynthVoice& sv = synth(v);
      const int np = synthPort(v - LEAD, SJ_NOTE);
      if (con && con[np]) {  // NOTE jack overrides the sequencer note; the voice's CV AMT scales its depth
        const double x = static_cast<double>(in[np]);
        h.note = voctNote(cvAmt_[np] * (inConv_[np].identity() ? x : inConv_[np].convert(x)));
      }
      const bool legato = h.tie && sv.gate;
      sv.noteOn(c, h.note, h.acc * ue_[sv.pAcc] > 0.0 ? h.acc : 0.0, h.tie);
      if (!legato) hitGain_[v] = h.gVel;
      mod_.vs[v].note = (h.note - 60.0) / 24.0;
    } else {
      voices_[v]->trigger(c, h);
      hitGain_[v] = h.gVel;
      // Choke: CH discharges OH (1.5 ms); CHOKE groups 1–4 discharge the other members of the group.
      if (v == CH) oh_->choke(0.0015);
      const int grp = stepIndex(target_[vparams_[v].choke], 5);
      if (grp > 0)
        for (int w = 0; w < kVoices; ++w)
          if (w != v && active_[w] && stepIndex(target_[vparams_[w].choke], 5) == grp) choking_[w] = true;
    }
    choking_[v] = false;
    chokeGain_[v] = 1.0;
    if (!active_[v]) {
      dcL_[v].reset();
      dcR_[v].reset();
    }
    active_[v] = true;
    mod_.vs[v].vel = p.velNorm;
    mod_.vs[v].acc = p.accNorm;
    mod_.vs[v].rndHit = rndHit_[v].bipolar();
    for (auto& l : mod_.lfo) l.onTrigger(v);
  }

  // Modulation clock (LFOs at base rate; m every 16 samples, §8.4).
  {
    const bool locked = running_ || (host_.valid && host_.playing);
    for (auto& l : mod_.lfo) l.tick(tempo(), ppq_, locked);
    for (int v = 0; v < kVoices; ++v) {
      mod_.vs[v].env = active_[v] ? (v >= LEAD ? synth(v).filterEnv() : voices_[v]->env()) : 0.0;
      mod_.vs[v].penv = active_[v] ? voices_[v]->pitchEnv() : 0.0;
    }
    if (mod_.tickClock()) mod_.update();
  }

  renderSubSamples(in, con);

  // Voice end (§3.7) and choke end.
  for (int v = 0; v < kVoices; ++v) {
    if (!active_[v]) continue;
    if (voices_[v]->quiet() || (choking_[v] && chokeGain_[v] < kQuiet)) {
      active_[v] = false;
      choking_[v] = false;
      chokeGain_[v] = 1.0;
      voices_[v]->reset();
      voiceOut_[v] = 0.0;
    }
  }

  float* out = extValues ? extValues : values_;
  writeControlOutputs(out);
  if (!extValues) {
    // Mirror inputs too, so the bay can show every jack's voltage.
    for (int k = 0; k < nInPorts_; ++k) values_[inPorts_[k]] = inBuf_[inPorts_[k]];
  }
  ++sample_;
}

void Engine::renderSubSamples(const float* in, const bool* con) {
  // Base-rate control per active voice; metal tolerances.
  const double tol = ue_[P_GLOBAL_TOLERANCE];
  for (int k = 0; k < 6; ++k) {
    ch_->bank.tol[k] = oh_->bank.tol[k] = 1.0 + 0.015 * tol * zMetal_[0][k];
    cy_->bank.tol[k] = 1.0 + 0.015 * tol * zMetal_[1][k];
  }
  cb_->tol2 = (1.0 + 0.015 * tol * zMetal_[2][1]) / (1.0 + 0.015 * tol * zMetal_[2][0]);
  for (int v = 0; v < kVoices; ++v) {
    if (!active_[v]) continue;
    ctx_[v] = makeCtx(v, in, con);
    voices_[v]->control(ctx_[v]);
  }
  // RET upsamplers (+L on the insert loop only).
  bool retOn[kVoices];
  for (int v = 0; v < kVoices; ++v) {
    const int rp = isDrum(v) ? drumPort(v, DJ_RET) : synthPort(v - LEAD, SJ_RET);
    retOn[v] = con && con[rp];
    if (retOn[v]) upRet_[v].push(static_cast<double>(in[rp]) / 5.0, retBuf_[v]);
  }
  // FOLD VC (§4.6): jack (linear interpolation across sub-samples) or the internal source.
  static constexpr int kFoldPort[5] = {PORT_FOLD_VC_BD1, PORT_FOLD_VC_BD2, PORT_FOLD_VC_LTC, PORT_FOLD_VC_MTC,
                                       PORT_FOLD_VC_HTC};
  bool vcJack[5];
  int vcSrc[5];
  double vcLevel[5];
  for (int i = 0; i < 5; ++i) {
    const WaveParams& w = waveIds_[i];
    vcJack[i] = con && con[kFoldPort[i]];
    vcSrc[i] = stepIndex(ue_[w.vcSrc], 6 + kVoices);
    vcLevel[i] = ue_[w.vcLevel];
    vcPrev_[i] = vcCur_[i];
    vcCur_[i] = vcJack[i] ? cvAmt_[kFoldPort[i]] * static_cast<double>(in[kFoldPort[i]]) / 5.0 : 0.0;
  }

  // Bus / send / solo bookkeeping.
  bool anySolo = false;
  for (int v = 0; v < kVoices; ++v) anySolo = anySolo || stepIndex(target_[vparams_[v].solo], 2) == 1;
  int route[kVoices];
  double panLg[kVoices], panRg[kVoices], send[kVoices], level[kVoices];
  bool audible[kVoices];
  bool busUsed[4] = {false, false, false, false};
  bool sendUsed = false;
  bool auxUsedNow[8] = {};
  for (int v = 0; v < kVoices; ++v) {
    const VoiceParams& vp = vparams_[v];
    route[v] = stepIndex(target_[vp.output], 14);
    if (!dsp::exactEq(ue_[vp.pan], panKey_[v])) {
      panKey_[v] = ue_[vp.pan];
      const double p = bip(ue_[vp.pan]);
      panLc_[v] = panL(p);
      panRc_[v] = panR(p);
    }
    panLg[v] = panLc_[v];
    panRg[v] = panRc_[v];
    send[v] = ue_[vp.send];
    level[v] = gLevel(ue_[vp.level]) * calib_[v];
    const bool muted = stepIndex(target_[vp.mute], 2) == 1;
    const bool solo = stepIndex(target_[vp.solo], 2) == 1;
    audible[v] = !muted && (!anySolo || solo);
    if (route[v] >= 1 && route[v] <= 4) busUsed[route[v] - 1] = true;
    if (route[v] >= 5 && route[v] <= 12) auxUsedNow[route[v] - 5] = true;
    if (route[v] == 13) auxUsedNow[kPairAux[v]] = true;
    if (send[v] > 0.0 && (active_[v] || retOn[v])) sendUsed = true;
  }
  for (int a = 0; a < 8; ++a) auxUsed_[a] = auxUsedNow[a];
  for (int b = 0; b < 4; ++b) {
    mix::Bus& bus = bus_[b];
    const int base = P_BUS_A_DRIVE + b * (P_BUS_B_DRIVE - P_BUS_A_DRIVE);
    bool same = true;
    for (int k = 0; k < 8; ++k) same = same && dsp::exactEq(busKey_[b][k], ue_[base + k]);
    if (!same) {
      for (int k = 0; k < 8; ++k) busKey_[b][k] = ue_[base + k];
      bus.drive.set(ue_[base + 0]);
      bus.tilt.set(ue_[base + 1]);
      bus.level = masterVolume(ue_[base + 2]);
      bus.comp.set(-40.0 + 40.0 * ue_[base + 3], 1.0 + 19.0 * ue_[base + 4], 0.0001 * std::pow(1000.0, ue_[base + 5]),
                   0.010 * std::pow(100.0, ue_[base + 6]), 24.0 * ue_[base + 7], fsE_);
    }
    bus.mix = ue_[base + 8];
    bus.compOn = stepIndex(target_[base + 9], 2) == 1;
    busSc_[b] = stepIndex(target_[base + 10], 17) - 1;
  }
  if (!dsp::exactEq(mDriveKey_, ue_[P_MASTER_DRIVE])) { mDriveKey_ = ue_[P_MASTER_DRIVE]; masterDrive_.set(ue_[P_MASTER_DRIVE]); }
  glueOn_ = ue_[P_MASTER_GLUE] > 0.0;
  if (!dsp::exactEq(glueKey_, ue_[P_MASTER_GLUE])) { glueKey_ = ue_[P_MASTER_GLUE]; glue_.set(-20.0 * ue_[P_MASTER_GLUE], 2.0, 0.010, 0.100, 0.0, fsE_); }
  width_.w = 2.0 * ue_[P_MASTER_WIDTH];
  if (std::fabs(width_.w - 1.0) < 1e-9) width_.w = 1.0;
  volume_ = masterVolume(ue_[P_MASTER_VOLUME]);
  clip_.on = stepIndex(target_[P_MASTER_CLIP], 2) == 1;
  if (!dsp::exactEq(ceilKey_, ue_[P_MASTER_CEILING])) { ceilKey_ = ue_[P_MASTER_CEILING]; clip_.C = std::pow(10.0, (-6.0 + 6.0 * ue_[P_MASTER_CEILING]) / 20.0); }
  const bool delayWas = delayActive_;
  delayActive_ = sendUsed || delayEnergy_ > 1e-14;
  if (delayActive_ && !delayWas) {
    for (auto& d : decSend_) d.reset();
  }
  asleep_ = canSleep(retOn, busUsed, con);
  if (asleep_) {
    // The skipped pass would have left every state at +0 and written only the bus comp's gain readout.
    for (int b = 0; b < 4; ++b) {
      mix::Bus& bus = bus_[b];
      if ((busUsed[b] || bus.compOn) && bus.compOn)
        bus.lastGain = sleepGain_[b](bus.comp.yL + bus.comp.makeup, mix::dbToGain);
    }
    ++sleptSamples_;
    return;
  }
  sleepVerified_ = false;

  double sendDec[2] = {0.0, 0.0};
  for (int sub = 0; sub < M_; ++sub) {
    double mL = 0.0, mR = 0.0, sL = 0.0, sR = 0.0;
    double bL[4] = {0, 0, 0, 0}, bR[4] = {0, 0, 0, 0};
    double aL[8] = {}, aR[8] = {};
    double scPeak[kVoices];
    for (int v = 0; v < kVoices; ++v) {
      double L = 0.0, R = 0.0;
      bool stereo = false, has = false;
      if (active_[v]) {
        VoiceCtx& c = ctx_[v];
        c.sub = sub;
        double vc = 0.0;
        const int wi = v == BD1 ? 0 : (v == BD2 ? 1 : (v == LTC ? 2 : (v == MTC ? 3 : (v == HTC ? 4 : -1))));
        if (wi >= 0) {
          if (vcJack[wi]) {
            vc = (vcPrev_[wi] + (vcCur_[wi] - vcPrev_[wi]) * (sub + 1) / M_) * vcLevel[wi];
          } else {
            const int s = vcSrc[wi];
            double x = 0.0;
            if (s == 0) x = voices_[v]->core();
            else if (s == 1) x = voices_[v]->noiseSample();
            else if (s <= 5) x = mod_.lfo[s - 2].value(v);
            else x = coreOut_[s - 6];
            vc = x * vcLevel[wi];
          }
        }
        stereo = voices_[v]->tick(c, vc, L, R);
        L = dcL_[v].hp(L);
        R = stereo ? dcR_[v].hp(R) : L;
        const double g = hitGain_[v] * level[v];  // VCA: g_vel · g_level · calib (§4.8)
        L *= g;
        R *= g;
        has = true;
      }
      const double mono = stereo ? (L + R) * 0.7071067811865476 : L;
      voiceOut_[v] = mono;
      coreOut_[v] = active_[v] ? voices_[v]->core() : 0.0;
      scPeak[v] = std::fabs(mono);
      // OUT jack tap (post-VCA, pre-pan), one decimator per connected tap.
      const int op = isDrum(v) ? drumPort(v, DJ_OUT) : synthPort(v - LEAD, SJ_OUT);
      if (con && con[op]) {
        double o;
        if (decOut_[v].push(mono, o)) outTap_[v] = o;
      }
      if (retOn[v]) {
        L = R = retBuf_[v][sub];
        stereo = false;
        has = true;
      }
      if (!has || !audible[v]) continue;
      if (choking_[v]) {
        L *= chokeGain_[v];
        R *= chokeGain_[v];
        chokeGain_[v] *= chokeA_;
      }
      double pl, pr;
      if (stereo) {
        pl = L * panLg[v] * 1.4142135623730951;
        pr = R * panRg[v] * 1.4142135623730951;
      } else {
        pl = L * panLg[v];
        pr = L * panRg[v];
      }
      const int r = route[v];
      if (r == 0) {
        mL += pl;
        mR += pr;
      } else if (r <= 4) {
        bL[r - 1] += pl;
        bR[r - 1] += pr;
      } else if (r <= 12) {
        aL[r - 5] += pl;
        aR[r - 5] += pr;
      } else {
        const int a = kPairAux[v];
        const int side = kPairSide[v];
        if (side == 0) aL[a] += stereo ? 0.5 * (L + R) : L;
        else if (side == 1) aR[a] += stereo ? 0.5 * (L + R) : L;
        else {
          aL[a] += pl;
          aR[a] += pr;
        }
      }
      sL += pl * send[v];
      sR += pr * send[v];
    }
    for (int b = 0; b < 4; ++b) {
      if (!busUsed[b] && !bus_[b].compOn) continue;
      const double sc = busSc_[b] >= 0 ? scPeak[busSc_[b]] : -1.0;
      bus_[b].process(bL[b], bR[b], sc);
      mL += bL[b];
      mR += bR[b];
    }
    masterDrive_.process(mL, mR);
    if (glueOn_) {
      const double g = glue_.gain(std::fmax(std::fabs(mL), std::fabs(mR)));
      mL *= g;
      mR *= g;
    }
    width_.process(mL, mR);
    mL += fxBuf_[0][sub];
    mR += fxBuf_[1][sub];
    mL *= volume_;
    mR *= volume_;
    clip_.process(mL, mR);
    double o;
    if (decMain_[0].push(mL, o)) outMainL_ = o;
    if (decMain_[1].push(mR, o)) outMainR_ = o;
    for (int a = 0; a < 8; ++a) {
      if (!auxUsed_[a]) continue;
      if (decAux_[2 * a].push(aL[a], o)) outAux_[2 * a] = o;
      if (decAux_[2 * a + 1].push(aR[a], o)) outAux_[2 * a + 1] = o;
    }
    if (delayActive_) {
      if (decSend_[0].push(sL, o)) sendDec[0] = o;
      if (decSend_[1].push(sR, o)) sendDec[1] = o;
    }
  }
  for (int a = 0; a < 8; ++a)
    if (!auxUsed_[a]) outAux_[2 * a] = outAux_[2 * a + 1] = 0.0;

  // Delay at the base rate; its return re-enters the domain through upsamplers on the next base sample.
  if (delayActive_) {
    const int div = stepIndex(target_[P_FX_DELAY_TIME], 12);
    const double comp = 2.0 * latencySamples() + 1.0;
    const double tp = tempo();
    if (!(dsp::exactEq(dlyKey_[0], (double)div) && dsp::exactEq(dlyKey_[1], tp) && dsp::exactEq(dlyKey_[2], comp) && dsp::exactEq(dlyKey_[3], ue_[P_FX_DELAY_FB]) && dsp::exactEq(dlyKey_[4], ue_[P_FX_DELAY_LP]))) {
      dlyKey_[0] = div; dlyKey_[1] = tp; dlyKey_[2] = comp; dlyKey_[3] = ue_[P_FX_DELAY_FB]; dlyKey_[4] = ue_[P_FX_DELAY_LP];
      delay_.set(kDelayBeats[div] * 60.0 / tp * fs_ - comp, 0.9 * ue_[P_FX_DELAY_FB],
                 2000.0 * std::pow(6.0, ue_[P_FX_DELAY_LP]), fs_);
    }
    double dl, dr;
    delay_.process(sendDec[0], sendDec[1], dl, dr);
    delayEnergy_ = 0.999 * delayEnergy_ + dl * dl + dr * dr;
    const double ret = 2.0 * ue_[P_MASTER_FX_SEND];
    upFx_[0].push(dl * ret, fxBuf_[0]);
    upFx_[1].push(dr * ret, fxBuf_[1]);
  } else {
    for (auto& c : fxBuf_)
      for (double& x : c) x = 0.0;
  }
}

// Exact-silence check (see shogun.h). The signature holds what decides which states the pass would touch (processed
// buses, used aux pairs, connected OUT taps, glue on); a change re-runs the bit check before sleeping on.
bool Engine::canSleep(const bool* retOn, const bool* busUsed, const bool* con) {
  if (!sleepEnabled_ || delayActive_) return false;
  for (int v = 0; v < kVoices; ++v)
    if (active_[v] || retOn[v]) return false;
  std::uint64_t sig = glueOn_ ? 1u : 0u;
  for (int b = 0; b < 4; ++b)
    if (busUsed[b] || bus_[b].compOn) sig |= std::uint64_t{2} << b;
  for (int a = 0; a < 8; ++a)
    if (auxUsed_[a]) sig |= std::uint64_t{1} << (5 + a);
  for (int v = 0; v < kVoices; ++v) {
    const int op = isDrum(v) ? drumPort(v, DJ_OUT) : synthPort(v - LEAD, SJ_OUT);
    if (con && con[op]) sig |= std::uint64_t{1} << (13 + v);
  }
  if (sleepVerified_ && sig == sleepSig_) return true;
  using dsp::bitsOf;
  auto z = [](double x) { return bitsOf(x) == 0; };
  bool ok = z(outMainL_) && z(outMainR_) && decMain_[0].silent() && decMain_[1].silent();
  for (int c = 0; c < 2 && ok; ++c)
    for (double x : fxBuf_[c]) ok = ok && z(x);
  for (int v = 0; v < kVoices && ok; ++v) ok = z(voiceOut_[v]) && z(coreOut_[v]);
  ok = ok && z(masterDrive_.l.xp) && z(masterDrive_.r.xp) && z(width_.sLp.s) && z(clip_.l.xp) && z(clip_.r.xp);
  if (glueOn_) ok = ok && z(glue_.yL);
  for (int b = 0; b < 4 && ok; ++b) {
    if (!(sig & (std::uint64_t{2} << b))) continue;
    const mix::Bus& bus = bus_[b];
    ok = z(bus.drive.l.xp) && z(bus.drive.r.xp) && z(bus.tilt.l.s) && z(bus.tilt.r.s) && (!bus.compOn || z(bus.comp.yL));
  }
  for (int a = 0; a < 8 && ok; ++a)
    if (auxUsed_[a]) ok = z(outAux_[2 * a]) && z(outAux_[2 * a + 1]) && decAux_[2 * a].silent() && decAux_[2 * a + 1].silent();
  for (int v = 0; v < kVoices && ok; ++v)
    if (sig & (std::uint64_t{1} << (13 + v))) ok = z(outTap_[v]) && decOut_[v].silent();
  sleepVerified_ = ok;
  sleepSig_ = sig;
  return ok;
}

void Engine::writeControlOutputs(float* out) {
  // Audio taps (decimated) and MIX.
  for (int v = 0; v < kVoices; ++v) {
    const int op = isDrum(v) ? drumPort(v, DJ_OUT) : synthPort(v - LEAD, SJ_OUT);
    out[op] = static_cast<float>(5.0 * outTap_[v]);
  }
  out[PORT_MIX_L] = static_cast<float>(5.0 * outMainL_);
  out[PORT_MIX_R] = static_cast<float>(5.0 * outMainR_);
  // Control outputs: base rate, written at the trigger sample (zero latency, §13.10).
  for (int v = 0; v < kDrumVoices; ++v)
    out[drumPort(v, DJ_ENV)] = active_[v] ? static_cast<float>(5.0 * voices_[v]->env() * hitGain_[v]) : 0.0f;
  out[synthPort(0, SJ_NOTE_OUT)] = static_cast<float>(lead_->noteOut());
  out[synthPort(1, SJ_NOTE_OUT)] = static_cast<float>(bass_->noteOut());
  out[PORT_LD_GATE] = lead_->gate ? 5.0f : 0.0f;
  out[PORT_BS_GATE] = bass_->gate ? 5.0f : 0.0f;
  const bool run = wasRunning_;
  const int gScale = stepIndex(target_[P_CLOCK_SCALE], 4);
  const int clkMode = stepIndex(target_[P_CLOCK_CLK_OUT], 6);
  const double rate = clkMode == 0 ? stepsPerQuarter(gScale) : kPpqn[clkMode];
  const double ph = ppq_ * rate;
  out[PORT_CLK_OUT] = (run && ppq_ >= 0.0 && ph - std::floor(ph + 1e-9) < 0.5 - 1e-9) ? 5.0f : 0.0f;
  out[PORT_RST_OUT] = (run && rstPulse_ > 0) ? 5.0f : 0.0f;
  if (rstPulse_ > 0) --rstPulse_;
  const bool pulseMode = stepIndex(target_[P_CLOCK_RUN_OUT], 2) == 1;
  out[PORT_RUN_OUT] = pulseMode ? (runPulse_ > 0 ? 5.0f : 0.0f) : (run ? 5.0f : 0.0f);
  if (runPulse_ > 0) --runPulse_;
  out[PORT_ACC_OUT] = run ? static_cast<float>(accOut_) : 0.0f;
  for (int i = 0; i < 4; ++i) out[PORT_LFO1 + i] = static_cast<float>(mod_.lfo[i].jackVolts());
  out[PORT_RND] = static_cast<float>(5.0 * rnd_);
}

}  // namespace shogun
